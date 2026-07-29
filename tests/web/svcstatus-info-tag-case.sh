#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/web/svcstatus-info-tag-case.sh
#
# Reported against production: the per-host info page sometimes seems to be
# missing/duplicating tags, and it looked case-related. Traced to two
# distinct bugs in lib/loadhosts.c's xmh_item_idx() (what web/svcstatus-
# info.c's "Other tags:" row uses to decide a raw tag is already a
# recognized attribute, and so should not be echoed again):
#
#  1. Array truncation, independent of case: xmh_item_idx() scans
#     xmh_item_key[] from index 0 and stops at the first NULL slot (by
#     design -- see the self-check at lib/loadhosts.c:~217). Ten keys
#     (CLASS:, OS:, DOC:, NOPROP:, NOCOLUMNS:, NOTBEFORE:, NOTAFTER:,
#     COMPACT:, INTERFACES:, ACCEPTONLY:) were added to enum xmh_item_t
#     AFTER that stopping point (XMH_IP), so they can never be recognized,
#     in any case.
#  2. Case sensitivity, for the keys that ARE reachable: xmh_find_item()
#     (backs every xmh_item(host, XMH_COMMENT/...) lookup) matches
#     case-insensitively, but xmh_item_idx() matches case-sensitively.
#
# See svcstatus-info-tag-case-harness.c for the full writeup and line
# references.
#
# NEITHER BUG IS FIXED -- pending a maintainer decision on the right fix for
# each. This test documents both for that discussion and is expected to
# fail until they land; it is deliberately not wired to `pass` on the
# current, inconsistent behavior.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
here=$(dirname "$0")

CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || skip "no C compiler available (CC=$CC)"

[ -f "$ROOT/include/config.h" ] && [ -f "$ROOT/lib/libxymoncomm.a" ] \
	|| skip "tree not built (run make first; the post-build CI suite covers this)"

work=$(mktemp -d "${TMPDIR:-/tmp}/xymon-svcstatus-info-tagcase.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

make -C "$ROOT/lib" libxymoncomm.a >"$work/libbuild.log" 2>&1 \
	|| { cat "$work/libbuild.log" >&2; fail "cannot refresh libxymoncomm.a"; }

cat > "$work/hosts.cfg" <<'EOF'
127.0.0.1 classhost.example.com # conn CLASS:web
127.0.0.1 commentcanon.example.com # conn COMMENT:hello
127.0.0.1 commentlower.example.com # conn comment:hello
EOF

"$CC" -I"$ROOT/include" -I"$ROOT/lib" -o "$work/harness" \
	"$here/svcstatus-info-tag-case-harness.c" "$ROOT/lib/libxymoncomm.a" \
	-lssl -lcrypto 2>"$work/cc.log" \
	|| { cat "$work/cc.log" >&2; fail "harness does not compile"; }

"$work/harness" "$work/hosts.cfg" 2>"$work/stderr.log" \
	|| fail "xmh_item_idx() bugs reproduced (expected until fixed): $(cat "$work/stderr.log")"

pass "xmh_item_idx() recognizes every reserved hosts.cfg tag key regardless of case"

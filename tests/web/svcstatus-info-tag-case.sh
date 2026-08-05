#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/web/svcstatus-info-tag-case.sh
#
# Reported against production: the per-host info page sometimes seems to be
# missing/duplicating tags, and it looked case-related. Investigating turned
# up four related defects in lib/loadhosts.c's reserved-tag key table.
# Three trace to xmh_item_idx(), which answers "is this raw hosts.cfg tag a
# recognized reserved attribute?" for both web/svcstatus-info.c's "Other
# tags:" row and xymonnet/xymonnet.c:492:
#
#  1. Key table truncation, independent of case: xmh_item_idx() scans
#     xmh_item_key[] from index 0 and stops at the first NULL slot (by
#     design -- see the self-check at lib/loadhosts.c:~217). Ten keys
#     (DOC:, NOPROP:, ACCEPTONLY:, CLASS:, OS:, NOCOLUMNS:, NOTBEFORE:,
#     NOTAFTER:, COMPACT:, INTERFACES:) were added to enum xmh_item_t AFTER
#     that stopping point (XMH_IP), so they can never be recognized, in any
#     case.
#  2. Case sensitivity, for the keys that ARE reachable: xmh_find_item()
#     (backs every xmh_item(host, XMH_COMMENT/...) lookup) matches
#     case-insensitively, but xmh_item_idx() matches case-sensitively. There
#     is no single convention to follow either -- the table mixes
#     upper-case keys (NET:, COMMENT:) with lower-case ones (ssldays=,
#     prefer), as does hosts.cfg(5) itself.
#  3. Record corruption via the same misclassification: because the ten
#     bug-1 keys look unrecognized, xymonnet accepts them as test specs;
#     all ten contain ':', so they reach xymonnet's "Simple TCP connect
#     test" branch, which splits the spec IN PLACE -- inside the host
#     record's own allelems buffer -- destroying the tag's value.
#  4. Latent: xmh_item_name[XMH_FLAG_MULTIHOMED] reads "XMH_MULTIHOMED"
#     (missing FLAG_), so xmh_item_isflag[] never marks it a flag and
#     xmh_item() returns "" instead of the canonical key. Its only consumer
#     tests == NULL, and "" is non-NULL, so nothing breaks today.
#
# See svcstatus-info-tag-case-harness.c for the full writeup and line
# references.
#
# NONE OF THESE ARE FIXED -- pending a maintainer decision on the right fix
# for each. The harness is written fix-forward: each assertion states the
# desired end state. It skips until all production fixes are present, then
# becomes a strict regression guard for the combined behavior.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
here=$(dirname "$0")

loadhosts_c="$ROOT/lib/loadhosts.c"
item_idx_body=$(awk '/^int xmh_item_idx/,/^}/' "$loadhosts_c")
if [[ $item_idx_body == *'while (xmh_item_key[i] &&'* ]] \
	|| [[ $item_idx_body == *'strncmp('* ]] \
	|| grep -Fq '"XMH_MULTIHOMED";' "$loadhosts_c"
then
	skip "reserved-tag key table fixes are not all present"
fi

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
127.0.0.1 mutatehost.example.com # conn CLASS:web
127.0.0.1 flaghost.example.com # conn dialup MULTIHOMED
EOF

"$CC" -I"$ROOT/include" -I"$ROOT/lib" -o "$work/harness" \
	"$here/svcstatus-info-tag-case-harness.c" "$ROOT/lib/libxymoncomm.a" \
	-lssl -lcrypto 2>"$work/cc.log" \
	|| { cat "$work/cc.log" >&2; fail "harness does not compile"; }

"$work/harness" "$work/hosts.cfg" 2>"$work/stderr.log" \
	|| fail "reserved-tag key table defects reproduced (expected until fixed):
$(cat "$work/stderr.log")"

pass "reserved hosts.cfg tag keys are recognized regardless of case, survive xymonnet, and register as flags"

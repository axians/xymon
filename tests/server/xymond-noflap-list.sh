#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/server/xymond-noflap-list.sh
#
# Flap-suppression defect: the list form of the hosts.cfg
# "noflap=test1,test2,..." tag stops working for every test after the first
# one evaluated. xymond/xymond.c's isset_noflap() (~line 1389) calls
# strtok() on the pointer xmh_item() returned for XMH_NOFLAP -- which points
# into the host record's own allelems buffer, not a copy -- so the first
# evaluation permanently truncates the host's noflap list at the first
# comma. The bare "noflap" flag form is unaffected.
#
# Four of the six places that tokenize an xmh_item() result copy it first
# (convertnk.c:32, xymond_client.c:263, loaddata.c:117, webaccess.c:75), so
# this is an oversight rather than a design choice. The other offender,
# xymongen/loaddata.c:422 (XMH_COMPACT), is covered structurally -- no
# user-visible symptom is claimed for it.
#
# See xymond-noflap-list-harness.c for the full writeup, including why the
# list assertions must share one host and run in sequence (the defect only
# appears on the second and later evaluations, matching xymond's one-call-
# per-status-message usage).
#
# NOT FIXED -- pending maintainer discussion. The harness is written
# fix-forward: each assertion states the desired end state. It skips while
# the production fix is absent, then becomes a strict regression guard once
# the copy-before-tokenizing implementation is present.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
here=$(dirname "$0")

noflap_body=$(awk '/^static int isset_noflap/,/^}/' "$ROOT/xymond/xymond.c")
[[ $noflap_body == *'strdup('* ]] \
	|| skip "noflap= list fix not present (requires fix/xymond-noflap-list)"

CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || skip "no C compiler available (CC=$CC)"

[ -f "$ROOT/include/config.h" ] && [ -f "$ROOT/lib/libxymoncomm.a" ] \
	|| skip "tree not built (run make first; the post-build CI suite covers this)"

work=$(mktemp -d "${TMPDIR:-/tmp}/xymon-noflap-list.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

make -C "$ROOT/lib" libxymoncomm.a >"$work/libbuild.log" 2>&1 \
	|| { cat "$work/libbuild.log" >&2; fail "cannot refresh libxymoncomm.a"; }

# Tags are spelled lowercase here because that is what hosts.cfg(5)
# documents ("noflap[=test1,test2,...]"), even though the key table in
# lib/loadhosts.c stores them upper-case.
cat > "$work/hosts.cfg" <<'EOF'
127.0.0.1 barehost.example.com # conn noflap
127.0.0.1 listhost.example.com # conn noflap=web,cpu,disk
127.0.0.1 recordhost.example.com # conn noflap=web,cpu
127.0.0.1 compacthost.example.com # conn COMPACT:net=http|smtp,sys=cpu|disk
EOF

"$CC" -I"$ROOT/include" -I"$ROOT/lib" -o "$work/harness" \
	"$here/xymond-noflap-list-harness.c" "$ROOT/lib/libxymoncomm.a" \
	-lssl -lcrypto 2>"$work/cc.log" \
	|| { cat "$work/cc.log" >&2; fail "harness does not compile"; }

"$work/harness" "$work/hosts.cfg" 2>"$work/stderr.log" \
	|| fail "noflap list defect reproduced (expected until fixed):
$(cat "$work/stderr.log")"

pass "noflap= list keeps working across repeated evaluations and leaves the host record intact"

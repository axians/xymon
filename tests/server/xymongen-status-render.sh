#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/server/xymongen-status-render.sh
#
# Behavioural test of the BUILT xymongen binary: rendering per-host status
# pages from a hosts.cfg plus a status-board snapshot, with no running
# xymond needed. xymongen supports reading the board from a static dump
# file via the BOARDDUMP env var instead of querying a live server (see
# xymongen/loaddata.c, load_state()) -- an intentional debugging hook that
# also makes this fully hermetic.
#
# Three hosts, two tags, one ungrouped host in front of a named "group":
#   solo.example.com  # conn        (ungrouped -- appears before the "group" line)
#   www.example.com   # conn http   (conn green, http red)
#   db.example.com    # conn        (conn green, no http tag at all)
#
# Asserts three distinct pieces of xymongen behaviour:
#   - the main page (xymon.html) renders exactly the tags each host has
#     (hosts.cfg tag -> column mapping), with the color from the board
#     dump, not from hosts.cfg;
#   - a "group" directive renders a named group block, without losing the
#     ungrouped host that precedes it on the same page (loadlayout.c only
#     resets the current group on a new page/subpage/subparent line, never
#     back to "no group" mid-page -- so hosts before the first "group" line
#     are the only way to get an ungrouped block on a page that also has
#     named groups);
#   - the nongreen page (nongreen.html) drops fully-green hosts and
#     fully-green columns entirely, not just recolors them -- db.example.com
#     (all green) must not appear on it at all, and neither should a
#     "conn" column header, since conn is green on every host that has it.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

require_bin XYMONGEN xymongen/xymongen

work=$(mktempdir)
mkdir -p "$work"/ack "$work"/hist "$work"/histlogs "$work"/home/web \
	"$work"/rawstatus "$work"/notes "$work"/rep "$work"/tmp "$work"/var "$work"/web

# Empty header/footer files are enough to satisfy xymongen's file-exists
# check and keep the "missing header" fallback banner out of the output,
# so assertions below only have to deal with the real page content.
for f in stdnormal_header stdnormal_footer stdnongreen_header stdnongreen_footer \
	stdcritical_header stdcritical_footer; do
	: > "$work/home/web/$f"
done

cat > "$work/hosts.cfg" <<'EOF'
127.0.0.1 solo.example.com # conn
group Example Hosts
127.0.0.1 www.example.com # conn http
127.0.0.1 db.example.com  # conn
EOF

# hostname|testname|color|testflags|lastchange|logtime|validtime|acktime|disabletime|sender|cookie|msg
cat > "$work/board.dump" <<'EOF'
solo.example.com|conn|green|||||||127.0.0.1|-1|OK
www.example.com|conn|green|||||||127.0.0.1|-1|OK
www.example.com|http|red|||||||127.0.0.1|-1|Connection refused
db.example.com|conn|green|||||||127.0.0.1|-1|OK
EOF

export XYMONACKDIR="$work/ack"
export XYMONHISTDIR="$work/hist"
export XYMONHISTLOGS="$work/histlogs"
export XYMONHOME="$work/home"
export HOSTSCFG="$work/hosts.cfg"
export XYMONRAWSTATUSDIR="$work/rawstatus"
export XYMONLOGSTATUS=DYNAMIC
export XYMONNOTESDIR="$work/notes"
export XYMONREPDIR="$work/rep"
export XYMONREPURL="/rep"
export XYMONSKIN=default
export XYMONTMP="$work/tmp"
export XYMONVAR="$work/var"
export XYMONWEB="$work/web"
export XYMONWWWDIR="$work/web"
export XYMONWEBHOST="http://localhost"
export XYMONWEBHOSTURL=""
export CGIBINURL="/cgi-bin"
export DOTHEIGHT=16
export DOTWIDTH=16
export MACHINE=localhost
export MACHINEADDR=127.0.0.1
export XYMONPAGECOLFONT=""
export XYMONPAGELOCAL=""
export XYMONPAGESUBLOCAL=""
export XYMONPAGEREMOTE=""
export XYMONPAGEROWFONT=""
export XYMONPAGETITLE="Test"
export PURPLEDELAY=0
export BOARDDUMP="$work/board.dump"

"$XYMONGEN" >"$work/xymongen.out" 2>"$work/xymongen.err" || {
	cat "$work/xymongen.out" >&2
	cat "$work/xymongen.err" >&2
	fail "xymongen exited non-zero"
}

assert_file_exists "$work/web/xymon.html" "main page was not generated"
main=$(cat "$work/web/xymon.html")

assert_contains 'solo.example.com' "$main" "main page must list the ungrouped solo.example.com"
assert_contains 'www.example.com' "$main" "main page must list www.example.com"
assert_contains 'db.example.com' "$main" "main page must list db.example.com"
assert_contains '<A NAME="group-Example_Hosts">' "$main" \
	"the 'group' directive must render a named group anchor"
assert_contains 'ALT="http:red:"' "$main" "www.example.com's failed http test must render red"
# All three hosts have "conn", all green in the board dump -- exactly
# three green conn dots (one ungrouped, two inside the named group), and
# no red/other color for it.
[ "$(grep -Fc 'ALT="conn:green:"' <<<"$main")" = 3 ] ||
	fail "expected exactly three green conn indicators, one per host"
# db.example.com has no "http" tag in hosts.cfg at all: xymongen must not
# invent a status for it just because another host on the same page has
# that column.
assert_not_contains 'ALT="http:green:"' "$main" \
	"db.example.com has no http tag; there must be no green http indicator"

assert_file_exists "$work/web/nongreen.html" "nongreen page was not generated"
nongreen=$(cat "$work/web/nongreen.html")

assert_contains 'www.example.com' "$nongreen" \
	"nongreen page must list www.example.com (it has a red test)"
assert_contains 'ALT="http:red:"' "$nongreen" "nongreen page must show the red http test"
assert_not_contains 'db.example.com' "$nongreen" \
	"nongreen page must drop db.example.com entirely -- it has no non-green tests"
assert_not_contains 'solo.example.com' "$nongreen" \
	"nongreen page must drop the ungrouped solo.example.com too -- it is fully green"
assert_not_contains '>conn<' "$nongreen" \
	"nongreen page must drop the conn column header -- conn is green on every host that has it"

pass "xymongen renders board-dump status colors and nongreen filtering correctly"

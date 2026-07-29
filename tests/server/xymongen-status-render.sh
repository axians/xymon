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
# Every host also carries "info", "trends" and "clientlog" -- in real
# deployments these three pseudo-tests exist on essentially every node
# (added by xymongen/the client, not something an operator tags in
# hosts.cfg), so a fixture without them tests an unrealistic shape of data.
#
# Asserts four distinct pieces of xymongen behaviour:
#   - the main page (xymon.html) renders a column for every test *present in
#     the board dump for that host* -- this is driven entirely by the board
#     data, not by hosts.cfg tags: db.example.com has no "http" tag in
#     hosts.cfg, but the real reason it shows no http indicator is that its
#     board-dump entry has no "http" line at all. (Verified separately: a
#     host with a board entry for a test it has no hosts.cfg tag for still
#     renders that column -- hosts.cfg tags govern host/page/group
#     placement, not which columns can appear.)
#   - a "group" directive renders a named group block, without losing the
#     ungrouped host that precedes it on the same page (loadlayout.c only
#     resets the current group on a new page/subpage/subparent line, never
#     back to "no group" mid-page -- so hosts before the first "group" line
#     are the only way to get an ungrouped block on a page that also has
#     named groups);
#   - the nongreen page (nongreen.html) drops fully-green hosts and
#     fully-green columns entirely, not just recolors them -- db.example.com
#     (all green) must not appear on it at all, and neither should a
#     "conn" column header, since conn is green on every host that has it;
#   - "info"/"trends"/"clientlog" are always rendered for a host that's
#     already on the nongreen page for some other reason, regardless of
#     their own color (pagegen.c: "CLIENT, TRENDS and INFO columns are
#     always included on non-Xymon pages") -- but, unlike a genuinely
#     nongreen test, they don't by themselves put a host on the page:
#     db.example.com and solo.example.com have the same green
#     info/trends/clientlog as www.example.com, yet only www.example.com
#     (which also has a real red test) appears.

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
solo.example.com|info|green|||||||127.0.0.1|-1|Host info
solo.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
solo.example.com|clientlog|green|||||||127.0.0.1|-1|Client data
www.example.com|conn|green|||||||127.0.0.1|-1|OK
www.example.com|http|red|||||||127.0.0.1|-1|Connection refused
www.example.com|info|green|||||||127.0.0.1|-1|Host info
www.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
www.example.com|clientlog|green|||||||127.0.0.1|-1|Client data
db.example.com|conn|green|||||||127.0.0.1|-1|OK
db.example.com|info|green|||||||127.0.0.1|-1|Host info
db.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
db.example.com|clientlog|green|||||||127.0.0.1|-1|Client data
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
# db.example.com's board dump has no "http" line at all (not merely "no
# http tag in hosts.cfg" -- xymongen renders whatever tests are present in
# the board dump for a host, regardless of hosts.cfg tags): xymongen must
# not invent a status for it just because another host on the same page
# has that column.
assert_not_contains 'ALT="http:green:"' "$main" \
	"db.example.com's board dump has no http entry; there must be no green http indicator"

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
# www.example.com is on the page (for its red http), so its green
# info/trends/clientlog must still render -- these three are always shown
# for a host that's already included, regardless of their own color.
assert_contains 'ALT="info:green:' "$nongreen" \
	"info must render for www.example.com even though info itself is green"
assert_contains 'ALT="trends:green:' "$nongreen" \
	"trends must render for www.example.com even though trends itself is green"
assert_contains 'ALT="clientlog:green:' "$nongreen" \
	"clientlog must render for www.example.com even though clientlog itself is green"

pass "xymongen renders board-dump status colors and nongreen filtering correctly"

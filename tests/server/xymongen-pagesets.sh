#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/server/xymongen-pagesets.sh
#
# Behavioural test of xymongen's page hierarchy ("page"/"subpage") and
# vertical pages ("vpage") -- hosts.cfg(5) directives that partition hosts
# across separate generated pages, and, for vpage, transpose the table
# (rows become tests, columns become hosts -- see pagegen.c's
# do_vertical(), documented for "a very large number of tests for a few
# hosts").
#
# Layout:
#   page siteA         -> a1 (green), a2 (red)
#   subpage siteA-sub   -> a3 (green)
#   page siteB         -> solo-b (green, ungrouped), then group-compress
#                          "Backend" -> b1 (green)
#   vpage vertgroup     -> vhost1 (conn green, cpu red, disk green)
#                          vhost2 (conn green, cpu green, disk yellow)
#
# siteB also covers a "group-compress" directive (the other spelling of
# "group", covered in xymongen-status-render.sh -- hosts.cfg(5) documents
# the two as handled identically) coexisting with an ungrouped host on the
# same page, and combined with the page/subpage hierarchy under test here.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

require_bin XYMONGEN xymongen/xymongen

work=$(mktempdir)
mkdir -p "$work"/ack "$work"/hist "$work"/histlogs "$work"/home/web \
	"$work"/rawstatus "$work"/notes "$work"/rep "$work"/tmp "$work"/var "$work"/web
for f in stdnormal_header stdnormal_footer stdnongreen_header stdnongreen_footer \
	stdcritical_header stdcritical_footer; do
	: > "$work/home/web/$f"
done

cat > "$work/hosts.cfg" <<'EOF'
page siteA Site A
127.0.0.1 a1.example.com # conn
127.0.0.1 a2.example.com # conn

subpage siteA-sub Site A Subpage
127.0.0.1 a3.example.com # conn

page siteB Site B
127.0.0.1 solo-b.example.com # conn
group-compress Backend
127.0.0.1 b1.example.com # conn

vpage vertgroup Vertical Test Page
127.0.0.1 vhost1.example.com # conn cpu disk
127.0.0.1 vhost2.example.com # conn cpu disk
EOF

cat > "$work/board.dump" <<'EOF'
a1.example.com|conn|green|||||||127.0.0.1|-1|OK
a1.example.com|info|green|||||||127.0.0.1|-1|Host info
a1.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
a2.example.com|conn|red|||||||127.0.0.1|-1|Down
a2.example.com|info|green|||||||127.0.0.1|-1|Host info
a2.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
a3.example.com|conn|green|||||||127.0.0.1|-1|OK
a3.example.com|info|green|||||||127.0.0.1|-1|Host info
a3.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
solo-b.example.com|conn|green|||||||127.0.0.1|-1|OK
solo-b.example.com|info|green|||||||127.0.0.1|-1|Host info
solo-b.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
b1.example.com|conn|green|||||||127.0.0.1|-1|OK
b1.example.com|info|green|||||||127.0.0.1|-1|Host info
b1.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
vhost1.example.com|conn|green|||||||127.0.0.1|-1|OK
vhost1.example.com|cpu|red|||||||127.0.0.1|-1|Load high
vhost1.example.com|disk|green|||||||127.0.0.1|-1|OK
vhost1.example.com|info|green|||||||127.0.0.1|-1|Host info
vhost1.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
vhost2.example.com|conn|green|||||||127.0.0.1|-1|OK
vhost2.example.com|cpu|green|||||||127.0.0.1|-1|OK
vhost2.example.com|disk|yellow|||||||127.0.0.1|-1|Getting full
vhost2.example.com|info|green|||||||127.0.0.1|-1|Host info
vhost2.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
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

assert_file_exists "$work/web/siteA/siteA.html"
assert_file_exists "$work/web/siteA/siteA-sub/siteA-sub.html"
assert_file_exists "$work/web/siteB/siteB.html"
assert_file_exists "$work/web/vertgroup/vertgroup.html"

siteA=$(cat "$work/web/siteA/siteA.html")
siteAsub=$(cat "$work/web/siteA/siteA-sub/siteA-sub.html")
siteB=$(cat "$work/web/siteB/siteB.html")

assert_contains 'a1.example.com' "$siteA" "siteA must list its own host a1"
assert_contains 'a2.example.com' "$siteA" "siteA must list its own host a2"
assert_not_contains 'a3.example.com' "$siteA" "siteA must not list the subpage's host a3"
assert_not_contains 'b1.example.com' "$siteA" "siteA must not list siteB's host"

assert_contains 'a3.example.com' "$siteAsub" "siteA-sub must list its own host a3"
assert_not_contains 'a1.example.com' "$siteAsub" "siteA-sub must not list the parent page's hosts"

assert_contains 'solo-b.example.com' "$siteB" "siteB must list its ungrouped host"
assert_contains 'b1.example.com' "$siteB" "siteB must list its grouped host b1"
assert_contains '<A NAME="group-Backend">' "$siteB" \
	"the 'group-compress' directive must render a named group anchor"
assert_not_contains 'a1.example.com' "$siteB" "siteB must not list siteA's hosts"

# Vertical page: rows are tests, columns are hosts -- the opposite of every
# other page above. Assert the transpose directly: each test name renders
# as a row label ("<td ...>testname</td>"), which never happens on a
# normal page (there, hostnames are the row labels, testnames are column
# headers alongside a "columndoc.sh?testname" link -- see the other tests
# in this area).
vert=$(cat "$work/web/vertgroup/vertgroup.html")
for test in conn cpu disk; do
	assert_contains "align=left>$test</td>" "$vert" \
		"vpage must render '$test' as a row label, not a column header"
done
assert_contains 'vhost1.example.com' "$vert" "vpage must list vhost1 as a column"
assert_contains 'vhost2.example.com' "$vert" "vpage must list vhost2 as a column"
# The transpose is what makes a single green cell identifiable by which
# host+test it belongs to without row/column ambiguity: vhost1's disk is
# green and vhost2's cpu is green, but vhost1's cpu is red and vhost2's
# disk is yellow -- if rows and columns were swapped back to normal, this
# exact color pairing could not appear.
assert_contains 'ALT="cpu:red:"' "$vert" "vhost1's cpu must render red"
assert_contains 'ALT="disk:yellow:"' "$vert" "vhost2's disk must render yellow"

pass "xymongen page/subpage hierarchy and vpage transpose are both correct"

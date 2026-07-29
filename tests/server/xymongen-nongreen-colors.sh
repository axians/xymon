#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/server/xymongen-nongreen-colors.sh
#
# Behavioural test of xymongen's nongreen-page filtering across every
# status color, with enough hosts to separate two rules that are easy to
# conflate: which COLUMNS appear on the page, and which HOSTS appear.
#
#   - A column header appears if *any* host has that test in a nongreen
#     color (COL_RED/YELLOW/PURPLE -- see pagegen.c's nongreencolors),
#     anywhere on the page -- not per-host.
#   - A host's row appears if *that host's own* worst color is nongreen --
#     independent of why the column exists. This means a host can show a
#     genuinely green cell in a column that only exists because some other
#     host has it in a bad color (multi.example.com's green "conn" below,
#     which only has a column at all because purple-host.example.com's
#     conn is purple).
#   - CLEAR is deliberately not a "nongreen" color in xymongen's default
#     nongreencolors bitmask, unlike RED/YELLOW/PURPLE -- a fully-clear
#     host must be excluded exactly like a fully-green one.
#   - A column that is green on every host that has it (here: "memory",
#     used only by multi.example.com) must not appear as a column at all.
#
# green-host.example.com is deliberately left ungrouped (listed before the
# "group-compress" line) alongside the rest in a named group, on the same
# page -- and "group-compress" specifically, to cover both group directive
# spellings across this test area ("group" is covered in
# xymongen-status-render.sh; hosts.cfg(5) documents the two as handled
# identically).
#
# Every host also carries green "info"/"trends" (present on essentially
# every real node -- see xymongen-status-render.sh for the dedicated
# assertions on that pair). Here they mainly prove the inclusion rules
# above hold even in their presence: green-host and clear-host both have
# green info/trends same as everyone else, yet still correctly stay off
# this page entirely -- info/trends never independently qualify a host.

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
127.0.0.1 green-host.example.com    # conn
group-compress Mixed Status Hosts
127.0.0.1 yellow-host.example.com   # conn disk
127.0.0.1 red-host.example.com      # conn cpu
127.0.0.1 purple-host.example.com   # conn
127.0.0.1 clear-host.example.com    # conn
127.0.0.1 multi.example.com         # conn disk cpu memory
EOF

cat > "$work/board.dump" <<'EOF'
green-host.example.com|conn|green|||||||127.0.0.1|-1|OK
green-host.example.com|info|green|||||||127.0.0.1|-1|Host info
green-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
yellow-host.example.com|conn|green|||||||127.0.0.1|-1|OK
yellow-host.example.com|disk|yellow|||||||127.0.0.1|-1|Getting full
yellow-host.example.com|info|green|||||||127.0.0.1|-1|Host info
yellow-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
red-host.example.com|conn|green|||||||127.0.0.1|-1|OK
red-host.example.com|cpu|red|||||||127.0.0.1|-1|Load high
red-host.example.com|info|green|||||||127.0.0.1|-1|Host info
red-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
purple-host.example.com|conn|purple|||||||127.0.0.1|-1|No data
purple-host.example.com|info|green|||||||127.0.0.1|-1|Host info
purple-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
clear-host.example.com|conn|clear|||||||127.0.0.1|-1|Dialup down
clear-host.example.com|info|green|||||||127.0.0.1|-1|Host info
clear-host.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
multi.example.com|conn|green|||||||127.0.0.1|-1|OK
multi.example.com|disk|yellow|||||||127.0.0.1|-1|Getting full
multi.example.com|cpu|red|||||||127.0.0.1|-1|Load high
multi.example.com|memory|green|||||||127.0.0.1|-1|OK
multi.example.com|info|green|||||||127.0.0.1|-1|Host info
multi.example.com|trends|green|||||||127.0.0.1|-1|Trend graphs
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

assert_file_exists "$work/web/xymon.html"
main=$(cat "$work/web/xymon.html")
assert_contains 'green-host.example.com' "$main" "main page must list the ungrouped host"
assert_contains '<A NAME="group-Mixed_Status_Hosts">' "$main" \
	"the 'group-compress' directive must render a named group anchor"

assert_file_exists "$work/web/nongreen.html"
nongreen=$(cat "$work/web/nongreen.html")

for host in yellow-host.example.com red-host.example.com purple-host.example.com \
	multi.example.com; do
	assert_contains "$host" "$nongreen" "$host has a nongreen test and must appear"
done
for host in green-host.example.com clear-host.example.com; do
	assert_not_contains "$host" "$nongreen" \
		"$host is fully green/clear and must be dropped entirely"
done

for col in '>conn<' '>cpu<' '>disk<'; do
	assert_contains "$col" "$nongreen" \
		"column $col must appear -- at least one host has it in a nongreen color"
done
assert_not_contains '>memory<' "$nongreen" \
	"memory is green on every host that has it and must not appear as a column"

# multi.example.com's own conn is green; the conn column only exists
# because purple-host.example.com's conn is purple. Both must be true at
# once: the column shows, and multi's genuinely-green cell within it
# renders green rather than being hidden or miscolored.
assert_contains 'ALT="conn:green:"' "$nongreen" \
	"multi.example.com's green conn must render green inside the (otherwise-qualifying) conn column"

pass "xymongen nongreen filtering is correct per-column and per-host across all colors"

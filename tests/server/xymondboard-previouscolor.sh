#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Behavioural guard for the xymondboard oldcolor and previouscolor fields.
# oldcolor tracks the previous report; previouscolor tracks the color before
# the latest actual color transition and survives checkpoint restart.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)

require_bin XYMOND xymond/xymond
require_bin XYMONCLIENT common/xymon
require_shm_segments 10

work=$(mktempdir)
XYMOND_PID=""

stop_xymond() {
	[ -n "$XYMOND_PID" ] || return 0
	kill "$XYMOND_PID" 2>/dev/null || true
	local i=0
	while kill -0 "$XYMOND_PID" 2>/dev/null && [ "$i" -lt 100 ]; do
		sleep 0.1
		i=$((i+1))
	done
	XYMOND_PID=""
}
register_cleanup stop_xymond

printf 'page test Test\n127.0.0.1 testhost.example.com # conn\n' > "$work/hosts.cfg"
mkdir -p "$work/home/etc" "$work/home/tmp" "$work/home/www"
sed -e 's|^XYMONHOME=.*|XYMONHOME="'"$work"'/home"|' \
    -e 's|^XYMONTMP=.*|XYMONTMP="'"$work"'/home/tmp"|' \
	"$ROOT/xymond/etcfiles/xymonserver.cfg" > "$work/xymonserver.cfg" \
	|| skip "no xymonserver.cfg to run against"

free_port() {
	local port tries=0
	while [ "$tries" -lt 50 ]; do
		port=$((20000 + (RANDOM % 20000)))
		"$XYMONCLIENT" "127.0.0.1:$port" "ping" >/dev/null 2>&1 || { printf '%s' "$port"; return 0; }
		tries=$((tries+1))
	done
	return 1
}

start_xymond() {
	local i=0
	PORT=$(free_port) || fail "no free port for xymond"
	"$XYMOND" --no-daemon --listen="127.0.0.1:$PORT" \
		--hosts="$work/hosts.cfg" --env="$work/xymonserver.cfg" \
		--pidfile="$work/xymond.pid" --checkpoint-file="$work/chk" "$@" \
		> "$work/xymond.log" 2>&1 &
	XYMOND_PID=$!

	while [ "$i" -lt 100 ]; do
		"$XYMONCLIENT" "127.0.0.1:$PORT" "ping" >/dev/null 2>&1 && return 0
		kill -0 "$XYMOND_PID" 2>/dev/null || { cat "$work/xymond.log" >&2; fail "xymond exited during startup"; }
		sleep 0.1
		i=$((i+1))
	done
	cat "$work/xymond.log" >&2
	fail "xymond did not answer on 127.0.0.1:$PORT"
}

status() {
	"$XYMONCLIENT" "127.0.0.1:$PORT" "status testhost,example,com.conn $1 msg" \
		|| fail "xymond rejected a $1 status"
	sleep 0.4
}

tuple() {
	"$XYMONCLIENT" "127.0.0.1:$PORT" \
		"xymondboard fields=testname,color,oldcolor,previouscolor" 2>/dev/null \
		| awk -F'|' '$1 == "conn" { print $2 "/" $3 "/" $4 }'
}

assert_tuple() {
	local got
	got=$(tuple)
	assert_equal "$1" "$got" "$2 (color/oldcolor/previouscolor)"
}

start_xymond

status green
assert_tuple "green/none/none" "a new status has no prior report or transition color"

status red
assert_tuple "red/green/green" "a transition records the color being left"

status red
assert_tuple "red/red/green" "a repeated report advances oldcolor but not previouscolor"

status yellow
assert_tuple "yellow/red/red" "the next transition advances both prior-color fields"

stop_xymond
[ -s "$work/chk" ] || fail "xymond wrote no checkpoint file"
grep -q '^@@XYMONDCHK-V1|\.prevchangecolor\.|testhost\.example\.com|conn|red$' "$work/chk" \
	|| fail "checkpoint does not contain the prevchangecolor extension record"

start_xymond --restart="$work/chk"
assert_tuple "yellow/red/red" "previouscolor survives checkpoint restart"

pass "xymondboard distinguishes previous-report oldcolor from transition-stable previouscolor"

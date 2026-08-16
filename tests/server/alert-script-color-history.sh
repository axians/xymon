#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)

require_bin XYMOND xymond/xymond
require_bin XYMONCHANNEL xymond/xymond_channel
require_bin XYMONALERT xymond/xymond_alert
require_bin XYMONCLIENT common/xymon

work=$(mktempdir)
XYMOND_PID=""
CHANNEL_PID=""

stop_channel() {
	[ -n "$CHANNEL_PID" ] || return 0
	kill "$CHANNEL_PID" 2>/dev/null || true
	wait "$CHANNEL_PID" 2>/dev/null || true
	CHANNEL_PID=""
}

stop_xymond() {
	[ -n "$XYMOND_PID" ] || return 0
	kill "$XYMOND_PID" 2>/dev/null || true
	wait "$XYMOND_PID" 2>/dev/null || true
	XYMOND_PID=""
}

cleanup_daemons() {
	stop_channel
	stop_xymond
}
register_cleanup cleanup_daemons

mkdir -p "$work/home/etc" "$work/home/tmp" "$work/home/www"
printf 'page test Test\n127.0.0.1 testhost.example.com # conn\n' > "$work/hosts.cfg"
cp "$work/hosts.cfg" "$work/home/etc/hosts.cfg"
: > "$work/home/etc/holidays.cfg"
sed -e 's|^XYMONHOME=.*|XYMONHOME="'"$work"'/home"|' \
    -e 's|^XYMONTMP=.*|XYMONTMP="'"$work"'/home/tmp"|' \
	"$ROOT/xymond/etcfiles/xymonserver.cfg" > "$work/xymonserver.cfg" \
	|| skip "no xymonserver.cfg to run against"

cat > "$work/capture.sh" <<'EOF'
#!/bin/sh
printf '%s|%s|%s|%s|%s\n' \
	"$RECOVERED" "$BBCOLORLEVEL" "$BBOLDCOLOR" "$BBPREVIOUSCOLOR" "$BBSVCNAME" \
	>> "$CAPTURE"
EOF
chmod +x "$work/capture.sh"

cat > "$work/alerts.cfg" <<EOF
HOST=testhost.example.com SERVICE=conn
        SCRIPT $work/capture.sh test RECOVERED REPEAT=1m
EOF

free_port() {
	local port tries=0
	while [ "$tries" -lt 50 ]; do
		port=$((20000 + (RANDOM % 20000)))
		"$XYMONCLIENT" "127.0.0.1:$port" ping >/dev/null 2>&1 || { printf '%s' "$port"; return 0; }
		tries=$((tries+1))
	done
	return 1
}

wait_for_lines() {
	local wanted=$1 tries=0 got=0
	while [ "$tries" -lt 100 ]; do
		got=$(wc -l < "$work/capture" 2>/dev/null || printf 0)
		[ "$got" -ge "$wanted" ] && return 0
		kill -0 "$CHANNEL_PID" 2>/dev/null || { cat "$work/channel.log" >&2; fail "alert channel exited"; }
		tries=$((tries+1)); sleep 0.1
	done
	cat "$work/channel.log" >&2
	cat "$work/alert.trace" >&2 2>/dev/null || true
	cat "$work/xymond.log" >&2
	fail "expected $wanted captured alert(s), got $got"
}

PORT=$(free_port) || fail "no free port for xymond"
export CAPTURE="$work/capture"
export XYMONHOME="$work/home"
export XYMONTMP="$work/home/tmp"
export XYMONDPORT="$PORT"
export XYMSRV="127.0.0.1"
export XYMONSERVERHOSTNAME="testhost.example.com"
export HOSTSCFG="$work/hosts.cfg"
export ALERTCOLORS="red,yellow,purple"

"$XYMOND" --no-daemon --listen="127.0.0.1:$PORT" \
	--hosts="$work/hosts.cfg" --env="$work/xymonserver.cfg" \
	--pidfile="$work/xymond.pid" > "$work/xymond.log" 2>&1 &
XYMOND_PID=$!

tries=0
while [ "$tries" -lt 100 ]; do
	"$XYMONCLIENT" "127.0.0.1:$PORT" ping >/dev/null 2>&1 && break
	kill -0 "$XYMOND_PID" 2>/dev/null || { cat "$work/xymond.log" >&2; fail "xymond exited"; }
	tries=$((tries+1)); sleep 0.1
done
[ "$tries" -lt 100 ] || fail "xymond did not answer"

start_channel() {
	"$XYMONCHANNEL" --channel=page --log="$work/channel.log" \
		"$XYMONALERT" --config="$work/alerts.cfg" \
		--checkpoint-file="$work/alert.chk" --checkpoint-interval=600 \
		--trace="$work/alert.trace" &
	CHANNEL_PID=$!
	sleep 0.5
	kill -0 "$CHANNEL_PID" 2>/dev/null || { cat "$work/channel.log" >&2; fail "alert channel did not start"; }
}

status() {
	"$XYMONCLIENT" "127.0.0.1:$PORT" "status testhost,example,com.conn $1 msg" >/dev/null
	sleep 0.4
}

: > "$work/capture"
start_channel
status green
status red
wait_for_lines 1
assert_equal '0|red|green|green|conn' "$(sed -n '1p' "$work/capture")" \
	"the first alert must expose both colors from the green-to-red transition"

status red
stop_channel
[ -s "$work/alert.chk" ] || fail "xymond_alert wrote no checkpoint"
awk -F'|' -v OFS='|' '{ $7 = 0; print }' "$work/alert.chk" > "$work/alert.chk.due"
mv "$work/alert.chk.due" "$work/alert.chk"
awk -F'|' -v OFS='|' '{ $1 = 0; print }' "$work/alert.chk.sub" > "$work/alert.chk.sub.due"
mv "$work/alert.chk.sub.due" "$work/alert.chk.sub"
start_channel
"$XYMONCLIENT" "127.0.0.1:$PORT" "status testhost,example,com.wakeup red msg" >/dev/null
wait_for_lines 2
assert_equal '0|red|red|green|conn' "$(sed -n '2p' "$work/capture")" \
	"a repeated alert after worker restart must keep oldcolor and previouscolor distinct"

stop_channel
awk -F'|' -v OFS='|' '{ $7 = 0; NF = 10; print }' "$work/alert.chk" > "$work/alert.chk.old"
mv "$work/alert.chk.old" "$work/alert.chk"
awk -F'|' -v OFS='|' '{ $1 = 0; print }' "$work/alert.chk.sub" > "$work/alert.chk.sub.due"
mv "$work/alert.chk.sub.due" "$work/alert.chk.sub"
start_channel
"$XYMONCLIENT" "127.0.0.1:$PORT" "status testhost,example,com.wakeup2 red msg" >/dev/null
wait_for_lines 3
assert_equal '0|red|none|none|conn' "$(sed -n '3p' "$work/capture")" \
	"an old checkpoint must not invent color history"

pass "alert scripts receive BBOLDCOLOR and BBPREVIOUSCOLOR from alert events and compatible checkpoints"

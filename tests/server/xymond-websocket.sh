#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
require_bin GATEWAY xymond/xymond_websocket
source_text=$(cat "$ROOT/xymond/xymond.c")
assert_contains 'posttochannel(stachgchn, "ack", log->ackmsg' "$source_text" \
	"acknowledgements must reach the WebSocket event feed"
assert_contains 'posttochannel(stachgchn, "disable"' "$source_text" \
	"disable comments must reach the WebSocket event feed"
assert_contains 'posttochannel(stachgchn, "heartbeat"' "$source_text" \
	"xymond must heartbeat the WebSocket event feed"
assert_contains 'nextliveheartbeat = now + 30' "$source_text" \
	"xymond WebSocket heartbeats must run every 30 seconds"
gateway_text=$(cat "$ROOT/xymond/xymond_websocket.c")
assert_contains '#define XYMOND_STALE_TIMEOUT 65' "$gateway_text" \
	"the gateway must identify a silent xymond within 65 seconds"
require_cc
work=$(mktempdir)
"$CC" -std=c99 -Wall -Wextra -Werror -o "$work/xymond-websocket-harness" \
	"$(dirname "$0")/xymond-websocket-harness.c" \
	|| fail "WebSocket protocol harness does not compile"
"$work/xymond-websocket-harness" "$GATEWAY" \
	|| fail "xymond_websocket protocol regression failed"

pass "xymond_websocket upgrades same-origin clients, broadcasts stachg JSON, and replays its in-memory ring"
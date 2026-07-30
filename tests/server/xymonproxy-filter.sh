#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || skip "no C compiler available (CC=$CC)"

WORK=$(mktempdir)
if ! "$CC" -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Werror \
	-I"$ROOT/xymonproxy" \
	"$ROOT/xymonproxy/filter.c" "$ROOT/xymonproxy/test-filter.c" \
	-o "$WORK/test-filter" 2>"$WORK/cc.log"; then
	cat "$WORK/cc.log" >&2
	fail "xymonproxy filter contract does not compile cleanly"
fi

"$WORK/test-filter"

PROXY_SOURCE=$(cat "$ROOT/xymonproxy/xymonproxy.c")
assert_contains 'Command filtered messages' "$PROXY_SOURCE"
assert_contains 'Host filtered messages' "$PROXY_SOURCE"
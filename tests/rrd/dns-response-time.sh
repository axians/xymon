#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compile and run the real do_net.c DNS response-time parser with RRD plumbing
# stubbed. Verifies per-NS samples, validation, sanitization, and aggregation.

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)

if [ -r "$here/../lib/assert.sh" ]; then
	# shellcheck source=tests/lib/assert.sh
	. "$here/../lib/assert.sh"
else
	fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
	skip() { printf 'SKIP: %s\n' "$*" >&2; exit 77; }
fi

CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || skip "no C compiler available (CC=$CC)"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

harness="$work/dns-response-time"
if ! "$CC" -g -O1 -fsanitize=address,undefined -o "$harness" \
		"$here/dns-response-time-harness.c" 2>"$work/cc-asan.log"; then
	"$CC" -g -O1 -o "$harness" "$here/dns-response-time-harness.c" 2>"$work/cc.log" \
		|| { cat "$work/cc.log" >&2; fail "harness does not compile"; }
fi

if ! ASAN_OPTIONS="detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}" \
		"$harness" >"$work/run.log" 2>&1; then
	cat "$work/run.log" >&2
	fail "DNS response-time parsing is broken (see output above)"
fi

exit 0

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Compile and run the real dns2.c cross-nameserver comparator.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || skip "no C compiler available (CC=$CC)"
pkg-config --exists libcares 2>/dev/null || skip "c-ares development files unavailable"

root=$(find_root)
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktempdir)
printf '#define HAVE_SYS_SELECT_H 1\n' >"$work/config.h"

cflags=$(pkg-config --cflags libcares)
libs=$(pkg-config --libs libcares)
if ! "$CC" -g -O1 -ffunction-sections -fdata-sections $cflags \
		-I"$work" -I"$root/include" -I"$root/xymonnet" \
		-c "$root/xymonnet/dns2.c" -o "$work/dns2.o" 2>"$work/cc.log"; then
	cat "$work/cc.log" >&2
	fail "dns2.c comparator does not compile"
fi
if ! "$CC" -g -O1 -ffunction-sections -fdata-sections \
		-I"$work" -I"$root/include" \
		-c "$root/lib/encoding.c" -o "$work/encoding.o" 2>>"$work/cc.log"; then
	cat "$work/cc.log" >&2
	fail "Xymon escape decoder does not compile"
fi
if ! "$CC" -g -O1 -ffunction-sections -fdata-sections \
		-I"$work" -I"$root/include" \
		-c "$root/lib/misc.c" -o "$work/misc.o" 2>>"$work/cc.log"; then
	cat "$work/cc.log" >&2
	fail "Xymon escape helper does not compile"
fi
if ! "$CC" -g -O1 $cflags -I"$work" -I"$root/include" -I"$root/xymonnet" \
		-Wl,--gc-sections -o "$work/dns-crossns-compare" \
		"$here/dns-crossns-compare-harness.c" "$work/dns2.o" "$work/encoding.o" "$work/misc.o" $libs 2>>"$work/cc.log"; then
	cat "$work/cc.log" >&2
	fail "DNS comparison harness does not link"
fi

"$work/dns-crossns-compare" || fail "DNS response comparison or content matching is broken"
pass "DNS content regex and available structured-response comparisons behave correctly"
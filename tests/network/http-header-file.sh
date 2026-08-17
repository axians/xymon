#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
require_bin XYMONNET xymonnet/xymonnet

work=$(mktempdir)
mkdir -p "$work/home/etc" "$work/tmp" "$work/var" "$work/logs"

cat >"$work/hosts.cfg" <<EOF
127.0.0.1 header-file.example # noconn http://127.0.0.1:1/ httphdrfile="$work/missing-headers"
EOF

HOSTSCFG="$work/hosts.cfg" XYMONHOME="$work/home" XYMONTMP="$work/tmp" \
	XYMONVAR="$work/var" XYMONSERVERLOGS="$work/logs" CONNTEST=FALSE \
	"$XYMONNET" --test-untagged --noping --no-ssl --no-update \
	--timeout=5 header-file.example >"$work/xymonnet.out" 2>"$work/xymonnet.err" \
    || fail "xymonnet did not report the protected-header setup failure normally:\n$(cat "$work/xymonnet.err")"

combined=$(cat "$work/xymonnet.out" "$work/xymonnet.err")
assert_contains "Cannot open HTTP header file $work/missing-headers" "$combined" \
    "missing protected-header file did not produce a useful error"
assert_contains "I/O error" "$combined" \
    "missing protected-header file did not fail the HTTP test closed"
assert_not_contains "Connect failed" "$combined" \
    "xymonnet attempted the HTTP connection after protected-header setup failed"

pass "xymonnet fails closed when a protected HTTP header file cannot be loaded"
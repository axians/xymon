#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/network/dns-ares-record.sh
#
# Guard for the ares_dns_record_t DNS response parser added for issue #235
# (step 1: port response parsing off the legacy adig-derived wire-format
# parser, no behavioural change).
#
# The wiring spans xymonnet/dns2.c:
#   - an ARES_VERSION feature-detection gate (c-ares >= 1.32, the oldest
#     release confirmed to provide every accessor this file calls --
#     ares_dns_parse()/ares_dns_record_t itself became public API back in
#     1.22, but ares_dns_record_rr_get_const() wasn't added until ~1.28-1.30
#     and ares_dns_rr_get_abin()/ares_dns_rr_get_abin_cnt() not until
#     ~1.31-1.32) selecting between dns_render_arespec() (structured parser)
#     and dns_render_legacy() (the original adig-derived
#     display_question()/display_rr() wire-format parser), kept as the
#     fallback for platforms below that floor. Ubuntu 24.04 (c-ares 1.27.0)
#     and Debian 12 / RHEL/Rocky/Alma 8 and 9 (all c-ares < 1.22) are among
#     the platforms below this floor at the time of writing.
#   - dns_detail_callback() dispatching to whichever path the gate selected.
#
# A behavioural run needs a live nameserver (or a loopback DNS stub) and two
# different c-ares generations to exercise both paths, well beyond what this
# suite can set up portably. This is a static guard that the wiring -- both
# the modern path AND the legacy fallback -- survives future edits. Skips only
# when the source file is absent (e.g. an autopkgtest run with no source
# tree); if a present tree has lost either path, that is a regression and the
# test fails rather than skips.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
DNS2="$ROOT/xymonnet/dns2.c"

[ -f "$DNS2" ] || skip "xymonnet/dns2.c absent"

src=$(cat "$DNS2")

# The version floor itself: c-ares 1.22 is when ares_dns_parse()/
# ares_dns_record_t became public API, but ares_dns_record_rr_get_const() and
# ares_dns_rr_get_abin()/ares_dns_rr_get_abin_cnt() -- both used by this file
# -- weren't added until later (confirmed absent from 1.27.0 and 1.30.0's
# public headers, confirmed present in 1.32.0's). Gating on 1.22 alone builds
# fine but fails to LINK on any platform whose c-ares is 1.22-1.31 (e.g.
# Ubuntu 24.04 "noble", c-ares 1.27.0). Getting this wrong either regresses
# that build failure, or needlessly keeps platforms that do have the full
# API on the legacy path.
assert_contains "ARES_VERSION >= 0x012000" "$src" \
	"dns2.c lost (or changed) the c-ares 1.32 ARES_VERSION gate (issue #235)"

# Both render paths must still exist: the structured one (what most modern
# platforms will use) and the legacy adig-derived one (the fallback still
# required for Debian 12 / RHEL 8-9-era c-ares).
assert_contains "dns_render_arespec" "$src" \
	"dns2.c lost the ares_dns_record_t-based renderer (issue #235)"
assert_contains "dns_render_legacy" "$src" \
	"dns2.c lost the legacy adig-derived renderer -- required fallback for c-ares < 1.22"
assert_contains "static const unsigned char *display_rr(" "$src" \
	"dns2.c lost display_rr() -- the legacy wire-format RR parser used by dns_render_legacy()"

# dns_detail_callback() must actually dispatch to both, gated by the same
# macro the ARES_VERSION check defines -- otherwise one path is wired up but
# dead code (e.g. gate present, renderer present, but the call site was
# reverted to always use one path).
callback_body=$(awk '/^void dns_detail_callback\(/{c=1} c{print} c&&/^}/{exit}' "$DNS2")
[ -n "$callback_body" ] || fail "dns2.c no longer has dns_detail_callback() -- ARES response dispatch unreachable"
grep -q 'XYMON_DNS_USE_ARES_DNS_RECORD' <<<"$callback_body" \
	|| fail "dns_detail_callback() no longer branches on XYMON_DNS_USE_ARES_DNS_RECORD -- one render path is dead code"
grep -q 'dns_render_arespec(abuf, alen, response);' <<<"$callback_body" \
	|| fail "dns_detail_callback() no longer calls dns_render_arespec() -- structured path unreachable"
grep -q 'dns_render_legacy(abuf, alen, response);' <<<"$callback_body" \
	|| fail "dns_detail_callback() no longer calls dns_render_legacy() -- legacy fallback unreachable"

# The structured renderer must still cover the record types the legacy parser
# handled (A, AAAA, CNAME, NS, PTR, HINFO, MX, SOA, TXT, SRV) -- losing a case
# silently degrades that type to "[Unknown RR; cannot parse]" instead of being
# a behaviour change caught anywhere else.
arespec_body=$(awk '/^static void dns_render_rr_arespec\([^;]*$/{c=1} c{print} c&&/^}/{exit}' "$DNS2")
[ -n "$arespec_body" ] || fail "dns2.c no longer has dns_render_rr_arespec() -- per-RR structured rendering unreachable"
for rectype in ARES_REC_TYPE_CNAME ARES_REC_TYPE_NS ARES_REC_TYPE_PTR \
		ARES_REC_TYPE_HINFO ARES_REC_TYPE_MX ARES_REC_TYPE_SOA \
		ARES_REC_TYPE_TXT ARES_REC_TYPE_A ARES_REC_TYPE_AAAA ARES_REC_TYPE_SRV; do
	grep -q "case $rectype:" <<<"$arespec_body" \
		|| fail "dns_render_rr_arespec() lost its $rectype case -- that record type would silently degrade to \"[Unknown RR; cannot parse]\""
done

pass "xymonnet keeps the #235 ares_dns_record_t wiring (version gate, both render paths, all legacy record types covered)"

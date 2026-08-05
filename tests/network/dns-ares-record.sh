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
#   - an ARES_VERSION feature-detection gate (c-ares >= 1.32, the exact
#     minimum release providing every accessor this file calls --
#     ares_dns_parse()/ares_dns_record_t itself became public API back in
#     1.22, ares_dns_record_rr_get_const() was added in 1.28.0, and
#     ares_dns_rr_get_abin()/ares_dns_rr_get_abin_cnt() not until 1.32.0)
#     selecting between dns_render_arespec() (structured parser)
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
# ares_dns_record_t became public API, but ares_dns_record_rr_get_const()
# (added 1.28.0) and ares_dns_rr_get_abin()/ares_dns_rr_get_abin_cnt() (added
# 1.32.0) -- both used by this file -- weren't added until later (confirmed
# by diffing each release's public ares_dns_record.h). Gating on 1.22 alone
# builds fine but fails to LINK on any platform whose c-ares is 1.22-1.31
# (e.g. Ubuntu 24.04 "noble", c-ares 1.27.0). Getting this wrong either
# regresses that build failure, or needlessly keeps platforms that do have
# the full API on the legacy path.
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

dns=$(cat "$ROOT/xymonnet/dns.c")
assert_contains 'strtok_r(tspec, ",", &querysave)' "$dns" \
	"comma-separated DNS queries must use independent tokenizer state"
assert_contains 'strtok_r(statcopy, ",", &saveptr)' "$dns" \
	"static dns-ns lists must not corrupt DNS query tokenizer state"
assert_contains 'NS response time: %s %d:%s' "$dns" \
	"per-NS timing records must retain query identity for multi-query hosts"
assert_contains "strchr(tspec, ';')" "$dns" \
	"dns= content expressions must be separated from the lookup before querying"
assert_contains 'dns_decode_content_pattern(contentpattern, &contentregexp' "$dns" \
	"DNS content expressions must validate and decode Xymon escapes"
assert_contains 'dns_response_matches(STRBUF(walk->msgbuf), (char *)contentregexp' "$dns" \
	"DNS content matching must inspect the response without consuming its string buffer"
assert_contains '!crossns_ok || !content_ok' "$dns" \
	"a DNS content mismatch must fail the DNS test"

xymonnet=$(cat "$ROOT/xymonnet/xymonnet.c")
nslookup_body=$(awk '/^void run_nslookup_service\(/{c=1} c{print} c&&/^}/{exit}' "$ROOT/xymonnet/xymonnet.c")
assert_contains 'int expected_open = (walk->reverse ? !walk->open : walk->open)' "$nslookup_body" \
	"multiple dns= tags must aggregate each lookup according to its own reverse expectation"
assert_contains 'aggregate_open = 0' "$nslookup_body" \
	"multiple dns= tags on one host must fail their aggregate status when any expected result fails"
assert_contains 'if (!walk->dialup) failed_tests_are_dialup = 0' "$nslookup_body" \
	"a dialup DNS failure must not hide a non-dialup failure in the aggregate status"
assert_contains 'if (walk->alwaystrue) failed_test_ignores_ping = 1' "$nslookup_body" \
	"an always-true DNS failure must remain visible when aggregate results are reported"
assert_contains 'addtobufferraw(combined, STRBUF(walk->banner)' "$nslookup_body" \
	"multiple dns= tags on one host must retain every lookup banner without duplicate timing lines"
assert_contains 'total_seconds += seconds' "$nslookup_body" \
	"multiple dns= tags must sum their response times into one aggregate timing value"
assert_contains 'walk->internal = 1' "$nslookup_body" \
	"multiple dns= tags on one host must emit only one aggregate DNS status"

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

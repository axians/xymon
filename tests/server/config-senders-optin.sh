#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# tests/server/config-senders-optin.sh
#
# Regression guard for the --config-senders opt-in switch in xymond.
#
# The "config" command lets a caller retrieve any file under the server's
# etc/ directory, and "query" lets a caller retrieve a single test's
# current status. --config-senders is opt-in and independent of
# --admin-senders:
#   - unset (default): "config"/"query" are gated by --status-senders.
#   - set: only hosts in --config-senders may send them, and --status-senders
#     no longer applies to either command.
#
# --admin-senders never grants "config"/"query" access, and --config-senders
# never grants "drop"/"rename" rights.
#
# A behavioural test would need a running xymond plus a network harness;
# what we guard here is that the *fix* survives future edits.

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
FILE="$ROOT/xymond/xymond.c"

[ -f "$FILE" ] || skip "xymond/xymond.c absent"
src=$(cat "$FILE")

# (1) The "config" access check must be gated by configsenders itself -- unset
# configsenders must not trigger any restriction.
assert_contains "if (configsenders) {" "$src" \
	"config-senders opt-in gate missing: restriction no longer conditional on configsenders being set"

# (2) configsenders membership must be checked, with no adminsenders check
# mixed into it -- the two lists must stay independent.
assert_contains "if (!oksender(configsenders, NULL, msg->addr.sin_addr, NULL)) {" "$src" \
	"config-senders: configsenders membership check missing or no longer independent of adminsenders"

# (3) Unset configsenders must fall back to gating "config" by statussenders.
assert_contains "else if (!oksender(statussenders, NULL, msg->addr.sin_addr, msg->buf)) {" "$src" \
	"config-senders: statussenders fallback missing for the unset (default) case"

# (4) The "query" access check must also be gated by configsenders when set,
# using the same opt-in pattern as "config".
assert_contains "if (!oksender(configsenders, (h ? h->ip : NULL), msg->addr.sin_addr, NULL)) {" "$src" \
	"config-senders: query's configsenders membership check missing"

# (5) Unset configsenders must fall back to gating "query" by statussenders.
assert_contains "else if (!oksender(statussenders, (h ? h->ip : NULL), msg->addr.sin_addr, msg->buf)) {" "$src" \
	"config-senders: statussenders fallback missing for query's unset (default) case"

pass "xymond.c keeps the config-senders opt-in contract for config and query"

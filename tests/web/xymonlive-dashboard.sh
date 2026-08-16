#!/usr/bin/env bash

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

require_cc
ROOT=$(find_root)
SRC="$ROOT/web/xymonlive.c"
JS="$ROOT/xymond/wwwfiles/gifs/xymonlive.js"
CSS="$ROOT/xymond/wwwfiles/gifs/xymonlive.css"
work=$(mktempdir)
source_text=$(cat "$SRC")
makefile_text=$(cat "$ROOT/web/Makefile")
cgiwrap_text=$(cat "$ROOT/web/cgiwrap.c")
js_text=$(cat "$JS")
css_text=$(cat "$CSS")

sed -n '/^static void json_string/,/^}/p' "$SRC" >"$work/xymonlive-json.inc"
grep -q '^static void json_string' "$work/xymonlive-json.inc" \
	|| fail "could not extract json_string() from xymonlive.c"
assert_contains "query[6] == '&'" "$source_text" \
	"data mode must accept additional query parameters"

"$CC" -D_GNU_SOURCE -I"$work" -o "$work/json-harness" \
	"$(dirname "$0")/xymonlive-json-harness.c" \
	|| fail "xymonlive JSON harness does not compile"
"$work/json-harness" || fail "xymonlive emits invalid JSON strings"

assert_contains 'xymondboard color=red,yellow,purple,blue ' "$source_text" \
	"xymonlive must query actionable colors from the current board"
assert_not_contains 'color=green' "$source_text" \
	"xymonlive must not query non-actionable green statuses"
assert_contains 'fields=hostname,testname,color,lastchange,logtime,' "$source_text" \
	"xymonlive must request the dashboard board fields"
assert_contains 'acktime,disabletime,ackmsg,dismsg,line1' "$source_text" \
	"xymonlive must request state comments from the current board"
assert_contains 'nldecode(ackmsg)' "$source_text" \
	"acknowledgement comments must be decoded before JSON serialization"
assert_contains 'nldecode(dismsg)' "$source_text" \
	"disable comments must be decoded before JSON serialization"
assert_not_contains 'allevents' "$source_text" \
	"xymonlive must not depend on the history log"
assert_contains 'Cache-Control: no-store' "$source_text" \
	"live responses must not be cached by HTTP intermediaries"
assert_contains 'F_SETLK' "$source_text" \
	"concurrent dashboard requests must share one cache refresh"
assert_contains 'rename(tempfn, cachefn)' "$source_text" \
	"dashboard cache updates must be atomic"
assert_contains 'mkstemp(tempfn)' "$source_text" \
	"dashboard cache must use securely created temporary files"
assert_contains 'serve_cache(cachefn)' "$source_text" \
	"dashboard must serve cached snapshots"
assert_contains 'hostsvcurl(hostname, "info", 0)' "$source_text" \
	"hostnames must link to the host-specific Xymon info view"
assert_contains 'hostsvcurl(hostname, testname, 0)' "$source_text" \
	"service names must link to their Xymon service view"
assert_contains 'CGI_XYMONLIVE_OPTS="--env=$XYMONENV --cache=3"' "$(cat "$ROOT/xymond/etcfiles/cgioptions.cfg.DIST")" \
	"installed dashboard must enable the shared snapshot cache"
assert_contains 'xymonlive.cgi' "$makefile_text" \
	"xymonlive CGI is absent from the web build"
assert_contains 'xymonlive.sh' "$makefile_text" \
	"xymonlive wrapper is absent from CGI installation"
assert_contains 'CGI_XYMONLIVE_OPTS' "$cgiwrap_text" \
	"cgiwrap does not recognize xymonlive"
assert_contains 'fetch("?data=1"' "$js_text" \
	"dashboard does not poll its direct-board JSON endpoint"
assert_contains 'addEvent(status, status.color, "green")' "$js_text" \
	"statuses leaving the active set must be shown as green"
assert_not_contains '"normal"' "$js_text" \
	"dashboard transitions must use the Xymon green color name"
assert_contains 'document.querySelectorAll(".fold")' "$js_text" \
	"dashboard panels must be independently foldable"
assert_contains '.workspace { display: grid; align-items: start; gap: 18px; }' "$css_text" \
	"folded workspace panels must collapse to their heading height"
assert_contains 'panel.classList.toggle("folded", folded)' "$js_text" \
	"fold controls must mark their panel for compact styling"
assert_contains '.folded > .section-heading { min-height: 38px; }' "$css_text" \
	"folded panels must use a compact heading"
assert_contains '.controls-panel.folded { margin: 10px 0 8px; }' "$css_text" \
	"folded search and filter controls must reduce their vertical margins"
assert_contains 'data-target=\"toolbar-content\"' "$source_text" \
	"search and filter controls must be independently foldable"
assert_contains '"toolbar-content": "search and filter"' "$js_text" \
	"search and filter fold controls must expose their panel name"
assert_contains 'id=\"layout\"' "$source_text" \
	"dashboard must provide a panel layout selector"
assert_contains '<h2>Recent changes</h2>' "$source_text" \
	"dashboard must accurately name its bounded transition list"
assert_not_contains 'Observed changes' "$source_text" \
	"dashboard must not imply that recent transitions are complete history"
assert_contains 'sessionStorage.setItem("xymonlive-layout", layout)' "$js_text" \
	"dashboard must remember the selected panel layout"
assert_contains 'workspace.className = "workspace layout-" + layout' "$js_text" \
	"dashboard must apply the selected panel layout"
assert_contains 'document.querySelectorAll(".drag-handle")' "$js_text" \
	"dashboard panels must provide draggable handles"
assert_contains 'handle.setPointerCapture(event.pointerId)' "$js_text" \
	"dashboard panel dragging must use reliable pointer capture"
assert_contains 'setLayout(droppedLayout(handle.dataset.panel, dropPosition(event, workspace)))' "$js_text" \
	"dropping a panel must update the persisted layout"
assert_contains '.workspace.layout-changes-left' "$css_text" \
	"dashboard must support recent changes on the left"
assert_contains '.workspace.layout-side-by-side > .status-panel:not(.folded), .workspace.layout-side-by-side > aside:not(.folded)' "$css_text" \
	"side-by-side panels must use equal heights without stretching folded panels"
assert_contains '.workspace.layout-side-by-side aside, .workspace.layout-changes-left aside { max-height: none; }' "$css_text" \
	"side-by-side panels must not cap recent changes height"
assert_contains '.workspace.layout-side-by-side #events, .workspace.layout-changes-left #events { height: auto; max-height: none; overflow-y: visible; }' "$css_text" \
	"side-by-side recent changes must grow without internal vertical scrolling"
assert_contains '.workspace.layout-changes-above aside { order: -1; }' "$css_text" \
	"dashboard must support observed changes above active statuses"
assert_contains '.workspace.layout-active-above aside, .workspace.layout-changes-above aside { max-height: none; }' "$css_text" \
	"stacked desktop layouts must let recent changes grow naturally"
assert_contains '.workspace #events { height: auto; max-height: none; overflow-y: visible; }' "$css_text" \
	"narrow stacked layouts must not internally scroll recent changes"
assert_contains 'hostLink.href = status.hostUrl' "$js_text" \
	"dashboard hostnames must use the host Xymon URL"
assert_contains '.table-wrap { overflow-x: auto; }' "$css_text" \
	"active statuses must provide a native horizontal scrollbar at the table bottom"
assert_not_contains 'status-scrollbar' "$source_text$js_text$css_text" \
	"active statuses must not duplicate the bottom scrollbar above the table"
assert_contains 'hostLink.title = escapeText(status.host)' "$js_text" \
	"truncated dashboard hostnames must expose their complete FQDN on hover"
assert_contains 'serviceLink.href = status.serviceUrl' "$js_text" \
	"dashboard service names must use the service Xymon URL"
assert_contains 'ack.title = status.ackMessage' "$js_text" \
	"acknowledgement badges must expose their comment on hover"
assert_contains 'disabled.title = status.disableMessage' "$js_text" \
	"disabled badges must expose their comment on hover"
assert_contains 'new RegExp(input.value, "i")' "$js_text" \
	"dashboard must support case-insensitive regular expression filters"
assert_contains 'events.filter(matchesRegex)' "$js_text" \
	"regular expression filters must also limit observed changes"
assert_contains 'part(date.getDate()) + " " + clock(epoch)' "$js_text" \
	"wide recent-change timestamps must use readable local date and time"
assert_contains '@media (min-width: 701px)' "$css_text" \
	"recent changes must use the Xymon Live threshold for single-line timestamps"
assert_contains 'compactTime.textContent = dateTime(event.time)' "$js_text" \
	"narrow recent changes must retain their date"
assert_contains 'max-width: 72px; white-space: normal' "$css_text" \
	"narrow recent-change timestamps must wrap date and time onto separate lines"
assert_contains 'seedEvents(payload.events, payload.statuses)' "$js_text" \
	"fresh browsers must accept recent transitions from the data endpoint"
assert_contains 'host.href = event.hostUrl' "$js_text" \
	"observed-change hostnames must link to their Xymon host view"
assert_contains 'service.href = event.serviceUrl' "$js_text" \
	"observed-change services must link to their Xymon service view"
assert_contains 'hostUrl: status.hostUrl, serviceUrl: status.serviceUrl' "$js_text" \
	"live transitions must retain canonical Xymon links"
assert_contains 'event.previousColor && event.color' "$js_text" \
	"seeded transitions must describe the previous and current snapshot colors"
assert_not_contains 'event.from' "$js_text" \
	"seeded transitions must not use ambiguous from/to color names"
assert_contains 'if (initialized || events.length || !Array.isArray(seed)) return' "$js_text" \
	"server history must only initialize a fresh browser once"
assert_contains 'id=\"regex-scope\"' "$source_text" \
	"dashboard must let users target host or service names"
assert_contains 'title=\"Enter plain text or a regular expression. Matching is case-insensitive.\">Filter</span>' "$source_text" \
	"dashboard filter must provide concise hover help"
assert_contains '<main><div class=\"top-heading\"><h1>Xymon Live</h1><section class=\"summary\"' "$source_text" \
	"dashboard title and status totals must share the top heading"
assert_contains '<div id=\"top-actions\" class=\"top-actions\"><div class=\"refresh-options\" role=\"group\" aria-label=\"Refresh interval\"' "$source_text" \
	"refresh interval must use the same in-page control at every width"
assert_not_contains 'id=\"interval\"' "$source_text" \
	"refresh interval must not use a native dropdown"
assert_contains 'id=\"top-menu-toggle\" class=\"top-menu-toggle\"' "$source_text" \
	"mobile top controls must provide a hamburger toggle"
assert_not_contains '<label class=\"interval\">' "$source_text" \
	"refresh interval must not remain in search and filter controls"
assert_not_contains 'id=\"count-all\"' "$source_text" \
	"dashboard top heading must not include an Active counter"
assert_not_contains 'id=\"count-green\"' "$source_text" \
	"dashboard status totals must omit non-actionable green statuses"
assert_not_contains 'color-swatch' "$source_text$css_text" \
	"dashboard color totals must not show status dots"
assert_contains 'colorFilter = selected ? "all" : button.dataset.color' "$js_text" \
	"color filters must toggle off without an Active reset button"
assert_contains 'output.dataset.digits = String(count).length' "$js_text" \
	"color totals must expose their digit count for overflow-safe sizing"
assert_not_contains '<span>Green</span>' "$source_text" \
	"dashboard color totals must not show long color labels"
assert_contains '<section class=\"status-panel\"><div class=\"section-heading status-heading\"><h2>Active statuses</h2>' "$source_text" \
	"active-status controls must remain on the status panel"
assert_contains 'aria-expanded=\"true\"' "$source_text" \
	"dashboard fold controls must expose their state"
assert_contains '@media (max-width: 700px)' "$css_text" \
	"dashboard has no narrow-viewport layout"
assert_contains '.top-heading h1 { display: none; }' "$css_text" \
	"dashboard title must yield its space on narrow screens"
assert_contains '.top-heading.menu-open .top-actions { display: flex; }' "$css_text" \
	"mobile hamburger must reveal top controls"
assert_contains '.workspace { gap: 8px; }' "$css_text" \
	"narrow stacked panels must use compact spacing"
assert_contains '.summary { width: 100%; min-width: 0; min-height: 52px;' "$css_text" \
	"mobile hamburger must not reduce the status counter width"
assert_contains 'min-width: 240px; overflow-x: auto' "$css_text" \
	"desktop controls must not collapse status totals near the compact threshold"
assert_contains 'container-type: inline-size' "$css_text" \
	"status counter sizing must use its own container width"
assert_contains '.top-menu-toggle { position: absolute; top: 11px; right: 3px;' "$css_text" \
	"mobile hamburger must not add a separate top-row strip"
assert_contains '.metric.blue strong { transform: translateX(-14px); }' "$css_text" \
	"mobile hamburger must not cover the blue status total"
assert_contains 'document.querySelector(".top-heading").classList.toggle("menu-open", open)' "$js_text" \
	"mobile hamburger must toggle the top control panel"
assert_contains 'min-height: 52px; justify-content: center' "$css_text" \
	"mobile status totals must fill the top row vertically"
assert_contains 'font-size: min(30px, 62cqw)' "$css_text" \
	"mobile status totals must grow to the available counter width"
assert_contains 'font: 600 30px/1 monospace' "$css_text" \
	"desktop status totals must grow to the available counter width"
assert_contains 'strong[data-digits="5"]' "$css_text" \
	"large mobile status totals must shrink before overflowing"
assert_contains 'overflow-x: auto; overflow-y: hidden' "$css_text" \
	"status totals must not show a vertical scrollbar"

if command -v node >/dev/null 2>&1; then
	node --check "$JS" || fail "xymonlive browser JavaScript has a syntax error"
fi

pass "xymonlive queries the board directly and emits browser-safe JSON"
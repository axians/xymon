#!/usr/bin/env bash

set -euo pipefail
# shellcheck source=tests/lib/assert.sh
. "$(dirname "$0")/../lib/assert.sh"

ROOT=$(find_root)
SRC="$ROOT/web/xymonlive.c"
JS="$ROOT/xymond/wwwfiles/gifs/xymonlive.js"
CSS="$ROOT/xymond/wwwfiles/gifs/xymonlive.css"
source_text=$(cat "$SRC")
js_text=$(cat "$JS")
css_text=$(cat "$CSS")
tasks_text=$(cat "$ROOT/xymond/etcfiles/tasks.cfg.DIST")
websocket_task=$(sed -n '/^\[xymonlive-websocket\]/,/^$/p' "$ROOT/xymond/etcfiles/tasks.cfg.DIST")
apache_open=$(cat "$ROOT/xymond/etcfiles/xymon-apache-open.DIST")
apache_secure=$(cat "$ROOT/xymond/etcfiles/xymon-apache-secure.DIST")

assert_contains 'data-websocket-path=\"%s/xymonlive-ws\"' "$source_text" \
	"the CGI page must publish its same-origin WebSocket path"
assert_contains '>Recent changes</h2>' "$source_text" \
	"the dashboard must present the transition stream"
assert_contains 'title=\"Timestamps stay bold for five minutes after each event\">Recent changes</h2>' "$source_text" \
	"Recent changes must explain why new timestamps are bold"
assert_contains 'id=\"event-count\" title=\"Matching events displayed after filters and the Rows limit\"' "$source_text" \
	"the shown count must explain which events it counts"
assert_contains 'data-target=\"controls-content\"' "$source_text" \
	"event controls must remain foldable"
assert_contains '<h2>Event controls</h2>' "$source_text" \
	"the controls panel must describe filtering, display, and Pause together"
assert_not_contains '<h2>Search and filter</h2>' "$source_text" \
	"the controls panel must not retain its stale Search label"
assert_contains 'class=\"heading-actions live-actions\"' "$source_text" \
	"Xymon Live connection state must sit on the right of Search and filter"
assert_contains 'title=\"Green means connected to the live event stream; red means disconnected and retrying; yellow means connecting or paused. Pause stops updates and reconnect attempts.\"' "$source_text" \
	"Xymon Live must explain its connection colors and Pause behavior"
assert_not_contains 'class=\"top-heading\"' "$source_text" \
	"the moved connection state must not leave a standalone top row"
assert_contains 'id=\"event-limit\"' "$source_text" \
	"the browser must provide a client-side row limit"
assert_contains '<option value=\"100\" selected>100</option>' "$source_text" \
	"the client-side row limit must default to 100"
assert_contains '<option value=\"500\">500</option>' "$source_text" \
	"the client-side row limit must allow the full retained history"
assert_not_contains 'id=\"search\"' "$source_text" \
	"Filter must be the single host/service matching control"
assert_contains 'placeholder=\"Plain text or regular expression\"' "$source_text" \
	"Filter must advertise both accepted input forms"
assert_contains 'title=\"Filter host or service names with case-insensitive plain text or a regular expression\"' "$source_text" \
	"Filter must explain plain-text and regular-expression matching"
assert_contains '<option value=\"message\">Message</option>' "$source_text" \
	"Match must support status message text"
assert_contains '<option value=\"any\" selected>Any</option>' "$source_text" \
	"Match must support all event fields"
assert_contains 'title=\"Choose whether Filter matches host, service, message, or any field\"' "$source_text" \
	"Match must explain all supported scopes"
assert_contains '<option value=\"disable\">Disable</option>' "$source_text" \
	"Type must support disable-comment events"
assert_contains '<option value=\"ack\">Ack</option>' "$source_text" \
	"Type must support acknowledgement events"
assert_contains 'sessionStorage.setItem("xymonlive-event-kind"' "$js_text" \
	"the selected event type must survive reloads"
assert_contains 'title=\"Maximum matching transitions rendered in this browser\"' "$source_text" \
	"Rows must explain its client-side display limit"
assert_contains 'title=\"Pause live updates and reconnect attempts\"' "$source_text" \
	"Pause must explain that it also pauses reconnects"
assert_not_contains 'Active statuses' "$source_text" \
	"the changes-only dashboard must not render the old snapshot panel"
assert_not_contains 'data=1' "$source_text$js_text" \
	"the dashboard must not retain its snapshot JSON endpoint"
assert_not_contains 'xymondboard' "$source_text" \
	"page requests must not query every active status"
assert_not_contains 'sendmessage(' "$source_text" \
	"the lightweight CGI must not contact xymond"
assert_not_contains 'cachefn' "$source_text" \
	"the removed snapshot endpoint must not leave cache state behind"

assert_contains 'connection = new WebSocket(websocketUrl())' "$js_text" \
	"the dashboard must consume transitions over WebSocket"
assert_contains 'if (paused || socket) return' "$js_text" \
	"reconnects must wait for the previous socket close event"
assert_contains 'location.protocol === "https:" ? "wss:" : "ws:"' "$js_text" \
	"HTTPS dashboards must use secure WebSockets"
assert_contains 'reconnectDelay = Math.min(reconnectDelay * 2, 30000)' "$js_text" \
	"disconnected browsers must reconnect with bounded backoff"
assert_contains 'armConnectionTimer(connection, 15000)' "$js_text" \
	"browsers must time out stalled WebSocket handshakes"
assert_contains 'armConnectionTimer(connection, 45000)' "$js_text" \
	"browsers must detect a stalled open gateway"
assert_contains 'if (socket !== connection) return' "$js_text" \
	"stale socket callbacks must not overwrite the current connection state"
assert_contains 'if (payload.type === "heartbeat") return' "$js_text" \
	"application heartbeats must refresh liveness without rendering events"
assert_contains 'disconnectLabel = "Xymond unavailable"' "$js_text" \
	"xymond channel loss must have a distinct browser status"
assert_contains 'payload.type === "xymond" && payload.state === "alive"' "$js_text" \
	"xymond heartbeat recovery must restore Live status"
assert_contains 'if (disconnectLabel === "Xymond unavailable") setConnection("offline", disconnectLabel)' "$js_text" \
	"xymond-unavailable status must remain visible during reconnect attempts"
assert_contains 'generation !== payload.generation' "$js_text" \
	"a gateway restart must reset stale browser history"
assert_contains 'generation + ":" + event.sequence' "$js_text" \
	"ring replay must be deduplicated by generation and sequence"
assert_contains 'events.length > 500' "$js_text" \
	"browser transition history must stay bounded"
assert_contains '[25, 50, 100, 250, 500].indexOf(storedLimit)' "$js_text" \
	"stored row limits must stay within the supported 500-event history"
assert_contains 'sessionStorage.setItem("xymonlive-event-limit"' "$js_text" \
	"the client-side row limit must survive reloads"
assert_contains 'paused ? "Resume live updates and reconnect" : "Pause live updates and reconnect attempts"' "$js_text" \
	"Pause and Resume must expose state-specific hover help"
assert_contains 'if (seenOrder.length > 500) seen.delete(seenOrder.shift())' "$js_text" \
	"replay deduplication must remain independently bounded across Clear cycles"
assert_contains 'events = []; renderEvents();' "$js_text" \
	"clearing the view must retain replay deduplication state"
assert_not_contains 'setInterval(' "$js_text" \
	"the changes-only client must not poll"
assert_contains 'encodeURIComponent(host)' "$js_text" \
	"transition links must safely encode host and service names"
assert_contains 'service, document.createTextNode(" | ")' "$js_text" \
	"transitions must separate the test and event with a second pipe"
assert_contains 'new RegExp(input.value, "i")' "$js_text" \
	"recent changes must retain case-insensitive regular-expression filtering"
assert_contains 'return matchesKind(event) && matchesRegex(event); }).slice(0, displayLimit)' "$js_text" \
	"Type and Filter must run before the display limit"
assert_contains 'regexFilter.test(item.message || "")' "$js_text" \
	"Filter must match the status message when requested"
assert_contains '" — " + event.message' "$js_text" \
	"recent changes must display the status message first line"
assert_contains 'event.kind === "ack"' "$js_text" \
	"acknowledgements must render distinctly from color transitions"
assert_contains 'event.kind === "disable"' "$js_text" \
	"disable comments must render distinctly from color transitions"
assert_contains 'id=\"message-display\"' "$source_text" \
	"recent changes must provide a message visibility toggle"
assert_contains 'showMessages && event.message' "$js_text" \
	"message visibility must not remove messages from client memory"
assert_contains 'sessionStorage.setItem("xymonlive-show-messages"' "$js_text" \
	"message visibility must survive reloads"
assert_contains 'showMessages ? "Hide status message text from recent changes" : "Show status message text in recent changes"' "$js_text" \
	"the message toggle must expose state-specific hover help"
assert_not_contains 'searchFilter' "$js_text" \
	"the removed Search control must leave no duplicate filter state"
assert_contains '/\/static\/$/.test(assetBase.pathname) ? assetBase : new URL("static/", assetBase)' "$js_text" \
	"static icons must work when XYMONSKIN is either the parent or static skin"
assert_contains 'image.src = new URL(color + ".gif", staticAssetBase).href' "$js_text" \
	"color icons must use Xymon's configured non-animated GIF directory"
assert_contains 'sessionStorage.setItem("xymonlive-color-icons"' "$js_text" \
	"the color display choice must survive a page reload"
assert_contains 'panel.classList.toggle("folded", folded)' "$js_text" \
	"folding must apply compact panel styling"
assert_contains 'showColorIcons ? "Show transition colors as text" : "Show transition colors as static GIFs"' "$js_text" \
	"the color toggle must expose state-specific hover help"
assert_contains 'title=\"Clear recent changes from this browser view\"' "$source_text" \
	"Clear must explain that it affects this browser view"
assert_contains 'button.dataset.target === "controls-content" ? "event controls" : "recent changes list"' "$js_text" \
	"fold controls must expose the correct panel name"
assert_contains 'sessionStorage.setItem("xymonlive-fold-" + button.dataset.target' "$js_text" \
	"fold state must survive a page reload"

assert_contains '.workspace.changes-only { grid-template-columns: minmax(0, 1fr); }' "$css_text" \
	"the changes-only workspace must use the full available width"
assert_contains '.changes-only #events { height: auto; max-height: none; overflow-y: visible; }' "$css_text" \
	"recent changes must grow without an internal vertical scrollbar"
assert_contains '.transition .color-icon { width: 16px; height: 16px;' "$css_text" \
	"static color GIFs must have stable dimensions"
assert_contains 'time.classList.toggle("recent", (Date.now() / 1000 - event.time) < 300)' "$js_text" \
	"timestamps newer than five minutes must be marked recent"
assert_contains '#events time.recent { color: var(--text); font-weight: 700; }' "$css_text" \
	"recent timestamps must render in bold"
assert_contains 'function scheduleAgeRefresh()' "$js_text" \
	"timestamp age styling must refresh without a new event"
assert_contains '#connection-dot.offline ~ #connection-text, #connection-dot.offline ~ time { color: var(--red);' "$css_text" \
	"disconnected status text and time must be visibly red"
assert_contains '.live-actions { margin-left: auto; }' "$css_text" \
	"the moved Xymon Live controls must remain right-aligned"
assert_contains '.section-heading { display: flex; align-items: center; justify-content: space-between; min-height: 38px;' "$css_text" \
	"expanded panel headings must remain as compact as folded headings"
assert_contains '#event-count { display: none; }' "$css_text" \
	"the secondary row count must not overflow minimum-width mobile headings"
assert_contains '#pause { align-self: end; }' "$css_text" \
	"Pause must align with the mobile Rows select instead of its label"
assert_not_contains 'min-width: 320px' "$css_text" \
	"the page must not force horizontal scrolling inside a 320px viewport"
assert_contains 'title=\"Local browser time\"' "$source_text" \
	"the Live clock must identify which time it displays"
assert_contains 'display.textContent = dateTime(now.getTime() / 1000)' "$js_text" \
	"the Live clock must display the current browser date and time"
assert_contains 'setTimeout(updateClock, 1000 - now.getMilliseconds())' "$js_text" \
	"the Live clock must advance on aligned second boundaries"
assert_not_contains 'dateTime(payload.time)' "$js_text" \
	"the Live clock must not freeze on the most recent event time"

assert_contains '[xymonlive-websocket]' "$tasks_text" \
	"xymonlaunch must start the transition gateway"
assert_contains 'xymond_channel --channel=stachg' "$websocket_task" \
	"the gateway must consume only actual status changes"
assert_contains '--listen=127.0.0.1 --port=$XYMONLIVEWEBSOCKETPORT' "$websocket_task" \
	"the gateway must remain on its configured loopback port"
assert_contains '--origin=$XYMONWEBHOST' "$websocket_task" \
	"the gateway must validate the complete configured browser origin"
assert_not_contains 'checkpoint' "$websocket_task" \
	"the in-memory gateway must not add checkpoint I/O"
for apache in "$apache_open" "$apache_secure"; do
	assert_contains '<IfModule proxy_wstunnel_module>' "$apache" \
		"Apache WebSocket proxying must be optional when the module is absent"
	assert_contains 'ProxyPreserveHost On' "$apache" \
		"the gateway must receive the public Host used by same-origin validation"
	assert_contains 'ws://127.0.0.1:@XYMONLIVEWEBSOCKETPORT@/xymonlive' "$apache" \
		"Apache must proxy the public endpoint to the loopback gateway"
done

if command -v node >/dev/null 2>&1; then
	node --check "$JS" || fail "xymonlive browser JavaScript has a syntax error"
fi

pass "xymonlive renders only the bounded, reconnecting WebSocket transition stream"
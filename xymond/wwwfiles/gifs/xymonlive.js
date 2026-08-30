(function () {
	"use strict";

	var events = [];
	var seen = new Set();
	var seenOrder = [];
	var generation = null;
	var socket = null;
	var reconnectTimer = null;
	var connectionTimer = null;
	var reconnectDelay = 1000;
	var disconnectLabel = "Disconnected";
	var paused = false;
	var regexFilter = null;
	var regexScope = "any";
	var eventKind = "all";
	var displayLimit = 100;
	var showMessages = true;
	var showColorIcons = false;
	var root = document.querySelector("main");
	var assetBase = new URL(".", document.currentScript.src);
	var staticAssetBase = /\/static\/$/.test(assetBase.pathname) ? assetBase : new URL("static/", assetBase);

	function escapeText(value) { return String(value == null ? "" : value); }
	function clock(epoch) { return new Date(epoch * 1000).toLocaleTimeString([], { hour12: false }); }
	function dateTime(epoch) {
		var date = new Date(epoch * 1000);
		var part = function (value) { return String(value).padStart(2, "0"); };
		return date.getFullYear() + "-" + part(date.getMonth() + 1) + "-" + part(date.getDate()) + " " + clock(epoch);
	}
	function updateClock() {
		var now = new Date();
		var display = document.getElementById("updated");
		display.dateTime = now.toISOString();
		display.textContent = dateTime(now.getTime() / 1000);
		setTimeout(updateClock, 1000 - now.getMilliseconds());
	}
	function statusUrl(host, test) {
		return root.dataset.cgiUrl + "/svcstatus.sh?HOST=" + encodeURIComponent(host) + "&SERVICE=" + encodeURIComponent(test);
	}
	function matchesRegex(item) {
		if (!regexFilter) return true;
		if (regexScope === "host") return regexFilter.test(item.host);
		if (regexScope === "service") return regexFilter.test(item.test);
		if (regexScope === "message") return regexFilter.test(item.message || "");
		return regexFilter.test(item.host) || regexFilter.test(item.test) || regexFilter.test(item.message || "");
	}
	function matchesKind(item) { return eventKind === "all" || (item.kind || "status") === eventKind; }
	function colorDisplay(color) {
		if (showColorIcons) {
			var image = document.createElement("img");
			image.className = "color-icon";
			image.src = new URL(color + ".gif", staticAssetBase).href;
			image.alt = color;
			image.title = color;
			return image;
		}
		var label = document.createElement("b");
		label.className = color;
		label.textContent = color;
		return label;
	}
	function renderEvents() {
		var list = document.getElementById("events");
		var visible = events.filter(function (event) { return matchesKind(event) && matchesRegex(event); }).slice(0, displayLimit);
		list.replaceChildren();
		visible.forEach(function (event) {
			var item = document.createElement("li");
			var time = document.createElement("time");
			var compactTime = document.createElement("span");
			var fullTime = document.createElement("span");
			var text = document.createElement("p");
			var host = document.createElement("a");
			var service = document.createElement("a");
			time.dateTime = new Date(event.time * 1000).toISOString();
			compactTime.className = "compact-time";
			compactTime.textContent = dateTime(event.time);
			fullTime.className = "full-time";
			fullTime.textContent = dateTime(event.time);
			time.append(compactTime, fullTime);
			time.classList.toggle("recent", (Date.now() / 1000 - event.time) < 300);
			text.className = "transition";
			host.href = statusUrl(event.host, "info");
			host.textContent = escapeText(event.host);
			service.href = statusUrl(event.host, event.test);
			service.textContent = escapeText(event.test);
			text.append(host, document.createTextNode(" | "), service, document.createTextNode(" | "));
			if (event.kind === "ack") text.append(document.createTextNode("Acknowledged"));
			else if (event.kind === "disable") text.append(document.createTextNode("Disabled"));
			else text.append(colorDisplay(event.previousColor), document.createTextNode(" -> "), colorDisplay(event.color));
			if (showMessages && event.message) text.append(document.createTextNode(" — " + event.message));
			item.append(time, text);
			list.append(item);
		});
		document.getElementById("event-count").textContent = visible.length + " shown";
		document.getElementById("empty").hidden = visible.length !== 0;
	}
	function addEvent(event) {
		var key = generation + ":" + event.sequence;
		if (seen.has(key) || ((event.kind || "status") === "status" && event.previousColor === event.color)) return;
		seen.add(key);
		seenOrder.push(key);
		if (seenOrder.length > 500) seen.delete(seenOrder.shift());
		event._key = key;
		events.unshift(event);
		if (events.length > 500) events.pop();
		renderEvents();
	}
	function websocketUrl() {
		var scheme = location.protocol === "https:" ? "wss:" : "ws:";
		return scheme + "//" + location.host + root.dataset.websocketPath;
	}
	function setConnection(state, label) {
		document.getElementById("connection-dot").className = state;
		document.getElementById("connection-text").textContent = label;
	}
	function clearConnectionTimer() {
		if (connectionTimer) clearTimeout(connectionTimer);
		connectionTimer = null;
	}
	function armConnectionTimer(connection, delay) {
		clearConnectionTimer();
		connectionTimer = setTimeout(function () {
			if (paused || socket !== connection) return;
			socket = null;
			setConnection("offline", "Disconnected");
			connection.close();
			scheduleReconnect();
		}, delay);
	}
	function scheduleReconnect() {
		if (paused || reconnectTimer) return;
		reconnectTimer = setTimeout(function () {
			reconnectTimer = null;
			connect();
		}, reconnectDelay);
		reconnectDelay = Math.min(reconnectDelay * 2, 30000);
	}
	function scheduleAgeRefresh() {
		setTimeout(function () {
			if (!document.hidden) renderEvents();
			scheduleAgeRefresh();
		}, 30000);
	}
	function connect() {
		var connection;
		if (paused || socket) return;
		if (disconnectLabel === "Xymond unavailable") setConnection("offline", disconnectLabel);
		else setConnection("", "Connecting");
		connection = new WebSocket(websocketUrl());
		socket = connection;
		armConnectionTimer(connection, 15000);
		connection.addEventListener("open", function () {
			if (socket !== connection) return;
			if (reconnectTimer) clearTimeout(reconnectTimer);
			reconnectTimer = null;
			reconnectDelay = 1000;
			armConnectionTimer(connection, 45000);
		});
		connection.addEventListener("message", function (message) {
			var payload;
			if (socket !== connection) return;
			armConnectionTimer(connection, 45000);
			try { payload = JSON.parse(message.data); }
			catch (failure) { return; }
			if (payload.type === "heartbeat") return;
			if (payload.type === "xymond" && payload.state === "unavailable") {
				disconnectLabel = "Xymond unavailable";
				setConnection("offline", disconnectLabel);
				return;
			}
			if (payload.type === "xymond" && payload.state === "alive") {
				disconnectLabel = "Disconnected";
				setConnection("online", "Live");
				return;
			}
			if (payload.type === "hello") {
				disconnectLabel = "Disconnected";
				setConnection("online", "Live");
				if (generation && generation !== payload.generation) {
					events = [];
					seen.clear();
					seenOrder = [];
					renderEvents();
				}
				generation = payload.generation;
				return;
			}
			if (payload.type === "change" && generation) {
				addEvent(payload);
			}
		});
		connection.addEventListener("close", function () {
			if (socket !== connection) return;
			clearConnectionTimer();
			socket = null;
			if (!paused) { setConnection("offline", disconnectLabel); scheduleReconnect(); }
		});
		connection.addEventListener("error", function () { connection.close(); });
	}
	function updateRegexFilter() {
		var input = document.getElementById("regex-filter");
		var error = document.getElementById("regex-error");
		regexScope = document.getElementById("regex-scope").value;
		try {
			regexFilter = input.value ? new RegExp(input.value, "i") : null;
			input.classList.remove("invalid");
			input.removeAttribute("aria-invalid");
			error.hidden = true;
		}
		catch (failure) {
			regexFilter = null;
			input.classList.add("invalid");
			input.setAttribute("aria-invalid", "true");
			error.textContent = failure.message;
			error.hidden = false;
		}
		try {
			sessionStorage.setItem("xymonlive-regex", input.value);
			sessionStorage.setItem("xymonlive-regex-scope", regexScope);
		}
		catch (failure) {}
		renderEvents();
	}
	function setFolded(button, folded) {
		var content = document.getElementById(button.dataset.target);
		var panel = content.closest(".controls-panel, aside");
		var panelName = button.dataset.target === "controls-content" ? "event controls" : "recent changes list";
		content.hidden = folded;
		panel.classList.toggle("folded", folded);
		button.setAttribute("aria-expanded", String(!folded));
		button.setAttribute("aria-label", (folded ? "Unfold " : "Fold ") + panelName);
		button.title = button.getAttribute("aria-label");
		try { sessionStorage.setItem("xymonlive-fold-" + button.dataset.target, folded ? "1" : "0"); } catch (failure) {}
	}

	try {
		document.getElementById("regex-filter").value = sessionStorage.getItem("xymonlive-regex") || "";
		var storedScope = sessionStorage.getItem("xymonlive-regex-scope") || "any";
		if (storedScope === "either") storedScope = "any";
		document.getElementById("regex-scope").value = storedScope;
		var storedKind = sessionStorage.getItem("xymonlive-event-kind") || "all";
		if (["all", "status", "disable", "ack"].indexOf(storedKind) !== -1) eventKind = storedKind;
		var storedLimit = Number(sessionStorage.getItem("xymonlive-event-limit"));
		if ([25, 50, 100, 250, 500].indexOf(storedLimit) !== -1) displayLimit = storedLimit;
		showMessages = sessionStorage.getItem("xymonlive-show-messages") !== "0";
		showColorIcons = sessionStorage.getItem("xymonlive-color-icons") === "1";
	}
	catch (failure) {}
	document.getElementById("regex-filter").addEventListener("input", updateRegexFilter);
	document.getElementById("regex-scope").addEventListener("change", updateRegexFilter);
	document.getElementById("event-kind").value = eventKind;
	document.getElementById("event-kind").addEventListener("change", function (event) {
		eventKind = event.target.value;
		try { sessionStorage.setItem("xymonlive-event-kind", eventKind); } catch (failure) {}
		renderEvents();
	});
	document.getElementById("event-limit").value = String(displayLimit);
	document.getElementById("event-limit").addEventListener("change", function (event) {
		displayLimit = Number(event.target.value);
		try { sessionStorage.setItem("xymonlive-event-limit", String(displayLimit)); } catch (failure) {}
		renderEvents();
	});
	document.getElementById("pause").addEventListener("click", function (event) {
		var connection;
		paused = !paused;
		event.currentTarget.textContent = paused ? "Resume" : "Pause";
		event.currentTarget.title = paused ? "Resume live updates and reconnect" : "Pause live updates and reconnect attempts";
		if (paused) {
			clearTimeout(reconnectTimer);
			reconnectTimer = null;
			clearConnectionTimer();
			connection = socket;
			socket = null;
			if (connection) connection.close();
			setConnection("", "Paused");
		}
		else connect();
	});
	document.getElementById("message-display").addEventListener("click", function (event) {
		showMessages = !showMessages;
		event.currentTarget.setAttribute("aria-pressed", String(showMessages));
		event.currentTarget.title = showMessages ? "Hide status message text from recent changes" : "Show status message text in recent changes";
		try { sessionStorage.setItem("xymonlive-show-messages", showMessages ? "1" : "0"); } catch (failure) {}
		renderEvents();
	});
	document.getElementById("message-display").setAttribute("aria-pressed", String(showMessages));
	document.getElementById("message-display").title = showMessages ? "Hide status message text from recent changes" : "Show status message text in recent changes";
	document.getElementById("color-display").addEventListener("click", function (event) {
		showColorIcons = !showColorIcons;
		event.currentTarget.setAttribute("aria-pressed", String(showColorIcons));
		event.currentTarget.textContent = showColorIcons ? "Text" : "GIFs";
		event.currentTarget.title = showColorIcons ? "Show transition colors as text" : "Show transition colors as static GIFs";
		try { sessionStorage.setItem("xymonlive-color-icons", showColorIcons ? "1" : "0"); } catch (failure) {}
		renderEvents();
	});
	document.getElementById("color-display").setAttribute("aria-pressed", String(showColorIcons));
	document.getElementById("color-display").textContent = showColorIcons ? "Text" : "GIFs";
	document.getElementById("color-display").title = showColorIcons ? "Show transition colors as text" : "Show transition colors as static GIFs";
	document.getElementById("clear").addEventListener("click", function () { events = []; renderEvents(); });
	document.querySelectorAll(".fold").forEach(function (button) {
		var folded = false;
		try { folded = sessionStorage.getItem("xymonlive-fold-" + button.dataset.target) === "1"; } catch (failure) {}
		setFolded(button, folded);
		button.addEventListener("click", function () { setFolded(button, button.getAttribute("aria-expanded") === "true"); });
	});
	document.addEventListener("visibilitychange", function () { if (!document.hidden && !paused) connect(); });

	updateRegexFilter();
	renderEvents();
	updateClock();
	connect();
	scheduleAgeRefresh();
}());
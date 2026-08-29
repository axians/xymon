(function () {
	"use strict";

	var active = new Map();
	var events = [];
	var initialized = false;
	var paused = false;
	var timer = null;
	var refreshInterval = 5000;
	var colorFilter = "all";
	var searchFilter = "";
	var regexFilter = null;
	var regexScope = "either";
	var showColorIcons = false;
	var assetBase = new URL(".", document.currentScript.src);
	var colors = ["red", "yellow", "purple", "blue"];
	var body = document.getElementById("status-body");
	var empty = document.getElementById("empty");

	function keyOf(status) { return status.host + "\u0000" + status.test; }
	function escapeText(value) { return String(value == null ? "" : value); }
	function clock(epoch) { return new Date(epoch * 1000).toLocaleTimeString([], { hour12: false }); }
	function dateTime(epoch) {
		var date = new Date(epoch * 1000);
		var part = function (value) { return String(value).padStart(2, "0"); };
		return date.getFullYear() + "-" + part(date.getMonth() + 1) + "-" + part(date.getDate()) + " " + clock(epoch);
	}
	function age(epoch) {
		var seconds = Math.max(0, Math.floor(Date.now() / 1000) - epoch);
		if (seconds < 60) return seconds + "s";
		if (seconds < 3600) return Math.floor(seconds / 60) + "m";
		if (seconds < 86400) return Math.floor(seconds / 3600) + "h " + Math.floor((seconds % 3600) / 60) + "m";
		return Math.floor(seconds / 86400) + "d " + Math.floor((seconds % 86400) / 3600) + "h";
	}

	function addEvent(status, previousColor, color) {
		events.unshift({ time: Math.floor(Date.now() / 1000), host: status.host, test: status.test, hostUrl: status.hostUrl, serviceUrl: status.serviceUrl, previousColor: previousColor, color: color });
		if (events.length > 120) events.length = 120;
	}

	function seedEvents(seed, statuses) {
		var urls = new Map((statuses || []).map(function (status) { return [keyOf(status), status]; }));
		if (initialized || events.length || !Array.isArray(seed)) return;
		events = seed.filter(function (event) {
			return event && event.host && event.test && event.previousColor && event.color && event.previousColor !== event.color && Number.isFinite(event.time);
		}).map(function (event) {
			var status = urls.get(keyOf(event));
			if (status) {
				event.hostUrl = event.hostUrl || status.hostUrl;
				event.serviceUrl = event.serviceUrl || status.serviceUrl;
			}
			return event;
		}).slice(0, 120);
	}

	function matchesRegex(item) {
		if (!regexFilter) return true;
		if (regexScope === "host") return regexFilter.test(item.host);
		if (regexScope === "service") return regexFilter.test(item.test);
		return regexFilter.test(item.host) || regexFilter.test(item.test);
	}

	function colorDisplay(color) {
		if (showColorIcons) {
			var image = document.createElement("img");
			image.className = "color-icon";
			image.src = new URL("static/" + color + ".gif", assetBase).href;
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
		list.replaceChildren();
		events.filter(matchesRegex).forEach(function (event) {
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
			text.className = "transition";
			host.href = event.hostUrl;
			host.textContent = event.host;
			service.href = event.serviceUrl;
			service.textContent = event.test;
			text.append(host, document.createTextNode(" / "), service, document.createTextNode(" "));
			text.append(colorDisplay(event.previousColor), document.createTextNode(" -> "), colorDisplay(event.color));
			item.append(time, text);
			list.append(item);
		});
	}

	function stateCell(status) {
		var wrap = document.createElement("span");
		wrap.className = "state";
		if (status.ackTime > 0) {
			var ack = document.createElement("span"); ack.className = "flag"; ack.textContent = "Ack";
			if (status.ackMessage) ack.title = status.ackMessage;
			wrap.append(ack);
		}
		if (status.disableTime > 0 || status.color === "blue") {
			var disabled = document.createElement("span"); disabled.className = "flag"; disabled.textContent = "Disabled";
			if (status.disableMessage) disabled.title = status.disableMessage;
			wrap.append(disabled);
		}
		if (!wrap.childNodes.length) wrap.textContent = "--";
		return wrap;
	}

	function render() {
		var query = searchFilter.toLowerCase();
		var visible = Array.from(active.values()).filter(function (status) {
			var matchesColor = colorFilter === "all" || status.color === colorFilter;
			var haystack = (status.host + " " + status.test + " " + status.summary).toLowerCase();
			return matchesColor && matchesRegex(status) && (!query || haystack.indexOf(query) !== -1);
		}).sort(function (left, right) {
			var severity = { red: 0, yellow: 1, purple: 2, blue: 3 };
			return severity[left.color] - severity[right.color] || left.host.localeCompare(right.host) || left.test.localeCompare(right.test);
		});

		body.replaceChildren();
		visible.forEach(function (status) {
			var row = document.createElement("tr");
			var statusCell = document.createElement("td");
			var dot = document.createElement("span"); dot.className = "dot " + status.color; dot.title = status.color; statusCell.append(dot);
			var host = document.createElement("td"); var hostLink = document.createElement("a"); hostLink.href = status.hostUrl; hostLink.textContent = escapeText(status.host); hostLink.title = escapeText(status.host); host.append(hostLink);
			var test = document.createElement("td"); var serviceLink = document.createElement("a"); serviceLink.href = status.serviceUrl; serviceLink.textContent = escapeText(status.test); test.append(serviceLink);
			var since = document.createElement("td"); since.textContent = age(status.lastChange); since.title = new Date(status.lastChange * 1000).toLocaleString();
			var state = document.createElement("td"); state.append(stateCell(status));
			var summary = document.createElement("td"); summary.textContent = escapeText(status.summary); summary.title = escapeText(status.summary);
			row.append(statusCell, host, test, since, state, summary);
			body.append(row);
		});

		document.getElementById("visible-count").textContent = visible.length + " shown";
		empty.hidden = visible.length !== 0;
		colors.forEach(function (color) {
			var count = Array.from(active.values()).filter(function (item) { return item.color === color; }).length;
			var output = document.getElementById("count-" + color);
			output.textContent = count;
			output.dataset.digits = String(count).length;
		});
		renderEvents();
	}

	function merge(statuses) {
		var next = new Map();
		statuses.forEach(function (status) {
			var key = keyOf(status);
			var previous = active.get(key);
			next.set(key, status);
			if (initialized && previous && previous.color !== status.color) addEvent(status, previous.color, status.color);
			else if (initialized && !previous) addEvent(status, "green", status.color);
		});
		if (initialized) active.forEach(function (status, key) {
			if (!next.has(key)) addEvent(status, status.color, "green");
		});
		active = next;
		initialized = true;
	}

	async function refresh() {
		if (paused) return;
		var dot = document.getElementById("connection-dot");
		var label = document.getElementById("connection-text");
		var error = document.getElementById("error");
		try {
			var response = await fetch("?data=1", { cache: "no-store", headers: { Accept: "application/json" } });
			if (!response.ok) throw new Error("Status request failed (HTTP " + response.status + ")");
			var payload = await response.json();
			seedEvents(payload.events, payload.statuses);
			merge(payload.statuses || []);
			render();
			dot.className = "online";
			label.textContent = "Live";
			document.getElementById("updated").textContent = clock(payload.generated);
			error.hidden = true;
		} catch (failure) {
			dot.className = "offline";
			label.textContent = "Disconnected";
			error.textContent = failure.message;
			error.hidden = false;
		}
	}

	function schedule() {
		clearInterval(timer);
		timer = setInterval(refresh, refreshInterval);
	}

	function setLayout(layout) {
		var workspace = document.querySelector(".workspace");
		var layouts = ["side-by-side", "changes-left", "active-above", "changes-above"];
		if (layouts.indexOf(layout) === -1) layout = "side-by-side";
		workspace.className = "workspace layout-" + layout;
		document.getElementById("layout").value = layout;
		try { sessionStorage.setItem("xymonlive-layout", layout); } catch (failure) {}
	}

	function dropPosition(event, workspace) {
		var bounds = workspace.getBoundingClientRect();
		var top = Math.max(0, bounds.top);
		var bottom = Math.min(window.innerHeight, bounds.bottom);
		var horizontal = (event.clientX - bounds.left) / bounds.width * 2 - 1;
		var vertical = (event.clientY - top) / Math.max(1, bottom - top) * 2 - 1;
		if (Math.abs(horizontal) > Math.abs(vertical)) return horizontal < 0 ? "left" : "right";
		return vertical < 0 ? "above" : "below";
	}

	function droppedLayout(panel, position) {
		if (position === "left") return panel === "status" ? "side-by-side" : "changes-left";
		if (position === "right") return panel === "status" ? "changes-left" : "side-by-side";
		if (position === "above") return panel === "status" ? "active-above" : "changes-above";
		return panel === "status" ? "changes-above" : "active-above";
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
		} catch (failure) {
			regexFilter = null;
			input.classList.add("invalid");
			input.setAttribute("aria-invalid", "true");
			error.textContent = failure.message;
			error.hidden = false;
		}
		try {
			sessionStorage.setItem("xymonlive-regex", input.value);
			sessionStorage.setItem("xymonlive-regex-scope", regexScope);
		} catch (failure) {}
		render();
	}

	function setFolded(button, folded) {
		var content = document.getElementById(button.dataset.target);
		var panel = content.closest(".controls-panel, .status-panel, aside");
		var panelNames = { events: "recent changes", "status-content": "active statuses", "toolbar-content": "search and filter" };
		var panelName = panelNames[button.dataset.target];
		content.hidden = folded;
		panel.classList.toggle("folded", folded);
		button.setAttribute("aria-expanded", String(!folded));
		button.setAttribute("aria-label", (folded ? "Unfold " : "Fold ") + panelName);
		button.title = button.getAttribute("aria-label");
		try { sessionStorage.setItem("xymonlive-fold-" + button.dataset.target, folded ? "1" : "0"); } catch (failure) {}
	}

	document.querySelectorAll(".metric").forEach(function (button) {
		button.addEventListener("click", function () {
			var selected = button.classList.contains("selected");
			document.querySelectorAll(".metric").forEach(function (item) { item.classList.remove("selected"); });
			if (!selected) button.classList.add("selected");
			colorFilter = selected ? "all" : button.dataset.color;
			render();
		});
	});
	document.getElementById("search").addEventListener("input", function (event) { searchFilter = event.target.value; render(); });
	try {
		document.getElementById("regex-filter").value = sessionStorage.getItem("xymonlive-regex") || "";
		document.getElementById("regex-scope").value = sessionStorage.getItem("xymonlive-regex-scope") || "either";
	} catch (failure) {}
	document.getElementById("regex-filter").addEventListener("input", updateRegexFilter);
	document.getElementById("regex-scope").addEventListener("change", updateRegexFilter);
	updateRegexFilter();
	try { setLayout(sessionStorage.getItem("xymonlive-layout") || "side-by-side"); }
	catch (failure) { setLayout("side-by-side"); }
	document.getElementById("layout").addEventListener("change", function (event) { setLayout(event.target.value); });
	(function () {
		var workspace = document.querySelector(".workspace");
		document.querySelectorAll(".drag-handle").forEach(function (handle) {
			var startX;
			var startY;
			var dragging = false;
			handle.addEventListener("pointerdown", function (event) {
				if (event.button !== 0) return;
				startX = event.clientX;
				startY = event.clientY;
				handle.setPointerCapture(event.pointerId);
			});
			handle.addEventListener("pointermove", function (event) {
				if (!handle.hasPointerCapture(event.pointerId)) return;
				if (!dragging && Math.hypot(event.clientX - startX, event.clientY - startY) < 6) return;
				dragging = true;
				workspace.classList.add("dragging");
				workspace.dataset.dropPosition = dropPosition(event, workspace);
			});
			handle.addEventListener("pointerup", function (event) {
				if (dragging) setLayout(droppedLayout(handle.dataset.panel, dropPosition(event, workspace)));
				dragging = false;
				workspace.classList.remove("dragging");
				workspace.removeAttribute("data-drop-position");
			});
			handle.addEventListener("pointercancel", function () {
				dragging = false;
				workspace.classList.remove("dragging");
				workspace.removeAttribute("data-drop-position");
			});
		});
	}());
	document.querySelectorAll(".refresh-options button").forEach(function (button) {
		button.addEventListener("click", function () {
			refreshInterval = Number(button.dataset.interval);
			document.querySelectorAll(".refresh-options button").forEach(function (item) {
				item.setAttribute("aria-pressed", String(item === button));
			});
			schedule();
		});
	});
	document.getElementById("top-menu-toggle").addEventListener("click", function (event) {
		var open = event.currentTarget.getAttribute("aria-expanded") !== "true";
		document.querySelector(".top-heading").classList.toggle("menu-open", open);
		event.currentTarget.setAttribute("aria-expanded", String(open));
		event.currentTarget.setAttribute("aria-label", (open ? "Close" : "Open") + " top controls");
		event.currentTarget.title = event.currentTarget.getAttribute("aria-label");
	});
	document.getElementById("pause").addEventListener("click", function (event) {
		paused = !paused;
		event.target.textContent = paused ? "Resume" : "Pause";
		document.getElementById("connection-text").textContent = paused ? "Paused" : "Live";
		if (!paused) refresh();
	});
	document.getElementById("color-display").addEventListener("click", function (event) {
		showColorIcons = !showColorIcons;
		event.currentTarget.setAttribute("aria-pressed", String(showColorIcons));
		event.currentTarget.textContent = showColorIcons ? "Text" : "GIFs";
		event.currentTarget.title = showColorIcons ? "Show colors as text" : "Show colors as static GIFs";
		try { sessionStorage.setItem("xymonlive-color-icons", showColorIcons ? "1" : "0"); } catch (failure) {}
		renderEvents();
	});
	try { showColorIcons = sessionStorage.getItem("xymonlive-color-icons") === "1"; } catch (failure) {}
	document.getElementById("color-display").setAttribute("aria-pressed", String(showColorIcons));
	document.getElementById("color-display").textContent = showColorIcons ? "Text" : "GIFs";
	document.getElementById("color-display").title = showColorIcons ? "Show colors as text" : "Show colors as static GIFs";
	document.getElementById("clear").addEventListener("click", function () { events = []; renderEvents(); });
	document.querySelectorAll(".fold").forEach(function (button) {
		var folded = false;
		try { folded = sessionStorage.getItem("xymonlive-fold-" + button.dataset.target) === "1"; } catch (failure) {}
		setFolded(button, folded);
		button.addEventListener("click", function () { setFolded(button, button.getAttribute("aria-expanded") === "true"); });
	});
	document.addEventListener("visibilitychange", function () { if (!document.hidden && !paused) refresh(); });

	refresh();
	schedule();
}());
// Nexus Mod Manager front end. Talks to the Rust side through Tauri commands.
"use strict";

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...children) => {
	const node = document.createElement(tag);
	for (const [key, value] of Object.entries(props)) {
		if (key === "class") node.className = value;
		else if (key.startsWith("on")) node.addEventListener(key.slice(2), value);
		else if (value === true) node.setAttribute(key, "");
		else if (value !== false && value != null) node.setAttribute(key, value);
	}
	for (const child of children.flat()) {
		if (child != null && child !== false) node.append(child);
	}
	return node;
};

let state = null;          // Last overview from the backend.
let selected = null;       // Selected mod folder.
const outputs = {};        // Folder -> { title, text } of the last build/test output.
const logLines = [];       // Console lines.
const MAX_LOG = 5000;

// ---------- helpers ----------

let toastTimer = null;
function toast(message, isError = false) {
	const t = $("toast");
	t.textContent = message;
	t.className = "toast" + (isError ? " error" : "");
	t.hidden = false;
	clearTimeout(toastTimer);
	toastTimer = setTimeout(() => (t.hidden = true), isError ? 7000 : 3500);
}

async function call(command, args = {}, { quiet = false } = {}) {
	try {
		return await invoke(command, args);
	} catch (error) {
		if (!quiet) toast(String(error), true);
		throw error;
	}
}

// A small modal: returns the value of the input (or true) on OK, null on cancel.
function ask({ title, body = [], input = null, ok = "OK", danger = false }) {
	return new Promise((resolve) => {
		const dlg = $("dialog");
		$("dlg-title").textContent = title;
		const container = $("dlg-body");
		container.replaceChildren(...body.map((b) => (typeof b === "string" ? el("p", {}, b) : b)));
		let field = null;
		if (input) {
			field = el("input", {
				placeholder: input.placeholder || "",
				value: input.value || "",
				"aria-label": title,
				// Enter means OK. (The form's first button is Cancel, which Enter would otherwise press.)
				onkeydown: (e) => {
					if (e.key === "Enter") {
						e.preventDefault();
						dlg.close("ok");
					}
				},
			});
			container.append(field);
		}
		const okBtn = $("dlg-ok");
		okBtn.textContent = ok;
		okBtn.className = danger ? "danger" : "primary";
		dlg.onclose = () => {
			if (dlg.returnValue !== "ok") return resolve(null);
			resolve(field ? field.value.trim() : true);
		};
		dlg.returnValue = "";
		dlg.showModal();
		(field || okBtn).focus();
	});
}

function flagClass(w) {
	return "flag k-" + (w.blocking && w.kind !== "ai" ? "blocking" : w.kind);
}

// Short labels for the list; the detail panel shows the full text.
function shortLabel(w) {
	switch (w.kind) {
		case "ai": return "AI-made";
		case "sandbox": return "Not sandboxed";
		case "permissions":
			if (w.title.startsWith("Permissions approved")) return "Permissions";
			return w.title.startsWith("Running") ? "Not approved" : "Needs approval";
		case "unverified": return "Unverified";
		case "perf": return "Slow";
		case "untested": return "Untested";
		case "build": return w.blocking ? "Not built" : "Rebuild";
		default:
			if (w.title.startsWith("Failed")) return "Failed test";
			return w.blocking ? "Won't run" : "Check";
	}
}

function modByFolder(folder) {
	return state?.library.mods.find((m) => m.folder === folder);
}

function enabledOrder() {
	return state.library.mods.filter((m) => m.enabled).map((m) => m.folder);
}

// ---------- loading ----------

async function refresh() {
	state = await call("overview");
	render();
}

let dragging = false;

function render() {
	// Keep keyboard focus on the same mod across re-renders.
	const focused = document.activeElement?.closest?.(".stop")?.dataset.folder;
	if (!dragging) renderMods();
	renderDetail();
	renderProfiles();
	renderSettings();
	renderGame();
	if (focused) document.querySelector(`.stop[data-folder="${CSS.escape(focused)}"]`)?.focus();
}

// ---------- mods view ----------

function renderMods() {
	const lib = state.library;
	$("mods-folder").textContent = lib.mods_dir;
	$("no-order").hidden = lib.has_load_order || lib.mods.length === 0;

	const on = lib.mods.filter((m) => m.enabled);
	const off = lib.mods.filter((m) => !m.enabled);

	$("route").replaceChildren(...on.map((m, i) => stopRow(m, i, true)));
	$("offroute").replaceChildren(...off.map((m, i) => stopRow(m, i, false)));
	$("route-hint").hidden = on.length < 2;
	$("off-empty").hidden = off.length > 0 || lib.mods.length === 0;

	const empty = $("route-empty");
	empty.hidden = on.length > 0;
	if (lib.mods.length === 0) {
		empty.replaceChildren(
			el("p", {}, "No mods yet."),
			el("p", {}, "Add one from a folder, a zip or GitHub, or start your own with New mod."),
		);
	} else {
		empty.replaceChildren(el("p", {}, "No mods are turned on. Turn one on below to put it on the route."));
	}
	$("rail-foot").textContent = `${on.length} of ${lib.mods.length} mods on`;
}

function stopRow(mod, index, onRoute) {
	const blocking = mod.warnings.some((w) => w.blocking);
	const shown = mod.warnings.filter((w) => !(w.kind === "permissions" && mod.permissions_approved));
	const row = el(
		"li",
		{
			class: "stop" + (blocking ? " blocked" : ""),
			tabindex: "0",
			role: "option",
			"aria-selected": String(selected === mod.folder),
			draggable: onRoute ? "true" : "false",
			"data-folder": mod.folder,
			onclick: () => select(mod.folder),
			onkeydown: (e) => onRowKey(e, mod, onRoute),
		},
		el("span", { class: "ring", "aria-hidden": "true" }),
		el(
			"div",
			{ class: "body" },
			el("div", { class: "name" }, mod.manifest.name, mod.manifest.version ? el("small", {}, mod.manifest.version) : null),
			el("div", { class: "flags" }, shown.map((w) => el("span", { class: flagClass(w), title: w.title }, shortLabel(w)))),
		),
		el(
			"div",
			{ class: "side" },
			onRoute
				? null
				: el("button", { onclick: (e) => { e.stopPropagation(); turnOn(mod); } }, "Turn on"),
		),
	);
	if (onRoute) {
		row.addEventListener("dragstart", (e) => {
			row.classList.add("dragging");
			dragging = true;
			e.dataTransfer.setData("text/plain", mod.folder);
			e.dataTransfer.effectAllowed = "move";
		});
		row.addEventListener("dragend", () => {
			row.classList.remove("dragging");
			dragging = false;
		});
		row.addEventListener("dragover", (e) => {
			e.preventDefault();
			const after = e.offsetY > row.offsetHeight / 2;
			row.classList.toggle("drop-after", after);
			row.classList.toggle("drop-before", !after);
		});
		row.addEventListener("dragleave", () => row.classList.remove("drop-before", "drop-after"));
		row.addEventListener("drop", (e) => {
			e.preventDefault();
			const after = row.classList.contains("drop-after");
			row.classList.remove("drop-before", "drop-after");
			const moved = e.dataTransfer.getData("text/plain");
			dragging = false;
			if (!moved || moved === mod.folder) return;
			const order = enabledOrder().filter((f) => f !== moved);
			let target = order.indexOf(mod.folder);
			if (after) target += 1;
			order.splice(target, 0, moved);
			saveOrder(order, `Moved ${modByFolder(moved).manifest.name}.`);
		});
	}
	return row;
}

function onRowKey(e, mod, onRoute) {
	if (e.key === "Enter" || e.key === " ") {
		e.preventDefault();
		select(mod.folder);
		return;
	}
	// Alt+Up / Alt+Down moves a mod on the route.
	if (onRoute && e.altKey && (e.key === "ArrowUp" || e.key === "ArrowDown")) {
		e.preventDefault();
		const order = enabledOrder();
		const i = order.indexOf(mod.folder);
		const j = e.key === "ArrowUp" ? i - 1 : i + 1;
		if (j < 0 || j >= order.length) return;
		[order[i], order[j]] = [order[j], order[i]];
		saveOrder(order, `Moved ${mod.manifest.name} ${e.key === "ArrowUp" ? "up" : "down"}.`).then(() => {
			document.querySelector(`.stop[data-folder="${CSS.escape(mod.folder)}"]`)?.focus();
		});
		return;
	}
	if (e.key === "ArrowUp" || e.key === "ArrowDown") {
		e.preventDefault();
		const rows = [...document.querySelectorAll(".stop")];
		const i = rows.indexOf(e.currentTarget);
		rows[e.key === "ArrowUp" ? i - 1 : i + 1]?.focus();
	}
}

async function saveOrder(order, message) {
	try {
		await call("set_load_order", { enabled: order });
		await refresh();
		if (message) toast(message);
	} catch {
		await refresh();
	}
}

async function turnOn(mod) {
	if (mod.manifest.permissions.length && !mod.permissions_approved) {
		const ok = await approve(mod);
		if (!ok) return;
	}
	await saveOrder([...enabledOrder(), mod.folder], `${mod.manifest.name} is on. It runs last.`);
}

async function turnOff(mod) {
	await saveOrder(enabledOrder().filter((f) => f !== mod.folder), `${mod.manifest.name} is off.`);
}

const PERMISSION_TEXT = {
	"conditions-write": "Change game conditions, which can affect missions, events and your save.",
	"network": "Connect to the internet.",
	"file-read": "Read files on your computer.",
	"file-write": "Write files on your computer.",
	"mods-data": "Read other mods' data.",
};

async function approve(mod) {
	const list = el("ul", {}, mod.manifest.permissions.map((p) => el("li", {}, el("strong", {}, p), PERMISSION_TEXT[p] ? ": " + PERMISSION_TEXT[p] : "")));
	const ok = await ask({
		title: `Allow ${mod.manifest.name} to do this?`,
		body: [list, "You'll be asked again if a new version wants more."],
		ok: "Allow",
	});
	if (!ok) return false;
	await call("approve_permissions", { folder: mod.folder });
	await refresh();
	return true;
}

function select(folder) {
	selected = folder;
	document.querySelectorAll(".stop").forEach((r) => r.setAttribute("aria-selected", String(r.dataset.folder === folder)));
	renderDetail();
}

// ---------- detail ----------

function renderDetail() {
	const mod = selected && modByFolder(selected);
	$("detail-empty").hidden = !!mod;
	$("detail-body").hidden = !mod;
	if (!mod) return;
	const m = mod.manifest;

	$("d-name").textContent = m.name;
	$("d-version").textContent = m.version ? `version ${m.version}` : "";
	const meta = [];
	if (m.authors.length) meta.push(`by ${m.authors.join(", ")}`);
	meta.push(mod.is_link ? `linked from ${mod.path}` : mod.folder);
	$("d-meta").textContent = meta.join(", ");
	$("d-desc").textContent = mod.manifest_error ? "" : m.description || "No description.";

	const primary = [];
	if (mod.enabled) primary.push(el("button", { onclick: () => turnOff(mod) }, "Turn off"));
	else primary.push(el("button", { class: "primary", onclick: () => turnOn(mod) }, "Turn on"));
	if (m.permissions.length && !mod.permissions_approved) primary.push(el("button", { onclick: () => approve(mod) }, "Review permissions"));
	if (mod.can_build) primary.push(el("button", { onclick: (e) => build(mod, e.currentTarget) }, mod.needs_build ? "Build" : "Rebuild"));
	if (mod.wasm_exists) primary.push(el("button", { onclick: (e) => test(mod, e.currentTarget) }, "Test"));
	$("d-primary").replaceChildren(...primary);

	const warnings = mod.warnings.map((w) =>
		el("li", { class: flagClass(w).replace("flag ", "") }, el("strong", {}, w.title), el("span", {}, w.detail)),
	);
	if (!warnings.length) warnings.push(el("li", { class: "none" }, el("strong", {}, "Nothing to warn about"), el("span", {}, "Signed, tested, and asks for no permissions.")));
	$("d-warnings").replaceChildren(...warnings);

	const t = mod.test;
	if (!t) $("d-test").textContent = "Not tested yet.";
	else {
		const when = new Date(t.when * 1000).toLocaleString();
		const timing = t.ok ? ` Average ${t.average_ms.toFixed(3)} ms per frame, peak ${t.peak_ms.toFixed(2)} ms.` : "";
		const stale = mod.test_current ? "" : " The mod has been rebuilt since.";
		$("d-test").replaceChildren(el("span", { class: t.ok ? "k-ok" : "k-compat" }, t.summary), timing + stale + ` Tested ${when}.`);
	}

	const actions = [el("button", { onclick: () => call("open_in_editor", { folder: mod.folder }) }, "Open in editor")];
	actions.push(el("button", { onclick: () => call("open_path", { path: mod.path }) }, "Show folder"));
	if (m.homepage) actions.push(el("button", { onclick: () => call("open_path", { path: m.homepage }) }, "Homepage"));
	if (mod.has_backups) actions.push(el("button", { onclick: () => rollback(mod) }, "Roll back"));
	actions.push(el("button", { class: "danger", onclick: () => remove(mod) }, mod.is_link ? "Unlink" : "Remove"));
	$("d-actions").replaceChildren(...actions);

	const out = outputs[mod.folder];
	$("d-output-wrap").hidden = !out;
	if (out) {
		$("d-output-title").textContent = out.title;
		$("d-output").textContent = out.text || "(no output)";
	}
}

async function busy(button, label, work) {
	const original = button?.textContent;
	if (button) {
		button.disabled = true;
		button.textContent = label;
	}
	try {
		return await work();
	} finally {
		if (button) {
			button.disabled = false;
			button.textContent = original;
		}
	}
}

async function build(mod, button) {
	const result = await busy(button, "Building…", () => call("build_mod", { folder: mod.folder }));
	outputs[mod.folder] = { title: result.ok ? "Build output" : "Build failed", text: result.output };
	await refresh();
	if (result.ok) toast(`Built ${mod.manifest.name}.`);
	else {
		toast(`${mod.manifest.name} didn't build. The output shows why.`, true);
		$("d-output-wrap").open = true;
	}
}

async function test(mod, button) {
	if (!state.modcheck_ready) {
		const ok = await ask({
			title: "Build the test tool first",
			body: ["Testing needs a small tool from your game checkout. Building it takes a minute or two."],
			ok: "Build it now",
		});
		if (!ok) return;
		const result = await busy(button, "Building test tool…", () => call("build_test_tool"));
		if (!result.ok) {
			outputs[mod.folder] = { title: "Test tool build failed", text: result.output };
			renderDetail();
			return toast("The test tool didn't build. The output shows why.", true);
		}
		await refresh();
	}
	try {
		const record = await busy(button, "Testing…", () => call("test_mod", { folder: mod.folder }));
		outputs[mod.folder] = { title: "Test output", text: record.output };
		toast(record.summary, !record.ok);
	} finally {
		await refresh();
	}
}

async function rollback(mod) {
	const ok = await ask({ title: `Roll back ${mod.manifest.name}?`, body: ["The previous version comes back. The current one is kept, so you can switch again."], ok: "Roll back" });
	if (!ok) return;
	const version = await call("rollback_mod", { folder: mod.folder });
	await refresh();
	toast(`${mod.manifest.name} is back to ${version === "unknown" ? "the previous version" : "version " + version}.`);
}

async function remove(mod) {
	const ok = await ask({
		title: mod.is_link ? `Unlink ${mod.manifest.name}?` : `Remove ${mod.manifest.name}?`,
		body: [mod.is_link
			? "The game stops seeing it. Your working folder isn't touched."
			: "It's moved to the backups, so Add mod or Roll back can bring it back."],
		ok: mod.is_link ? "Unlink" : "Remove",
		danger: true,
	});
	if (!ok) return;
	await call("remove_mod", { folder: mod.folder });
	if (selected === mod.folder) selected = null;
	await refresh();
	toast(`${mod.is_link ? "Unlinked" : "Removed"} ${mod.manifest.name}.`);
}

// ---------- adding mods ----------

async function addMod(kind) {
	let source = null;
	if (kind === "folder" || kind === "link") source = await call("pick_folder");
	else if (kind === "zip") source = await call("pick_zip");
	else if (kind === "git") {
		source = await ask({
			title: "Add a mod from GitHub",
			body: ["Paste the repository address. Every mod in it is added, turned off."],
			input: { placeholder: "https://github.com/someone/their-mod" },
			ok: "Add",
		});
	}
	if (!source) return;
	await install(kind, source, false);
}

async function install(kind, source, replace) {
	try {
		toast("Adding…");
		const added = await invoke("add_mod", { kind, source, replace });
		selected = added[0];
		await refresh();
		toast(`Added ${added.join(", ")}. Turn ${added.length > 1 ? "them" : "it"} on to run ${added.length > 1 ? "them" : "it"}.`);
	} catch (error) {
		const text = String(error);
		if (text.startsWith("exists:")) {
			const names = text.slice(7);
			const ok = await ask({
				title: `Replace ${names}?`,
				body: ["It's already installed. The current version is kept as a backup, so you can roll back."],
				ok: "Replace",
			});
			if (ok) await install(kind, source, true);
		} else toast(text, true);
	}
}

async function newMod() {
	if (!state.sdk_ready) {
		toast("Set the game source folder in Settings first. New mods are built with its mod SDK.", true);
		return;
	}
	const name = await ask({
		title: "Start a new mod",
		body: ["It's created in the mods folder with a C template and a build script, and turned on."],
		input: { placeholder: "e.g. trade-helper" },
		ok: "Create",
	});
	if (!name) return;
	const folder = await call("new_mod", { name });
	selected = folder;
	await refresh();
	const mod = modByFolder(folder);
	toast(`Created ${folder}. Building it…`);
	await build(mod, null);
}

// ---------- game ----------

function renderGame() {
	const running = state.game_running;
	$("game-dot").hidden = !running;
	for (const id of ["launch-btn", "launch-btn-2"]) {
		$(id).disabled = running || !state.game_exe;
		$(id).title = state.game_exe ? "" : "Couldn't find the game. Set its location in Settings.";
	}
	$("stop-btn").hidden = !running;
	$("game-status").textContent = running
		? "The game is running. Mod output appears below as it happens."
		: state.game_exe
			? "The game isn't running."
			: "Couldn't find the game. Build it, or set its location in Settings.";
}

async function launch() {
	const dev = $("dev-mode").checked;
	await call("launch_game", { dev });
	addLog({ sys: true, text: `Game started${dev ? " with live reload" : ""}.` });
	await refresh();
}

// ---------- console ----------

const LINE = /^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d) \| ([IWE]) \| (.*)$/;
const MOD_TAG = /\[mod ([^\]]+)\]|[Mm]od "([^"]+)"/;

function addLog(entry) {
	if (!entry.sys) {
		const match = LINE.exec(entry.line);
		entry.time = match ? match[2] : "";
		entry.level = match ? match[3] : entry.stream === "err" ? "W" : "I";
		entry.text = match ? match[4] : entry.line;
		const tag = MOD_TAG.exec(entry.text);
		entry.mod = tag ? tag[1] || tag[2] : null;
	}
	logLines.push(entry);
	if (logLines.length > MAX_LOG) logLines.splice(0, logLines.length - MAX_LOG);
	if (entry.mod && ![...$("f-mod").options].some((o) => o.value === entry.mod)) $("f-mod").append(el("option", { value: entry.mod }, entry.mod));
	if (passes(entry)) appendLine(entry, true);
}

function passes(entry) {
	if (entry.sys) return true;
	const source = $("f-source").value;
	const level = $("f-level").value;
	const mod = $("f-mod").value;
	const text = $("f-text").value.toLowerCase();
	if (source === "mods" && !entry.mod) return false;
	if (level === "W" && entry.level === "I") return false;
	if (level === "E" && entry.level !== "E") return false;
	if (mod && entry.mod !== mod) return false;
	if (text && !entry.text.toLowerCase().includes(text)) return false;
	return true;
}

function lineNode(entry) {
	if (entry.sys) return el("div", { class: "ln sys" }, el("time", {}, ""), entry.text);
	let body = entry.text;
	if (entry.mod) {
		const at = body.indexOf(entry.mod);
		body = el("span", { class: "m-mod" }, body.slice(0, at), el("b", {}, entry.mod), body.slice(at + entry.mod.length));
	}
	return el("div", { class: "ln " + entry.level }, el("time", {}, entry.time), body);
}

function appendLine(entry, follow) {
	const box = $("console");
	box.querySelector(".placeholder")?.remove();
	const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
	box.append(lineNode(entry));
	if (follow && atBottom) box.scrollTop = box.scrollHeight;
}

function redrawConsole() {
	const box = $("console");
	const shown = logLines.filter(passes);
	box.replaceChildren(...shown.map(lineNode));
	if (!shown.length) {
		box.append(el("div", { class: "placeholder" },
			logLines.length ? "Nothing matches these filters." : "Launch the game to see mod output here."));
	}
	box.scrollTop = box.scrollHeight;
}

// ---------- profiles ----------

function renderProfiles() {
	const entries = Object.entries(state.profiles);
	$("profiles-empty").hidden = entries.length > 0;
	$("profiles").replaceChildren(
		...entries.map(([name, folders]) =>
			el(
				"li",
				{},
				el("div", {}, el("strong", {}, name), el("div", { class: "mods-list" }, folders.length ? folders.join(", ") : "No mods")),
				el(
					"div",
					{ class: "row" },
					el("button", { class: "primary", onclick: () => applyProfile(name) }, "Use"),
					el("button", { class: "danger", onclick: () => deleteProfile(name) }, "Delete"),
				),
			),
		),
	);
}

async function applyProfile(name) {
	const missing = await call("apply_profile", { name });
	await refresh();
	toast(missing.length ? `Using ${name}. Not installed, so skipped: ${missing.join(", ")}.` : `Using ${name}.`, missing.length > 0);
}

async function deleteProfile(name) {
	const ok = await ask({ title: `Delete the profile "${name}"?`, body: ["The mods themselves aren't affected."], ok: "Delete", danger: true });
	if (!ok) return;
	await call("delete_profile", { name });
	await refresh();
}

// ---------- settings ----------

function renderSettings() {
	const s = state.settings;
	const form = $("settings-form");
	if (!form.dataset.dirty) {
		$("s-game-dir").value = s.game_dir;
		$("s-game-exe").value = s.game_exe;
		$("s-mods-dir").value = s.mods_dir;
		$("s-editor").value = s.editor;
	}
	setStatus("st-game", !!state.game_exe, state.game_exe ? `Game found: ${state.game_exe}` : "Game not found. Build it in the source folder, or choose the program below.");
	setStatus("st-sdk", state.sdk_ready, state.sdk_ready ? `Found, for mod API version ${state.api_version}.` : "Not found. Check the game source folder.");
	setStatus("st-modcheck", state.modcheck_ready, state.modcheck_ready ? "Built and ready." : "Not built yet.");
	$("build-tool").textContent = state.modcheck_ready ? "Rebuild test tool" : "Build test tool";
}

function setStatus(id, ok, text) {
	$(id).textContent = text;
	$(id).className = "status " + (ok ? "ok" : "bad");
}

function showToolOutput(result) {
	$("tool-output-wrap").hidden = false;
	$("tool-output-wrap").open = true;
	$("tool-output").textContent = result.output || "(no output)";
}

// ---------- wiring ----------

function showView(name) {
	document.querySelectorAll(".tab").forEach((t) => (t.dataset.view === name ? t.setAttribute("aria-current", "page") : t.removeAttribute("aria-current")));
	document.querySelectorAll(".view").forEach((v) => (v.hidden = v.id !== "view-" + name));
	if (name === "console") redrawConsole();
}

function closeMenu() {
	$("add-menu").hidden = true;
	$("add-btn").setAttribute("aria-expanded", "false");
}

function wire() {
	document.querySelectorAll(".tab").forEach((t) => t.addEventListener("click", () => showView(t.dataset.view)));

	$("add-btn").addEventListener("click", (e) => {
		e.stopPropagation();
		const menu = $("add-menu");
		menu.hidden = !menu.hidden;
		$("add-btn").setAttribute("aria-expanded", String(!menu.hidden));
		if (!menu.hidden) menu.querySelector("button").focus();
	});
	document.addEventListener("click", closeMenu);
	document.addEventListener("keydown", (e) => e.key === "Escape" && closeMenu());
	document.querySelectorAll("[data-add]").forEach((b) =>
		b.addEventListener("click", () => {
			closeMenu();
			addMod(b.dataset.add);
		}),
	);

	$("new-btn").addEventListener("click", newMod);
	$("mods-folder").addEventListener("click", () => call("open_path", { path: state.library.mods_dir }));
	$("launch-btn").addEventListener("click", launch);
	$("launch-btn-2").addEventListener("click", launch);
	$("stop-btn").addEventListener("click", () => call("stop_game"));

	for (const id of ["f-source", "f-level", "f-mod"]) $(id).addEventListener("change", redrawConsole);
	$("f-text").addEventListener("input", redrawConsole);
	$("clear-log").addEventListener("click", () => {
		logLines.length = 0;
		redrawConsole();
	});

	$("profile-form").addEventListener("submit", async (e) => {
		e.preventDefault();
		const name = $("profile-name").value.trim();
		if (!name) return toast("Give the profile a name.", true);
		await call("save_profile", { name });
		$("profile-name").value = "";
		await refresh();
		toast(`Saved ${name}.`);
	});

	const form = $("settings-form");
	form.addEventListener("input", () => (form.dataset.dirty = "1"));
	form.addEventListener("submit", async (e) => {
		e.preventDefault();
		await call("save_settings", {
			settings: {
				game_dir: $("s-game-dir").value.trim(),
				game_exe: $("s-game-exe").value.trim(),
				mods_dir: $("s-mods-dir").value.trim(),
				editor: $("s-editor").value.trim(),
			},
		});
		delete form.dataset.dirty;
		await refresh();
		toast("Settings saved.");
	});
	document.querySelectorAll("[data-browse]").forEach((b) =>
		b.addEventListener("click", async () => {
			const path = await call("pick_folder");
			if (path) {
				$(b.dataset.browse).value = path;
				form.dataset.dirty = "1";
			}
		}),
	);
	document.querySelectorAll("[data-browse-file]").forEach((b) =>
		b.addEventListener("click", async () => {
			const path = await call("pick_file");
			if (path) {
				$(b.dataset.browseFile).value = path;
				form.dataset.dirty = "1";
			}
		}),
	);

	$("build-tool").addEventListener("click", async (e) => {
		const result = await busy(e.currentTarget, "Building…", () => call("build_test_tool"));
		showToolOutput(result);
		await refresh();
		toast(result.ok ? "Test tool built." : "The test tool didn't build. The output shows why.", !result.ok);
	});
	$("check-game").addEventListener("click", async (e) => {
		const result = await busy(e.currentTarget, "Checking…", () => call("check_with_game"));
		showToolOutput(result);
	});

	listen("game-log", (event) => addLog({ ...event.payload }));
	listen("game-exit", async (event) => {
		const code = event.payload;
		addLog({ sys: true, text: code === 0 || code === null ? "Game closed." : `Game closed with code ${code}.` });
		await refresh();
	});

	// Pick up mods rebuilt or changed outside the manager.
	window.addEventListener("focus", () => refresh().catch(() => {}));
	setInterval(() => document.hasFocus() && refresh().catch(() => {}), 5000);
}

wire();
refresh().then(() => {
	const first = state.library.mods[0];
	if (first && !selected) select(first.folder);
});

// @ts-check
// Sessions (docs/spec/features.md, section 9): the home tab with all sessions,
// a typed editor for new and existing ones, one tab per opened session, and
// the shared test catalogue. Plain JavaScript as an ES module, checked by
// tsc --strict --checkJs; no URIs in the forms, choices instead of free text.

/**
 * @typedef {{role: string, label: string, state: string, detail: string}} LinkStatus
 * @typedef {{name: string, type: "lis" | "device", state: string, links: LinkStatus[],
 *            messages_in: number, messages_out: number, last_activity: string,
 *            last_problem: string, error: string}} SessionStatus
 * @typedef {{port: string, baud: number, data_bits: number, parity: string,
 *            stop_bits: number, flow: string}} SerialCfg
 * @typedef {{role: string, transport: "serial" | "tcp-server" | "tcp-client",
 *            serial?: SerialCfg, tcp?: {address?: string, host?: string, port: number}}} LinkCfg
 * @typedef {{lisbench: string, name: string, type: "lis" | "device", profile: string,
 *            timing: string, links: LinkCfg[],
 *            lis?: {unsolicited: string, answer_queries: boolean, no_order_reply: string},
 *            device?: {mode: string, specimens: string[] | {prefix: string, start: number},
 *                      tests: string[], interval: string, query_timeout: string,
 *                      measure_time: string}}} SessionCfg
 * @typedef {{code: string, name: string, kind: string, unit: string, decimals: number,
 *            reference?: {low?: number, high?: number}, simulate?: {low: number, high: number},
 *            values: string[], abnormal: string[]}} TestDef
 * @typedef {{value: string, unit: string, flags: string, status: string, operator: string,
 *            completed_at: string}} ResultData
 * @typedef {{test: string, state: string, unsolicited: boolean, result?: ResultData}} OrderItem
 * @typedef {{id: string, patient_id: string, patient_name: string, priority: string,
 *            items: OrderItem[]}} Specimen
 * @typedef {{specimens: Specimen[], unmatched: {specimen: string, test: string,
 *            result: ResultData}[]}} Book
 * @typedef {{id: string, state: string, tests: string[], values: string[], note: string}} DevSpecimen
 * @typedef {{t: string, text: string, lines: string[]}} LogEntry
 */

const BAUDS = [1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200];

/**
 * Builds an element: h("td", {class: "x"}, "text", child).
 * @param {string} tag
 * @param {Record<string, string | boolean | number | ((ev: Event) => void)>} [attrs]
 * @param {(Node | string | null | undefined | false)[]} children
 * @returns {HTMLElement}
 */
function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (typeof v === "function") {
      el.addEventListener(k.replace(/^on/, ""), v);
    } else if (typeof v === "boolean") {
      if (v) el.setAttribute(k, "");
    } else {
      el.setAttribute(k, String(v));
    }
  }
  for (const c of children) {
    if (c !== null && c !== undefined && c !== false) el.append(c);
  }
  return el;
}

/**
 * @param {string} path
 * @param {object} [body]
 * @returns {Promise<any>}
 */
async function api(path, body) {
  const response = await fetch(path, body === undefined ? {} : {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    throw new Error(data.error ?? `${response.status} ${response.statusText}`);
  }
  return data;
}

/** @param {unknown} e */
function toast(e) {
  const t = document.getElementById("toast");
  if (!(t instanceof HTMLElement)) return;
  t.textContent = e instanceof Error ? e.message : String(e);
  t.hidden = false;
  window.clearTimeout(Number(t.dataset.timer ?? 0));
  t.dataset.timer = String(window.setTimeout(() => { t.hidden = true; }, 7000));
}

/** @param {() => Promise<unknown>} fn */
function guarded(fn) {
  return () => { fn().catch(toast); };
}

/**
 * @param {string} name
 * @param {string[]} options
 * @param {string} value
 * @param {Record<string, string>} [labels]
 */
function select(name, options, value, labels = {}) {
  return h("select", { name }, ...options.map((o) =>
    h("option", { value: o, selected: o === value }, labels[o] ?? o)));
}

/**
 * Radio buttons; returns the group and a getter.
 * @param {string} name
 * @param {[string, string][]} options value, label
 * @param {string} value
 * @param {(v: string) => void} [changed]
 */
function radios(name, options, value, changed) {
  const group = h("div", { class: "radios" });
  for (const [v, label] of options) {
    const input = /** @type {HTMLInputElement} */ (h("input", { type: "radio", name, value: v, checked: v === value }));
    input.addEventListener("change", () => changed?.(v));
    group.append(h("label", { class: "check" }, input, label));
  }
  return {
    el: group,
    get: () => {
      const c = group.querySelector("input:checked");
      return c instanceof HTMLInputElement ? c.value : value;
    },
  };
}

/** @param {HTMLElement} root @param {string} name */
function input(root, name) {
  const el = root.querySelector(`[name="${name}"]`);
  if (el instanceof HTMLInputElement || el instanceof HTMLSelectElement || el instanceof HTMLTextAreaElement) {
    return el;
  }
  throw new Error(`missing field ${name}`);
}

/** @param {string} d "30s", "500ms" */
function seconds(d) {
  return d.endsWith("ms") ? Number(d.slice(0, -2)) / 1000 : Number(d.replace(/s$/, ""));
}

/** @param {string} state */
function badge(state) {
  return h("span", { class: `badge ${state}` }, state);
}

// --- shared data ---------------------------------------------------------------

/** @type {TestDef[]} */
let catalog = [];
/** @type {{path: string, description: string}[]} */
let ports = [];
/** @type {string[]} */
let profiles = [];

async function loadShared() {
  const [c, p, pr] = await Promise.all([api("/api/catalog"), api("/api/serial-ports"), api("/api/profiles")]);
  catalog = c.tests;
  ports = p;
  profiles = pr;
}

// --- top navigation ------------------------------------------------------------------

/** @param {string} view @param {string} [session] */
function show(view, session) {
  for (const b of Array.from(document.querySelectorAll("#main-nav button"))) {
    if (b instanceof HTMLElement) {
      b.classList.toggle("active", session ? b.dataset.session === session : b.dataset.view === view);
    }
  }
  for (const v of ["home", "catalog", "session", "tools"]) {
    const el = document.getElementById(`view-${v}`);
    if (el) el.hidden = v !== view;
  }
  for (const pane of Array.from(document.querySelectorAll("#view-session > .session"))) {
    if (pane instanceof HTMLElement) pane.hidden = pane.dataset.session !== session;
  }
}

function setupNav() {
  for (const b of Array.from(document.querySelectorAll("#main-nav > button"))) {
    if (!(b instanceof HTMLButtonElement)) continue;
    b.addEventListener("click", () => {
      show(b.dataset.view ?? "home");
      if (b.dataset.view === "catalog") renderCatalog();
    });
  }
}

// --- home: the session list ------------------------------------------------------------

/** @type {Map<string, SessionStatus>} */
const statuses = new Map();

async function refreshSessions() {
  /** @type {SessionStatus[]} */
  const list = await api("/api/sessions");
  statuses.clear();
  for (const s of list) statuses.set(s.name, s);
  renderSessions();
}

/** @param {LinkStatus} l */
function linkView(l) {
  return h("div", { class: "link", title: l.detail || "" }, badge(l.state), ` ${l.role}: ${l.label}`);
}

function renderSessions() {
  const table = document.getElementById("sessions");
  if (!(table instanceof HTMLTableElement)) return;
  table.replaceChildren(h("thead", {}, h("tr", {},
    ...["Session", "Type", "State", "Connections", "In / out", "Last activity", "Last problem", ""].map((t) => h("th", {}, t)))));
  const body = h("tbody");
  if (statuses.size === 0) {
    body.append(h("tr", {}, h("td", { colspan: 8, class: "muted" },
      "No sessions yet. Create a LIS simulator, an instrument simulator, or both.")));
  }
  for (const s of statuses.values()) {
    const running = s.state === "running" || s.state === "stopping";
    body.append(h("tr", {},
      h("td", {}, h("a", { href: "#", onclick: (ev) => { ev.preventDefault(); void openSession(s.name); } }, s.name)),
      h("td", {}, s.type === "lis" ? "LIS simulator" : "instrument simulator"),
      h("td", {}, badge(s.state), s.error ? h("div", { class: "detail" }, s.error) : null),
      h("td", {}, ...s.links.map(linkView)),
      h("td", {}, `${s.messages_in} / ${s.messages_out}`),
      h("td", {}, s.last_activity),
      h("td", { class: "detail" }, s.last_problem),
      h("td", { class: "actions" },
        running
          ? h("button", { type: "button", onclick: guarded(() => api("/api/session/stop", { name: s.name })) }, "Stop")
          : h("button", { type: "button", class: "primary", disabled: Boolean(s.error), onclick: guarded(() => api("/api/session/start", { name: s.name })) }, "Start"),
        h("button", { type: "button", onclick: () => void openSession(s.name) }, "Open"),
        !running && h("button", { type: "button", onclick: guarded(async () => {
          await api("/api/session/delete", { name: s.name });
          closeTab(s.name);
          await refreshSessions();
        }) }, "Delete"))));
  }
  table.append(body);
}

// --- the session editor ----------------------------------------------------------------

/** @param {"lis" | "device"} type @returns {SessionCfg} */
function newConfig(type) {
  const n = statuses.size + 1;
  return type === "lis"
    ? { lisbench: "session/1", name: `lis-${n}`, type, profile: "", timing: "lis01-a2",
        links: [{ role: "main", transport: "tcp-server", tcp: { address: "0.0.0.0", port: 15000 } }],
        lis: { unsolicited: "flag", answer_queries: true, no_order_reply: "y-record" } }
    : { lisbench: "session/1", name: `instrument-${n}`, type, profile: "", timing: "lis01-a2",
        links: [{ role: "main", transport: "tcp-client", tcp: { host: "127.0.0.1", port: 15000 } }],
        device: { mode: "host-query", specimens: { prefix: "S-", start: 1 }, tests: [],
                  interval: "30s", query_timeout: "60s", measure_time: "2s" } };
}

/** Fills the choice list of one port picker from `ports`. @param {HTMLSelectElement} list */
function fillPortList(list) {
  list.replaceChildren(h("option", { value: "" }, ports.length > 0 ? "(ports found)" : "(no serial port found)"),
    ...ports.map((p) => h("option", { value: p.path }, `${p.path} (${p.description || "serial port"})`)));
  list.value = "";
}

/**
 * A serial port field: free text (a port that is not plugged in yet, or one the
 * list misses), a list of the ports found on the computer running lisbench, and
 * a button that asks for them again. All pickers on the page share the list.
 * @param {string} name
 * @param {string} value
 */
function portPicker(name, value) {
  const field = /** @type {HTMLInputElement} */ (h("input", { name, value,
    placeholder: "e.g. /dev/ttyUSB0 or COM3", autocomplete: "off" }));
  const list = /** @type {HTMLSelectElement} */ (h("select", { class: "port-list", title: "ports found", "aria-label": "ports found" }));
  fillPortList(list);
  list.addEventListener("change", () => {
    if (list.value !== "") field.value = list.value;
    list.value = "";
    field.dispatchEvent(new Event("input", { bubbles: true }));
  });
  const refresh = h("button", { type: "button", class: "small", title: "look for serial ports again",
    "aria-label": "look for serial ports again" }, "\u21bb");
  refresh.addEventListener("click", guarded(async () => {
    ports = await api("/api/serial-ports");
    for (const l of Array.from(document.querySelectorAll("select.port-list"))) {
      if (l instanceof HTMLSelectElement) fillPortList(l);
    }
  }));
  return h("span", { class: "port-picker" }, field, list, refresh);
}

/**
 * One connection: serial port, wait for a connection, or connect to a partner.
 * @param {LinkCfg} link
 * @param {string} title
 */
function linkEditor(link, title) {
  const box = h("fieldset", { class: "linkbox" }, h("legend", {}, title));
  const serial = link.serial ?? { port: ports[0]?.path ?? "", baud: 9600, data_bits: 8, parity: "none", stop_bits: 1, flow: "none" };
  const tcp = link.tcp ?? { port: 15000 };

  const serialBox = h("div", { class: "grid" },
    h("label", {}, "Port", portPicker("port", serial.port)),
    h("label", {}, "Baud rate", select("baud", BAUDS.map(String), String(serial.baud))),
    h("label", {}, "Data bits", select("data_bits", ["7", "8"], String(serial.data_bits))),
    h("label", {}, "Parity", select("parity", ["none", "even", "odd"], serial.parity)),
    h("label", {}, "Stop bits", select("stop_bits", ["1", "2"], String(serial.stop_bits))),
    h("label", {}, "Flow control", select("flow", ["none", "software", "hardware"], serial.flow)));
  const serverBox = h("div", { class: "grid" },
    h("label", {}, "Accept connections from", select("address", ["0.0.0.0", "127.0.0.1"], tcp.address ?? "0.0.0.0",
      { "0.0.0.0": "the network", "127.0.0.1": "this computer only" })),
    h("label", {}, "Port", h("input", { name: "server_port", type: "number", min: 1, max: 65535, required: true, value: tcp.port })));
  const clientBox = h("div", { class: "grid" },
    h("label", {}, "Host (name or IP address)", h("input", { name: "host", required: true, value: tcp.host ?? "127.0.0.1" })),
    h("label", {}, "Port", h("input", { name: "client_port", type: "number", min: 1, max: 65535, required: true, value: tcp.port })));
  const showFor = (/** @type {string} */ t) => {
    serialBox.hidden = t !== "serial";
    serverBox.hidden = t !== "tcp-server";
    clientBox.hidden = t !== "tcp-client";
  };
  const transport = radios(`transport-${title}`, [
    ["serial", "Serial port"], ["tcp-server", "Network: wait for the partner to connect"],
    ["tcp-client", "Network: connect to the partner"]], link.transport, showFor);
  showFor(link.transport);
  box.append(transport.el, serialBox, serverBox, clientBox);
  return {
    el: box,
    /** @returns {LinkCfg} */
    get: () => {
      const t = /** @type {LinkCfg["transport"]} */ (transport.get());
      if (t === "serial") {
        return { role: link.role, transport: t, serial: {
          port: input(serialBox, "port").value, baud: Number(input(serialBox, "baud").value),
          data_bits: Number(input(serialBox, "data_bits").value), parity: input(serialBox, "parity").value,
          stop_bits: Number(input(serialBox, "stop_bits").value), flow: input(serialBox, "flow").value } };
      }
      if (t === "tcp-server") {
        return { role: link.role, transport: t, tcp: { address: input(serverBox, "address").value,
          port: Number(input(serverBox, "server_port").value) } };
      }
      return { role: link.role, transport: t, tcp: { host: input(clientBox, "host").value.trim(),
        port: Number(input(clientBox, "client_port").value) } };
    },
  };
}

/** @param {string[]} chosen */
function testChoices(chosen) {
  const box = h("div", { class: "cases" });
  if (catalog.length === 0) box.append(h("span", { class: "muted" }, "The test catalogue is empty; add tests there first."));
  for (const t of catalog) {
    box.append(h("label", { class: "check" },
      h("input", { type: "checkbox", value: t.code, checked: chosen.includes(t.code) }), `${t.code} ${t.name}`));
  }
  return {
    el: box,
    get: () => Array.from(box.querySelectorAll("input:checked"), (i) => /** @type {HTMLInputElement} */ (i).value),
  };
}

/**
 * The form for one session, new or existing.
 * @param {SessionCfg} cfg
 * @param {boolean} isNew
 */
function openEditor(cfg, isNew) {
  const editor = document.getElementById("editor");
  if (!editor) return;
  show("home");
  editor.hidden = false;
  const form = /** @type {HTMLFormElement} */ (h("form", { class: "panel editor" }));
  const general = h("div", { class: "grid" },
    h("label", {}, "Name", h("input", { name: "name", value: cfg.name, required: true, pattern: "[A-Za-z0-9._-]{1,64}",
      readonly: !isNew, title: "letters, digits, '.', '_' or '-'" })),
    h("label", {}, "Timing", select("timing", ["lis01-a2", "fast-test"], cfg.timing,
      { "lis01-a2": "LIS01-A2 (15 s, 30 s, 10 s)", "fast-test": "fast test (devices built for it)" })),
    h("label", {}, "Instrument profile", select("profile", ["", ...profiles], cfg.profile, { "": "(none)" })));

  // Connections: a LIS may use one port, or one for orders and one for results.
  const separate = cfg.type === "lis" && cfg.links.some((l) => l.role === "orders");
  const linksBox = h("div");
  /** @type {{get: () => LinkCfg}[]} */
  let linkEditors = [];
  const drawLinks = (/** @type {boolean} */ two) => {
    const base = cfg.links[0] ?? newConfig(cfg.type).links[0];
    const of = (/** @type {string} */ role) => ({ ...(cfg.links.find((l) => l.role === role) ?? base), role });
    linkEditors = two
      ? [linkEditor(of("orders"), "Orders (queries in, orders out)"), linkEditor(of("results"), "Results (results in)")]
      : [linkEditor(of("main"), "Connection")];
    linksBox.replaceChildren(...linkEditors.map((e) => /** @type {any} */ (e).el));
  };
  drawLinks(separate);
  const twoPorts = /** @type {HTMLInputElement} */ (h("input", { type: "checkbox", checked: separate }));
  twoPorts.addEventListener("change", () => drawLinks(twoPorts.checked));

  form.append(h("h3", {}, `${isNew ? "New" : "Edit"} ${cfg.type === "lis" ? "LIS simulator" : "instrument simulator"}`), general);
  if (cfg.type === "lis") form.append(h("label", { class: "check" }, twoPorts, "separate ports for orders and results"));
  form.append(linksBox);

  /** @type {() => Partial<SessionCfg>} */
  let typeSettings;
  if (cfg.type === "lis") {
    const l = cfg.lis ?? { unsolicited: "flag", answer_queries: true, no_order_reply: "y-record" };
    const unsolicited = radios("unsolicited", [
      ["flag", "keep for review (assign or discard by hand)"], ["accept", "accept (create the item)"],
      ["ignore", "ignore (log only)"]], l.unsolicited);
    form.append(h("fieldset", {}, h("legend", {}, "Results without an order (unsolicited)"), unsolicited.el),
      h("div", { class: "grid" },
        h("label", { class: "check" }, h("input", { type: "checkbox", name: "answer_queries", checked: l.answer_queries }),
          "answer queries (Q) from the order book"),
        h("label", {}, "Answer when there is no order", select("no_order_reply", ["y-record", "empty"], l.no_order_reply,
          { "y-record": "O record with report type Y", "empty": "header and terminator only" }))));
    typeSettings = () => ({ lis: { unsolicited: unsolicited.get(),
      answer_queries: /** @type {HTMLInputElement} */ (input(form, "answer_queries")).checked,
      no_order_reply: input(form, "no_order_reply").value } });
  } else {
    const d = cfg.device ?? /** @type {NonNullable<SessionCfg["device"]>} */ (newConfig("device").device);
    const listed = Array.isArray(d.specimens);
    const counter = Array.isArray(d.specimens) ? { prefix: "S-", start: 1 } : d.specimens;
    const tests = testChoices(d.tests);
    const testsBox = h("fieldset", {}, h("legend", {}, "Tests to report for each specimen"), tests.el);
    const mode = radios("mode", [
      ["host-query", "host query: ask the LIS (Q), measure what it orders"],
      ["unsolicited", "unsolicited: report the tests below for every specimen"]], d.mode,
      (v) => { testsBox.hidden = v !== "unsolicited"; });
    testsBox.hidden = d.mode !== "unsolicited";
    const source = radios("source", [["counter", "numbered"], ["list", "from a list"]], listed ? "list" : "counter",
      (v) => { counterBox.hidden = v !== "counter"; listBox.hidden = v !== "list"; });
    const counterBox = h("div", { class: "grid" },
      h("label", {}, "Prefix", h("input", { name: "prefix", value: counter.prefix })),
      h("label", {}, "First number", h("input", { name: "start", type: "number", min: 0, value: counter.start })));
    const listBox = h("label", {}, "Specimen IDs, one per line",
      h("textarea", { name: "specimens", rows: 3 }, Array.isArray(d.specimens) ? d.specimens.join("\n") : ""));
    counterBox.hidden = listed;
    listBox.hidden = !listed;
    form.append(h("fieldset", {}, h("legend", {}, "Mode"), mode.el), testsBox,
      h("fieldset", {}, h("legend", {}, "Specimens"), source.el, counterBox, listBox),
      h("div", { class: "grid" },
        h("label", {}, "Pause between specimens (s)", h("input", { name: "interval", type: "number", min: 0, step: "any", value: seconds(d.interval) })),
        h("label", {}, "Wait for the LIS's answer (s)", h("input", { name: "query_timeout", type: "number", min: 1, step: "any", value: seconds(d.query_timeout) })),
        h("label", {}, "Measuring time (s)", h("input", { name: "measure_time", type: "number", min: 0, step: "any", value: seconds(d.measure_time) }))));
    const ms = (/** @type {string} */ name) => `${Math.round(Number(input(form, name).value) * 1000)}ms`;
    typeSettings = () => ({ device: {
      mode: mode.get(),
      specimens: source.get() === "list"
        ? input(form, "specimens").value.split("\n").map((s) => s.trim()).filter((s) => s !== "")
        : { prefix: input(form, "prefix").value, start: Number(input(form, "start").value) },
      tests: tests.get(), interval: ms("interval"), query_timeout: ms("query_timeout"), measure_time: ms("measure_time") } });
  }

  const save = async (/** @type {boolean} */ start) => {
    if (!form.reportValidity()) return;
    /** @type {SessionCfg} */
    const out = { lisbench: "session/1", name: input(form, "name").value.trim(), type: cfg.type,
      profile: input(form, "profile").value, timing: input(form, "timing").value,
      links: linkEditors.map((e) => e.get()), ...typeSettings() };
    await api("/api/session/save", { config: out });
    editor.hidden = true;
    editor.replaceChildren();
    await refreshSessions();
    if (start) await api("/api/session/start", { name: out.name });
    await openSession(out.name);
  };
  form.append(h("div", { class: "actions" },
    h("button", { type: "button", class: "primary", onclick: guarded(() => save(true)) }, "Save and start"),
    h("button", { type: "button", onclick: guarded(() => save(false)) }, "Save"),
    h("button", { type: "button", onclick: () => { editor.hidden = true; editor.replaceChildren(); } }, "Cancel")));
  editor.replaceChildren(form);
  form.scrollIntoView({ behavior: "smooth" });
}

// --- session tabs ---------------------------------------------------------------------

/** @type {Map<string, {pane: HTMLElement, type: string, render: (kind: string, data: any) => void}>} */
const tabs = new Map();

// Open tabs survive a reload of the page (a convenience of this browser only).
const TABS_KEY = "lisbench.tabs";
function rememberTabs() {
  try {
    localStorage.setItem(TABS_KEY, JSON.stringify([...tabs.keys()]));
  } catch {
    // private window or storage blocked: tabs are simply not restored
  }
}
/** @returns {string[]} */
function rememberedTabs() {
  try {
    const v = JSON.parse(localStorage.getItem(TABS_KEY) ?? "[]");
    return Array.isArray(v) ? v.filter((x) => typeof x === "string") : [];
  } catch {
    return [];
  }
}

/** @param {string} name */
function closeTab(name) {
  const t = tabs.get(name);
  if (!t) return;
  t.pane.remove();
  document.querySelector(`#session-tabs button[data-session="${CSS.escape(name)}"]`)?.remove();
  tabs.delete(name);
  rememberTabs();
  show("home");
}

/** @param {string} name */
async function openSession(name) {
  if (!tabs.has(name)) {
    const d = await api(`/api/session?${new URLSearchParams({ name })}`);
    const pane = h("div", { class: "session", "data-session": name });
    document.getElementById("view-session")?.append(pane);
    const tab = h("button", { type: "button", "data-session": name }, name,
      h("span", { class: "close", title: "close tab", onclick: (ev) => { ev.stopPropagation(); closeTab(name); } }, "×"));
    tab.addEventListener("click", () => show("session", name));
    document.getElementById("session-tabs")?.append(tab);
    tabs.set(name, buildSession(name, pane, d));
    rememberTabs();
  }
  show("session", name);
}

/**
 * @param {string} name
 * @param {HTMLElement} pane
 * @param {{config: SessionCfg, status: SessionStatus, log: LogEntry[], state: any}} d
 */
function buildSession(name, pane, d) {
  const header = h("div", { class: "session-head" });
  const log = h("div", { class: "events log-box" });
  const body = h("div");
  /** @type {SessionStatus} */
  let status = d.status;
  const drawHeader = () => {
    const running = status.state === "running" || status.state === "stopping";
    header.replaceChildren(
      h("h2", {}, name, " ", badge(status.state)),
      h("div", { class: "links" }, ...status.links.map(linkView)),
      h("div", { class: "muted" }, `messages in ${status.messages_in}, out ${status.messages_out}`,
        status.last_activity ? `, last ${status.last_activity}` : ""),
      status.last_problem ? h("div", { class: "detail problem" }, status.last_problem) : "",
      h("div", { class: "actions" },
        running
          ? h("button", { type: "button", onclick: guarded(() => api("/api/session/stop", { name })) }, "Stop")
          : h("button", { type: "button", class: "primary", onclick: guarded(() => api("/api/session/start", { name })) }, "Start"),
        !running && h("button", { type: "button", onclick: guarded(async () => {
          const fresh = await api(`/api/session?${new URLSearchParams({ name })}`);
          openEditor(fresh.config, false);
        }) }, "Settings")));
  };
  /** @param {LogEntry} e */
  const addLog = (e) => {
    const row = h("div", { class: `row${e.text.startsWith("problem") ? " problem" : ""}${e.lines.length ? " has-detail" : ""}` },
      `${e.t} ${e.text}`);
    if (e.lines.length) {
      row.append(h("div", { class: "detail" }, e.lines.join("\n")));
      row.addEventListener("click", () => row.classList.toggle("open"));
    }
    log.append(row);
    while (log.childElementCount > 1000) log.firstElementChild?.remove();
    log.scrollTop = log.scrollHeight;
  };
  const view = d.config.type === "lis" ? lisView(name, body) : deviceView(body);
  drawHeader();
  view(d.state);
  for (const e of d.log) addLog(e);
  pane.append(header, body, h("h3", {}, "Log (click a line for the records)"), log);
  return {
    pane, type: d.config.type,
    render: (/** @type {string} */ kind, /** @type {any} */ data) => {
      if (kind === "status") { status = data; drawHeader(); }
      if (kind === "log") addLog(data);
      if (kind === "book" || kind === "specimens") view(data);
    },
  };
}

/**
 * The LIS simulator: order book and results.
 * @param {string} session
 * @param {HTMLElement} body
 */
function lisView(session, body) {
  const addForm = /** @type {HTMLFormElement} */ (h("form", { class: "panel editor" }));
  const tests = testChoices([]);
  addForm.append(h("h3", {}, "New specimen"),
    h("div", { class: "grid" },
      h("label", {}, "Specimen ID", h("input", { name: "id", required: true, placeholder: "e.g. S-00001" })),
      h("label", {}, "Patient ID", h("input", { name: "patient_id" })),
      h("label", {}, "Patient name", h("input", { name: "patient_name", placeholder: "Last^First" })),
      h("label", {}, "Priority", select("priority", ["R", "S", "A"], "R", { R: "routine", S: "stat", A: "as soon as possible" }))),
    h("fieldset", {}, h("legend", {}, "Tests"), tests.el),
    h("div", { class: "actions" }, h("button", { type: "submit", class: "primary" }, "Add to the order book")));
  addForm.addEventListener("submit", (ev) => {
    ev.preventDefault();
    api("/api/lis/specimen", { session, id: input(addForm, "id").value.trim(),
      patient_id: input(addForm, "patient_id").value.trim(), patient_name: input(addForm, "patient_name").value.trim(),
      priority: input(addForm, "priority").value, tests: tests.get() })
      .then(() => { input(addForm, "id").value = ""; }).catch(toast);
  });
  const table = h("table", { class: "results" });
  const unmatched = h("div");
  body.append(addForm, h("div", { class: "toolbar" }, h("h3", {}, "Order book"),
    h("button", { type: "button", onclick: guarded(() => api("/api/lis/send", { session, specimens: [] })) }, "Send all new orders")),
    table, unmatched);

  /** @param {Book} book */
  return (book) => {
    table.replaceChildren(h("thead", {}, h("tr", {},
      ...["Specimen", "Patient", "Priority", "Tests: state and result", ""].map((t) => h("th", {}, t)))));
    const tb = h("tbody");
    if (book.specimens.length === 0) tb.append(h("tr", {}, h("td", { colspan: 5, class: "muted" }, "No specimens yet.")));
    for (const s of book.specimens) {
      const hasNew = s.items.some((i) => i.state === "new");
      tb.append(h("tr", {},
        h("td", {}, s.id),
        h("td", {}, [s.patient_id, s.patient_name].filter(Boolean).join(" ")),
        h("td", {}, s.priority),
        h("td", {}, ...s.items.map((i) => h("div", { class: "item" },
          h("b", {}, i.test), " ", badge(i.state), i.unsolicited ? h("span", { class: "muted" }, " unsolicited") : null,
          i.result ? ` ${i.result.value} ${i.result.unit} ${i.result.flags}` : "",
          (i.state === "sent" || i.state === "answered") && h("button", { type: "button", class: "small",
            onclick: guarded(() => api("/api/lis/cancel", { session, specimen: s.id, test: i.test })) }, "cancel")))),
        h("td", { class: "actions" },
          hasNew && h("button", { type: "button", class: "small", onclick: guarded(() => api("/api/lis/send", { session, specimens: [s.id] })) }, "Send"),
          h("button", { type: "button", class: "small", onclick: guarded(() => api("/api/lis/specimen/delete", { session, id: s.id })) }, "Delete"))));
    }
    table.append(tb);
    unmatched.replaceChildren();
    if (book.unmatched.length > 0) {
      unmatched.append(h("h3", {}, "Results without an order (for review)"), h("table", { class: "results" },
        h("thead", {}, h("tr", {}, ...["Specimen", "Test", "Result", "Status", ""].map((t) => h("th", {}, t)))),
        h("tbody", {}, ...book.unmatched.map((u, index) => h("tr", {},
          h("td", {}, u.specimen), h("td", {}, u.test),
          h("td", {}, `${u.result.value} ${u.result.unit} ${u.result.flags}`), h("td", {}, u.result.status),
          h("td", { class: "actions" },
            h("button", { type: "button", class: "small", onclick: guarded(() => api("/api/lis/unmatched", { session, index, action: "accept" })) }, "Accept"),
            h("button", { type: "button", class: "small", onclick: guarded(() => api("/api/lis/unmatched", { session, index, action: "discard" })) }, "Discard")))))));
    }
  };
}

/**
 * The instrument simulator: recent specimens.
 * @param {HTMLElement} body
 */
function deviceView(body) {
  const table = h("table", { class: "results" });
  body.append(h("h3", {}, "Specimens (most recent last)"), table);
  /** @param {{specimens: DevSpecimen[]}} state */
  return (state) => {
    table.replaceChildren(h("thead", {}, h("tr", {}, ...["Specimen", "State", "Tests", "Results", "Note"].map((t) => h("th", {}, t)))));
    const tb = h("tbody");
    if (state.specimens.length === 0) tb.append(h("tr", {}, h("td", { colspan: 5, class: "muted" }, "Nothing yet; start the session.")));
    for (const s of state.specimens) {
      tb.append(h("tr", {}, h("td", {}, s.id), h("td", {}, badge(s.state)), h("td", {}, s.tests.join(", ")),
        h("td", {}, s.values.map((v, i) => `${s.tests[i]} ${v}`).join(", ")), h("td", { class: "detail" }, s.note)));
    }
    table.append(tb);
  };
}

// --- catalogue -----------------------------------------------------------------------

function renderCatalog() {
  const table = document.getElementById("catalog");
  if (!(table instanceof HTMLTableElement)) return;
  table.replaceChildren(h("thead", {}, h("tr", {}, ...["Code", "Name", "Kind", "Unit", "Reference low", "high", "Decimals",
    "Simulate low", "high", "Values (qualitative)", "Abnormal values", ""].map((t) => h("th", {}, t)))));
  const tb = h("tbody");
  catalog.forEach((t, index) => {
    const num = (/** @type {string} */ name, /** @type {number | undefined} */ v) =>
      h("input", { name, type: "number", step: "any", value: v === undefined ? "" : v });
    tb.append(h("tr", { "data-index": index },
      h("td", {}, h("input", { name: "code", value: t.code, required: true, size: 6 })),
      h("td", {}, h("input", { name: "name", value: t.name })),
      h("td", {}, select("kind", ["numeric", "qualitative"], t.kind)),
      h("td", {}, h("input", { name: "unit", value: t.unit, size: 6 })),
      h("td", {}, num("low", t.reference?.low)), h("td", {}, num("high", t.reference?.high)),
      h("td", {}, h("input", { name: "decimals", type: "number", min: 0, max: 6, value: t.decimals })),
      h("td", {}, num("sim_low", t.simulate?.low)), h("td", {}, num("sim_high", t.simulate?.high)),
      h("td", {}, h("input", { name: "values", value: t.values.join(", "), placeholder: "POS, NEG" })),
      h("td", {}, h("input", { name: "abnormal", value: t.abnormal.join(", "), placeholder: "POS" })),
      h("td", {}, h("button", { type: "button", class: "small", onclick: () => { catalog.splice(index, 1); renderCatalog(); } }, "Delete"))));
  });
  table.append(tb);
}

/** @returns {TestDef[]} */
function readCatalog() {
  const rows = Array.from(document.querySelectorAll("#catalog tbody tr"));
  return rows.map((row) => {
    const r = /** @type {HTMLElement} */ (row);
    const v = (/** @type {string} */ n) => input(r, n).value.trim();
    const n = (/** @type {string} */ name) => (v(name) === "" ? undefined : Number(v(name)));
    const list = (/** @type {string} */ name) => v(name).split(",").map((s) => s.trim()).filter((s) => s !== "");
    /** @type {TestDef} */
    const t = { code: v("code"), name: v("name") || v("code"), kind: v("kind"), unit: v("unit"),
      decimals: Number(v("decimals") || 0), values: list("values"), abnormal: list("abnormal") };
    const low = n("low");
    const high = n("high");
    if (low !== undefined || high !== undefined) t.reference = { ...(low !== undefined ? { low } : {}), ...(high !== undefined ? { high } : {}) };
    const sl = n("sim_low");
    const sh = n("sim_high");
    if (sl !== undefined && sh !== undefined) t.simulate = { low: sl, high: sh };
    return t;
  });
}

function setupCatalog() {
  document.getElementById("catalog-add")?.addEventListener("click", () => {
    catalog = readCatalog();
    catalog.push({ code: "", name: "", kind: "numeric", unit: "", decimals: 0, values: [], abnormal: [] });
    renderCatalog();
  });
  document.getElementById("catalog-save")?.addEventListener("click", guarded(async () => {
    const tests = readCatalog();
    await api("/api/catalog/save", { lisbench: "catalog/1", tests });
    catalog = (await api("/api/catalog")).tests;
    renderCatalog();
    toast("Catalogue saved.");
  }));
}

// --- live events ------------------------------------------------------------------------

function listen() {
  const source = new EventSource("/api/events");
  source.addEventListener("session", (ev) => {
    if (!(ev instanceof MessageEvent)) return;
    /** @type {{name: string, kind: string, data: any}} */
    const e = JSON.parse(ev.data);
    if (e.kind === "status") {
      statuses.set(e.name, e.data);
      renderSessions();
    }
    tabs.get(e.name)?.render(e.kind, e.data);
  });
  source.addEventListener("open", () => { void refreshSessions().catch(toast); });
}

async function main() {
  setupNav();
  setupCatalog();
  document.getElementById("new-lis")?.addEventListener("click", () => openEditor(newConfig("lis"), true));
  document.getElementById("new-device")?.addEventListener("click", () => openEditor(newConfig("device"), true));
  try {
    await loadShared();
    await refreshSessions();
    for (const name of rememberedTabs()) {
      if (statuses.has(name)) await openSession(name);
    }
    show("home");
  } catch (e) {
    toast(e);
  }
  listen();
}

void main();

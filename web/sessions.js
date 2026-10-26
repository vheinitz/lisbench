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
 *            low_level?: string, serial?: SerialCfg, tcp?: {address?: string, host?: string, port: number}}} LinkCfg
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
 * @typedef {{test: string, state: string, unsolicited: boolean, review: boolean,
 *            result?: ResultData}} OrderItem
 * @typedef {{id: string, patient_id: string, patient_name: string, items: OrderItem[]}} Specimen
 * @typedef {{specimens: Specimen[]}} Book
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

/** @param {unknown} e @param {boolean} [ok] a confirmation, not an error */
function toast(e, ok = false) {
  const t = document.getElementById("toast");
  if (!(t instanceof HTMLElement)) return;
  t.classList.toggle("ok", ok);
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
 * A field whose value has components (LIS02-A2: separated by the component
 * delimiter ^). By default one box per component, so nobody has to type ^;
 * "raw" switches to one box with the value as sent. The value is kept in a
 * hidden input `name` for the form.
 * @param {string} name
 * @param {string[]} labels one per component
 * @param {string} [value]
 */
function componentField(name, labels, value = "") {
  const hidden = /** @type {HTMLInputElement} */ (h("input", { type: "hidden", name, value }));
  const parts = labels.map((l, i) => /** @type {HTMLInputElement} */ (h("input", {
    "aria-label": l, placeholder: l, value: value.split("^")[i] ?? "" })));
  const raw = /** @type {HTMLInputElement} */ (h("input", { "aria-label": `${name} as sent`, value,
    placeholder: labels.join("^"), hidden: true }));
  const boxes = h("span", { class: "components" }, ...parts);
  const toggle = /** @type {HTMLButtonElement} */ (h("button", { type: "button", class: "small",
    title: "edit as one text with ^ between the components" }, "raw"));
  const sync = () => {
    const rawMode = !raw.hidden;
    for (const p of parts) {
      p.setCustomValidity(/[|\\^&]/.test(p.value) ? "one component: | \\ ^ & are not allowed here; use raw for ^" : "");
    }
    raw.setCustomValidity(/[|\\&]/.test(raw.value) ? "| \\ & are not allowed" : "");
    hidden.value = rawMode ? raw.value.trim()
      : parts.map((p) => p.value.trim()).join("^").replace(/\^+$/, "");
  };
  for (const p of [...parts, raw]) p.addEventListener("input", sync);
  toggle.addEventListener("click", () => {
    sync();
    const toRaw = raw.hidden;
    if (toRaw) raw.value = hidden.value;
    else parts.forEach((p, i) => { p.value = hidden.value.split("^")[i] ?? ""; });
    raw.hidden = !toRaw;
    boxes.hidden = toRaw;
    toggle.textContent = toRaw ? "boxes" : "raw";
    toggle.title = toRaw ? "one box per component" : "edit as one text with ^ between the components";
    sync();
  });
  return h("span", { class: "component-field" }, hidden, boxes, raw, toggle);
}

/**
 * Shows at most `rows` rows of the table and scrolls the rest, with the
 * header kept in view. Follows the table's size, also when its tab opens.
 * @param {HTMLTableElement | HTMLElement} table
 * @param {number} [rows]
 * @param {boolean} [stickToEnd] keep the newest (last) rows in view
 */
function scrollRows(table, rows = 5, stickToEnd = false) {
  const box = h("div", { class: "scroll-rows" }, table);
  const fit = () => {
    const body = Array.from(table.querySelectorAll("tbody > tr"));
    if (body.length <= rows) {
      box.style.maxHeight = "";
      return;
    }
    const head = table.querySelector("thead");
    const height = (head instanceof HTMLElement ? head.offsetHeight : 0) +
      body.slice(0, rows).reduce((sum, r) => sum + (r instanceof HTMLElement ? r.offsetHeight : 0), 0);
    if (height > 0) box.style.maxHeight = `${height + 2}px`;
    if (stickToEnd) box.scrollTop = box.scrollHeight;
  };
  new ResizeObserver(fit).observe(table);
  return box;
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
      if (b.dataset.view === "catalog" && !catalogDirty) renderCatalog();
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
  // How the messages are carried; LIS01-A2 is the standard, the others are
  // seen on TCP (and the same as on the wire behind a serial device server).
  const lowLevel = h("div", { class: "grid" },
    h("label", {}, "Low-level protocol", select("low_level", ["lis01-a2", "none", "mllp"], link.low_level ?? "lis01-a2", {
      "lis01-a2": "LIS01-A2 (ENQ, frames, ACK, EOT)",
      none: "none: records with CR, message ends with L",
      mllp: "MLLP block (VT ... FS CR)",
    })));
  box.append(transport.el, serialBox, serverBox, clientBox, lowLevel);
  return {
    el: box,
    /** @returns {LinkCfg} */
    get: () => {
      const t = /** @type {LinkCfg["transport"]} */ (transport.get());
      const low_level = input(lowLevel, "low_level").value;
      if (t === "serial") {
        return { role: link.role, transport: t, low_level, serial: {
          port: input(serialBox, "port").value, baud: Number(input(serialBox, "baud").value),
          data_bits: Number(input(serialBox, "data_bits").value), parity: input(serialBox, "parity").value,
          stop_bits: Number(input(serialBox, "stop_bits").value), flow: input(serialBox, "flow").value } };
      }
      if (t === "tcp-server") {
        return { role: link.role, transport: t, low_level, tcp: { address: input(serverBox, "address").value,
          port: Number(input(serverBox, "server_port").value) } };
      }
      return { role: link.role, transport: t, low_level, tcp: { host: input(clientBox, "host").value.trim(),
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
      h("label", { class: "wide" }, "Patient name (P-6)",
        componentField("patient_name", ["Last", "First", "Middle", "Suffix", "Title"]))),
    h("fieldset", {}, h("legend", {}, "Tests"), tests.el),
    h("div", { class: "actions" }, h("button", { type: "submit", class: "primary" }, "Add to the order book")));
  addForm.addEventListener("submit", (ev) => {
    ev.preventDefault();
    api("/api/lis/specimen", { session, id: input(addForm, "id").value.trim(),
      patient_id: input(addForm, "patient_id").value.trim(), patient_name: input(addForm, "patient_name").value.trim(),
      tests: tests.get() })
      .then(() => { input(addForm, "id").value = ""; }).catch((e) => toast(e));
  });
  const table = h("table", { class: "results" });
  // Two clicks, so that one slip does not lose a recorded scenario.
  const clear = h("button", { type: "button" }, "Clear order book");
  clear.addEventListener("click", () => {
    if (clear.dataset.armed) {
      delete clear.dataset.armed;
      clear.textContent = "Clear order book";
      api("/api/lis/clear", { session }).catch(toast);
      return;
    }
    clear.dataset.armed = "1";
    clear.textContent = "Really clear all specimens?";
    window.setTimeout(() => { delete clear.dataset.armed; clear.textContent = "Clear order book"; }, 4000);
  });
  body.append(addForm, h("div", { class: "toolbar" }, h("h3", {}, "Order book"),
    h("button", { type: "button", onclick: guarded(() => api("/api/lis/send", { session, specimens: [] })) }, "Send all new orders"),
    clear),
    h("p", { class: "muted" }, "Specimens and tests the instrument sends or asks for are added here; " +
      "add or remove tests to build a scenario."),
    scrollRows(table));

  /** @param {string} specimen @param {string} test @param {string} label @param {string} path */
  const itemButton = (specimen, test, label, path) => h("button", { type: "button", class: "small",
    onclick: guarded(() => api(path, { session, specimen, test })) }, label);

  /** A catalogue test to add to a specimen. @param {Specimen} s */
  const addTest = (s) => {
    const have = new Set(s.items.map((i) => i.test));
    const left = catalog.filter((t) => !have.has(t.code));
    if (left.length === 0) return null;
    const choice = /** @type {HTMLSelectElement} */ (h("select", { class: "add-test", "aria-label": `add a test to ${s.id}` },
      h("option", { value: "" }, "+ test"), ...left.map((t) => h("option", { value: t.code }, `${t.code} ${t.name}`))));
    choice.addEventListener("change", () => {
      if (choice.value !== "") api("/api/lis/test", { session, specimen: s.id, test: choice.value }).catch(toast);
    });
    return choice;
  };

  /** @param {Book} book */
  return (book) => {
    table.replaceChildren(h("thead", {}, h("tr", {},
      ...["Specimen", "Patient", "Tests: state and result", ""].map((t) => h("th", {}, t)))));
    const tb = h("tbody");
    if (book.specimens.length === 0) tb.append(h("tr", {}, h("td", { colspan: 4, class: "muted" }, "No specimens yet.")));
    for (const s of book.specimens) {
      const hasNew = s.items.some((i) => i.state === "new");
      tb.append(h("tr", {},
        h("td", {}, s.id),
        h("td", {}, [s.patient_id, s.patient_name].filter(Boolean).join(" ")),
        h("td", {}, ...s.items.map((i) => h("div", { class: "item" },
          h("b", {}, i.test), " ", badge(i.state),
          i.review ? h("span", {}, " ", badge("for-review")) : i.unsolicited ? h("span", { class: "muted" }, " unsolicited") : null,
          i.result ? ` ${i.result.value} ${i.result.unit} ${i.result.flags}` : "", " ",
          i.review && itemButton(s.id, i.test, "accept", "/api/lis/accept"),
          (i.state === "sent" || i.state === "answered") && itemButton(s.id, i.test, "cancel", "/api/lis/cancel"),
          itemButton(s.id, i.test, i.review ? "discard" : "remove", "/api/lis/test/delete"))),
          addTest(s)),
        h("td", { class: "actions" },
          hasNew && h("button", { type: "button", class: "small", onclick: guarded(() => api("/api/lis/send", { session, specimens: [s.id] })) }, "Send"),
          h("button", { type: "button", class: "small", onclick: guarded(() => api("/api/lis/specimen/delete", { session, id: s.id })) }, "Delete"))));
    }
    table.append(tb);
  };
}

/**
 * The instrument simulator: recent specimens.
 * @param {HTMLElement} body
 */
function deviceView(body) {
  const table = h("table", { class: "results" });
  body.append(h("h3", {}, "Specimens (most recent last)"), scrollRows(table, 5, true));
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

/** Edits not saved yet: kept when the user looks at another tab and comes back. */
let catalogDirty = false;

/** @param {boolean} dirty */
function setCatalogDirty(dirty) {
  catalogDirty = dirty;
  const note = document.getElementById("catalog-unsaved");
  if (note) note.hidden = !dirty;
}

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
      h("td", {}, h("button", { type: "button", class: "small", onclick: () => {
        catalog = readCatalog();  // keep what was typed in the other rows
        catalog.splice(index, 1);
        renderCatalog();
        setCatalogDirty(true);
      } }, "Delete"))));
  });
  table.append(tb);
}

/**
 * Checks the rows as the server will, and marks the first field in error so
 * that the browser points at it. Returns the problems, one per line.
 */
function checkCatalog() {
  /** @type {string[]} */
  const problems = [];
  /** @type {HTMLInputElement | HTMLSelectElement | null} */
  let first = null;
  const codes = new Set();
  Array.from(document.querySelectorAll("#catalog tbody tr")).forEach((row, i) => {
    const r = /** @type {HTMLElement} */ (row);
    const field = (/** @type {string} */ n) => /** @type {HTMLInputElement} */ (input(r, n));
    for (const n of ["code", "low", "high", "sim_low", "sim_high", "values", "abnormal", "decimals"]) field(n).setCustomValidity("");
    const code = field("code").value.trim();
    const label = `Test ${i + 1}${code ? ` (${code})` : ""}`;
    /** @param {string} n @param {string} text */
    const bad = (n, text) => {
      field(n).setCustomValidity(text);
      first ??= field(n);
      problems.push(`${label}: ${text}`);
    };
    const num = (/** @type {string} */ n) => (field(n).value.trim() === "" ? undefined : Number(field(n).value));
    const list = (/** @type {string} */ n) => field(n).value.split(",").map((x) => x.trim()).filter((x) => x !== "");
    if (code === "") bad("code", "a test needs a code");
    else if (/[|\\^&]/.test(code)) bad("code", "a code must not contain | \\ ^ &");
    else if (codes.has(code)) bad("code", `code ${code} appears twice`);
    codes.add(code);
    const low = num("low");
    const high = num("high");
    if (low !== undefined && high !== undefined && low > high) bad("low", "reference low is above high");
    const sl = num("sim_low");
    const sh = num("sim_high");
    if ((sl === undefined) !== (sh === undefined)) bad(sl === undefined ? "sim_low" : "sim_high", "simulate needs both low and high");
    else if (sl !== undefined && sh !== undefined && sl > sh) bad("sim_low", "simulate low is above high");
    const decimals = num("decimals") ?? 0;
    if (!Number.isInteger(decimals) || decimals < 0 || decimals > 6) bad("decimals", "decimals is 0 to 6");
    const values = list("values");
    if (input(r, "kind").value === "qualitative" && values.length === 0) bad("values", "a qualitative test needs values, e.g. POS, NEG");
    const stray = list("abnormal").filter((a) => !values.includes(a));
    if (stray.length > 0) bad("abnormal", `${stray.join(", ")} not among the values`);
  });
  /** @type {HTMLInputElement | HTMLSelectElement | null} */ (first)?.reportValidity();
  return problems;
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
  document.getElementById("catalog")?.addEventListener("input", () => setCatalogDirty(true));
  document.getElementById("catalog")?.addEventListener("change", () => setCatalogDirty(true));
  document.getElementById("catalog-add")?.addEventListener("click", () => {
    catalog = readCatalog();
    catalog.push({ code: "", name: "", kind: "numeric", unit: "", decimals: 0, values: [], abnormal: [] });
    renderCatalog();
    setCatalogDirty(true);
    /** @type {HTMLInputElement | null} */ (document.querySelector("#catalog tbody tr:last-child input[name=code]"))?.focus();
  });
  document.getElementById("catalog-save")?.addEventListener("click", guarded(async () => {
    const problems = checkCatalog();
    if (problems.length > 0) {
      toast(`Not saved. ${problems.join("; ")}`);
      return;
    }
    await api("/api/catalog/save", { lisbench: "catalog/1", tests: readCatalog() });
    catalog = (await api("/api/catalog")).tests;
    renderCatalog();
    setCatalogDirty(false);
    toast("Catalogue saved.", true);
  }));
  window.addEventListener("beforeunload", (ev) => {
    if (catalogDirty) ev.preventDefault();  // the browser asks before the edits are lost
  });
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

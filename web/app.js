// @ts-check
// lisbench web UI. Plain JavaScript, checked by the TypeScript compiler
// (tsc --noEmit --checkJs); no build step. Commands go to /api as POST with
// JSON, live data arrives on the event stream /api/events.

/**
 * @typedef {{t: string, from: string, kind: string, lines: string[]}} MonitorEvent
 * @typedef {{name: string, size: number, kind: "capture" | "message" | "scenario"}} DataFile
 * @typedef {{id: string, purpose: string, level: string, verdict: string, detail: string,
 *            measured_ms: number | null}} CaseResult
 * @typedef {{running: boolean, [key: string]: unknown}} TaskState
 * @typedef {{version: string, data_dir: string, monitor: TaskState, sim: TaskState,
 *            conform: TaskState}} Status
 */

const MAX_ROWS = 5000;
const HANDSHAKE = new Set(["control", "frame", "repeated"]);

/**
 * @template {HTMLElement} T
 * @param {string} selector
 * @param {new () => T} type
 * @returns {T}
 */
function $(selector, type) {
  const el = document.querySelector(selector);
  if (!(el instanceof type)) {
    throw new Error(`missing element ${selector}`);
  }
  return el;
}

/** @param {string} text */
function toast(text) {
  const t = $("#toast", HTMLDivElement);
  t.textContent = text;
  t.hidden = false;
  window.clearTimeout(Number(t.dataset.timer ?? 0));
  t.dataset.timer = String(window.setTimeout(() => { t.hidden = true; }, 6000));
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

/** @param {string} text */
function lines(text) {
  return text.split("\n").map((l) => l.trim()).filter((l) => l !== "");
}

/** @param {HTMLFormElement} form */
function fields(form) {
  /** @type {Record<string, string | boolean | string[]>} */
  const out = {};
  for (const el of Array.from(form.elements)) {
    if (el instanceof HTMLInputElement && el.name) {
      out[el.name] = el.type === "checkbox" ? el.checked : el.value.trim();
    } else if (el instanceof HTMLTextAreaElement && el.name) {
      out[el.name] = el.value;
    } else if (el instanceof HTMLSelectElement && el.name) {
      out[el.name] = el.multiple ? Array.from(el.selectedOptions, (o) => o.value) : el.value;
    }
  }
  return out;
}

// --- event tables -----------------------------------------------------------

/**
 * Appends one event as a row; the first line is shown, the rest (record
 * fields) on click.
 * @param {HTMLElement} table
 * @param {MonitorEvent} e
 * @param {boolean} showHandshake
 */
function addRow(table, e, showHandshake) {
  const row = document.createElement("div");
  const handshake = HANDSHAKE.has(e.kind);
  row.className = `row ${e.kind}${handshake ? " handshake" : ""}`;
  row.hidden = handshake && !showHandshake;
  const [first = "", ...rest] = e.lines;
  row.append(first);
  if (rest.length > 0) {
    row.classList.add("has-detail");
    const detail = document.createElement("div");
    detail.className = "detail";
    detail.textContent = rest.join("\n");
    row.append(detail);
    row.addEventListener("click", () => row.classList.toggle("open"));
  }
  table.append(row);
  while (table.childElementCount > MAX_ROWS) {
    table.firstElementChild?.remove();
  }
}

/**
 * @param {HTMLElement} table
 * @param {boolean} show
 */
function setHandshake(table, show) {
  for (const row of Array.from(table.querySelectorAll(".row.handshake"))) {
    if (row instanceof HTMLElement) {
      row.hidden = !show;
    }
  }
}

// --- views ------------------------------------------------------------------

// The Tools view: the v0.7 views, one at a time.
function setupNav() {
  const buttons = Array.from(document.querySelectorAll("#tools-nav button"));
  for (const button of buttons) {
    if (!(button instanceof HTMLButtonElement)) continue;
    button.addEventListener("click", () => {
      for (const b of buttons) {
        b.classList.toggle("active", b === button);
      }
      for (const v of Array.from(document.querySelectorAll(".tool-view"))) {
        if (v instanceof HTMLElement) v.hidden = v.id !== `view-${button.dataset.view}`;
      }
      if (button.dataset.view === "captures" || button.dataset.view === "sim") {
        void loadFiles();
      }
    });
  }
}

const monitor = {
  form: $("#monitor-form", HTMLFormElement),
  table: $("#monitor-table", HTMLDivElement),
  handshake: $("#monitor-handshake", HTMLInputElement),
  follow: $("#monitor-follow", HTMLInputElement),

  setup() {
    this.form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      const f = fields(this.form);
      try {
        await api("/api/monitor/start", {
          a: f.a, b: f.b, tap: f.tap, save: f.save, profile: f.profile,
          labels: String(f.labels).split(",").map((s) => s.trim()),
          watches: lines(String(f.watches)),
        });
        this.table.replaceChildren();
        $("#monitor-summary", HTMLPreElement).textContent = "";
      } catch (e) {
        toast(String(e instanceof Error ? e.message : e));
      }
    });
    $("#monitor-stop", HTMLButtonElement).addEventListener("click",
      () => api("/api/monitor/stop", {}).catch((e) => toast(e.message)));
    $("#monitor-clear", HTMLButtonElement).addEventListener("click", () => this.table.replaceChildren());
    this.handshake.addEventListener("change", () => setHandshake(this.table, this.handshake.checked));
  },

  /** @param {MonitorEvent} e */
  add(e) {
    addRow(this.table, e, this.handshake.checked);
    if (this.follow.checked) {
      this.table.scrollTop = this.table.scrollHeight;
    }
  },
};

/** @type {DataFile[]} */
let files = [];

async function loadFiles() {
  try {
    files = await api("/api/files");
  } catch (e) {
    toast(String(e instanceof Error ? e.message : e));
    return;
  }
  const list = $("#capture-list", HTMLUListElement);
  list.replaceChildren();
  for (const f of files.filter((x) => x.kind === "capture")) {
    const li = document.createElement("li");
    const b = document.createElement("button");
    b.type = "button";
    const name = document.createElement("span");
    name.textContent = f.name;
    const size = document.createElement("span");
    size.className = "size";
    size.textContent = `${(f.size / 1024).toFixed(1)} kB`;
    b.append(name, size);
    b.addEventListener("click", () => void openCapture(f.name));
    li.append(b);
    list.append(li);
  }
  if (list.childElementCount === 0) {
    list.textContent = "No captures yet. Save one from the Monitor view.";
  }
  const sim = $("#sim-form", HTMLFormElement);
  /** @param {string} name @param {DataFile["kind"]} kind @param {boolean} keepFirst */
  const fill = (name, kind, keepFirst) => {
    const select = sim.elements.namedItem(name);
    if (!(select instanceof HTMLSelectElement)) return;
    const first = keepFirst ? select.options[0] : null;
    select.replaceChildren(...(first ? [first] : []));
    for (const f of files.filter((x) => x.kind === kind)) {
      select.append(new Option(f.name, f.name));
    }
  };
  fill("send", "message", false);
  fill("answer", "message", true);
  fill("scenario", "scenario", true);
}

/** @param {string} name */
async function openCapture(name) {
  const params = new URLSearchParams({ name });
  const watch = $("#capture-watch", HTMLInputElement).value.trim();
  const profile = $("#capture-profile", HTMLInputElement).value.trim();
  if (watch) params.set("watch", watch);
  if (profile) params.set("profile", profile);
  try {
    const c = await api(`/api/capture?${params}`);
    $("#capture-title", HTMLHeadingElement).textContent =
      `${c.name}: ${c.events.length} events${c.truncated ? " (truncated)" : ""}`;
    const table = $("#capture-table", HTMLDivElement);
    table.replaceChildren();
    for (const e of /** @type {MonitorEvent[]} */ (c.events)) {
      addRow(table, e, false);
    }
    $("#capture-summary", HTMLPreElement).textContent = c.summary.join("\n");
  } catch (e) {
    toast(String(e instanceof Error ? e.message : e));
  }
}

const conform = {
  form: $("#conform-form", HTMLFormElement),
  log: $("#conform-log", HTMLPreElement),
  /** @type {unknown} */
  last: null,

  async setup() {
    try {
      const suite = /** @type {{id: string, purpose: string, level: string}[]} */ (await api("/api/suite"));
      const box = $("#conform-cases", HTMLDivElement);
      for (const tc of suite) {
        const label = document.createElement("label");
        label.className = "check";
        const input = document.createElement("input");
        input.type = "checkbox";
        input.value = tc.id;
        label.append(input, `${tc.id} ${tc.purpose}${tc.level === "should" ? " (should)" : ""}`);
        box.append(label);
      }
    } catch (e) {
      toast(String(e instanceof Error ? e.message : e));
    }
    this.form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      const f = fields(this.form);
      const only = Array.from(this.form.querySelectorAll("#conform-cases input:checked"),
        (el) => /** @type {HTMLInputElement} */ (el).value);
      try {
        await api("/api/conform/start", {
          iut: f.iut, endpoint: f.endpoint, profile: f.profile, fast: f.fast,
          wait: Number(f.wait), only,
        });
        this.log.textContent = "";
        $("#conform-results", HTMLTableElement).replaceChildren();
      } catch (e) {
        toast(String(e instanceof Error ? e.message : e));
      }
    });
    $("#conform-stop", HTMLButtonElement).addEventListener("click",
      () => api("/api/conform/stop", {}).catch((e) => toast(e.message)));
    $("#conform-download", HTMLButtonElement).addEventListener("click", () => {
      const blob = new Blob([JSON.stringify(this.last, null, 2)], { type: "application/json" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = "conformance.json";
      a.click();
      URL.revokeObjectURL(a.href);
    });
  },

  /** @param {{results: CaseResult[], summary: string, stopped: boolean}} r */
  show(r) {
    this.last = r;
    $("#conform-download", HTMLButtonElement).disabled = false;
    const table = $("#conform-results", HTMLTableElement);
    table.replaceChildren();
    const head = table.createTHead().insertRow();
    for (const h of ["Case", "Verdict", "Level", "Test purpose", "Observed"]) {
      const th = document.createElement("th");
      th.textContent = h;
      head.append(th);
    }
    const body = table.createTBody();
    for (const c of r.results) {
      const tr = body.insertRow();
      tr.insertCell().textContent = c.id;
      const v = document.createElement("span");
      v.className = `verdict ${c.verdict.toLowerCase()}`;
      v.textContent = c.verdict;
      tr.insertCell().append(v);
      tr.insertCell().textContent = c.level;
      tr.insertCell().textContent = c.purpose;
      const d = tr.insertCell();
      d.className = "detail";
      d.textContent = c.detail;
    }
    this.log.textContent += `${r.summary}${r.stopped ? " (stopped)" : ""}\n`;
  },
};

const sim = {
  form: $("#sim-form", HTMLFormElement),
  log: $("#sim-log", HTMLDivElement),

  setup() {
    this.form.addEventListener("submit", async (ev) => {
      ev.preventDefault();
      const f = fields(this.form);
      try {
        await api("/api/sim/start", {
          scenario: f.scenario, role: f.role, endpoint: f.endpoint, send: f.send,
          answer: f.answer, faults: lines(String(f.faults)), serve: f.serve, fast: f.fast,
        });
        this.log.replaceChildren();
      } catch (e) {
        toast(String(e instanceof Error ? e.message : e));
      }
    });
    $("#sim-stop", HTMLButtonElement).addEventListener("click",
      () => api("/api/sim/stop", {}).catch((e) => toast(e.message)));
  },

  /** @param {{t: string, text: string, lines: string[]}} e */
  add(e) {
    addRow(this.log, { t: e.t, from: "", kind: "sim", lines: [`${e.t} ${e.text}`, ...e.lines] }, true);
    this.log.scrollTop = this.log.scrollHeight;
  },
};

/** @param {Status} s */
function showStatus(s) {
  $("#version", HTMLSpanElement).textContent = `lisbench ${s.version}`;
  $("#data-dir", HTMLSpanElement).textContent = s.data_dir || "(no --captures directory)";
  /** @type {[string, TaskState, string][]} */
  const tasks = [["monitor", s.monitor, "monitor"], ["sim", s.sim, "simulator"], ["conform", s.conform, "test"]];
  for (const [id, state, what] of tasks) {
    $(`#${id}-start`, HTMLButtonElement).disabled = state.running;
    $(`#${id}-stop`, HTMLButtonElement).disabled = !state.running;
    $(`#${id}-state`, HTMLSpanElement).textContent = state.running ? `${what} running` : "";
  }
}

function connect() {
  const link = $("#link", HTMLSpanElement);
  const source = new EventSource("/api/events");
  /** @param {string} name @param {(data: any) => void} fn */
  const on = (name, fn) => source.addEventListener(name, (ev) => {
    if (ev instanceof MessageEvent) fn(JSON.parse(ev.data));
  });
  source.addEventListener("open", () => {
    link.textContent = "live";
    link.className = "pill on";
    api("/api/status").then(showStatus).catch(() => {});
  });
  source.addEventListener("error", () => {
    link.textContent = "offline";
    link.className = "pill off";
  });
  on("status", showStatus);
  on("monitor", (e) => monitor.add(e));
  on("monitor-info", (e) => monitor.add({ t: "", from: "", kind: "info", lines: [e.text] }));
  on("summary", (e) => {
    if (e.source === "monitor") $("#monitor-summary", HTMLPreElement).textContent = e.lines.join("\n");
  });
  on("conform", (e) => { conform.log.textContent += `${e.line}\n`; });
  on("result", (r) => conform.show(r));
  on("sim", (e) => sim.add(e));
  on("problem", (e) => toast(`${e.source}: ${e.text}`));
}

setupNav();
monitor.setup();
void conform.setup();
sim.setup();
connect();
void loadFiles();
api("/api/conform/result").then((r) => { if (r && r.results) conform.show(r); }).catch(() => {});

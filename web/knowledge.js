// @ts-check
// Knowledge page: the field tables come from lisbench's record table
// (/api/lis02/records, from src/lis02_records.cpp); codes and notes per field
// live here, keyed "<record type>.<field name>". Our own words, 2026-10-26;
// tests/test_web.cpp checks that every key names a field of the table.

/**
 * @typedef {{type: string, name: string, fields: string[]}} RecordInfo
 * @typedef {{std?: string, codes?: [string, string][], conv?: string, issue?: string, ref?: string}} Note
 */

/** @type {Record<string, Note>} */
const NOTES = {
  "H.delimiter_definition": {
    std: "Repeat, component and escape delimiter; the field delimiter is the character before it.",
    conv: "\\^& with | as field delimiter.",
    issue: "Parsed with escapes applied, which breaks on the escape delimiter itself.",
  },
  "H.message_control_id": { std: "An ID for this message.", conv: "Mostly empty." },
  "H.sender_name": { conv: "name^version^serial of the instrument or LIS." },
  "H.receiver_id": { conv: "Often the LIS name or empty; few receivers check it." },
  "H.processing_id": {
    codes: [["P", "production"], ["T", "training"], ["D", "debugging"], ["Q", "quality control"]],
    issue: "Often empty; a receiver that requires P rejects good messages.",
  },
  "H.version": {
    conv: "Seen as LIS2-A2, LIS2-A, E1394-97, 1394-97 or 1.",
    issue: "Do not reject a message for its version string.",
  },
  "H.timestamp": { std: "When the message was made, YYYYMMDDHHMMSS." },

  "P.sequence_number": { std: "1, 2, ... per message." },
  "P.practice_patient_id": {
    std: "Patient ID assigned by the ordering side (the LIS).",
    conv: "The ID a LIS sends with orders; instruments echo it in results.",
  },
  "P.laboratory_patient_id": {
    std: "Patient ID assigned by the laboratory.",
    issue: "Instruments that number their own patients send their ID here instead of P-3.",
    ref: "The LIS simulator takes P-3, else P-4, as the patient ID of a specimen it learns from a result.",
  },
  "P.patient_name": { std: "Last^first^middle^suffix^title.", issue: "8-bit characters without agreement." },
  "P.birthdate": { std: "YYYYMMDD." },
  "P.sex": { codes: [["M", "male"], ["F", "female"], ["U", "unknown"]] },
  "P.location": { conv: "Ward or clinic; used by some instruments for sorting." },

  "O.sequence_number": { std: "Restarts at 1 under each P." },
  "O.specimen_id": {
    std: "Specimen ID assigned by the computer system: the barcode.",
    conv: "The key for matching orders, queries and results.",
    ref: "Order book key in the LIS simulator.",
  },
  "O.instrument_specimen_id": {
    std: "Specimen ID assigned by the instrument.",
    conv: "Rack^position or a sequence number.",
    issue: "Some instruments send the barcode only here and leave O-3 empty.",
  },
  "O.universal_test_id": {
    std: "Four components: universal ID, name, type, manufacturer's (local) code.",
    conv: "^^^CODE; several tests with the repeat delimiter, or one O per test.",
    issue: "Code in component 1, or extra components (dilution, replicate) after the code.",
  },
  "O.priority": {
    codes: [["S", "stat"], ["A", "as soon as possible"], ["R", "routine"], ["C", "callback"], ["P", "preoperative"]],
    ref: "The LIS simulator sends R.",
  },
  "O.requested_at": { std: "When the test was ordered." },
  "O.collected_at": { std: "When the specimen was collected." },
  "O.action_code": {
    codes: [["C", "cancel the order"], ["A", "add tests to an existing specimen"], ["N", "new order with a new specimen"],
      ["P", "pending specimen"], ["L", "reserved"], ["X", "specimen or test already in process"], ["Q", "QC specimen"]],
    ref: "The LIS simulator sends N, and C to cancel.",
  },
  "O.received_at": { std: "When the laboratory received the specimen." },
  "O.specimen_descriptor": { std: "Specimen type^source.", conv: "Serum, Plasma, Urine, Blood; free text in practice." },
  "O.reported_at": { std: "When results were reported or last changed." },
  "O.report_type": {
    codes: [["O", "order: please perform"], ["C", "correction of reported results"], ["P", "preliminary results"],
      ["F", "final results"], ["X", "cannot be done, order cancelled"], ["I", "in instrument pending"],
      ["Y", "no order on record for this test (query answer)"], ["Z", "no record of this patient (query answer)"],
      ["Q", "response to a query"]],
    issue: "Instruments differ in which of Y, Z or an empty answer they understand as \"no order\".",
    ref: "LIS session setting for a specimen without order: Y record or H and L only.",
  },

  "R.sequence_number": { std: "Restarts at 1 under each O." },
  "R.universal_test_id": {
    std: "As O-5; names the test of this result.",
    conv: "Instruments add components after the code: dilution, replicate, result kind.",
  },
  "R.value": {
    std: "The measurement or text result.",
    issue: "Non-numeric values in numeric tests: <0.5, >500, ***, error text.",
    ref: "Watches with < and > compare only numeric values.",
  },
  "R.units": { conv: "Free text: mg/dL, mmol/L, %. Case and spelling differ between instruments." },
  "R.reference_ranges": { conv: "Forms seen: 70 to 110, 70-110, 70^110; often empty." },
  "R.abnormal_flags": {
    codes: [["L", "below low normal"], ["H", "above high normal"], ["LL", "below panic low"], ["HH", "above panic high"],
      ["<", "below absolute low (off scale)"], [">", "above absolute high"], ["N", "normal"], ["A", "abnormal"],
      ["U", "significant change up"], ["D", "significant change down"], ["B", "better"], ["W", "worse"]],
    issue: "Instrument-specific flags (error or measurement flags) in this field or in C records.",
  },
  "R.abnormality_nature": { codes: [["A", "age-based"], ["S", "sex-based"], ["R", "race-based"], ["N", "general normal range"]] },
  "R.result_status": {
    codes: [["C", "correction"], ["P", "preliminary"], ["F", "final"], ["X", "cannot be done"], ["I", "in instrument pending"],
      ["S", "partial, not final"], ["M", "MIC level"], ["R", "previously transmitted"], ["N", "needed to run a new order"],
      ["Q", "response to a query"], ["V", "operator verified"], ["W", "warning: validity questionable"]],
    ref: "Order item states: P/I preliminary, F final, C corrected, X cannot be done.",
  },
  "R.operator_id": { std: "Performing^verifying operator." },
  "R.started_at": { std: "When the test started." },
  "R.completed_at": { std: "When the test was completed.", issue: "Instrument clock not synchronised." },
  "R.instrument_id": { conv: "Module or analyzer ID in a system with several units." },

  "C.comment_source": { codes: [["P", "practice"], ["L", "LIS (computer system)"], ["I", "instrument"]] },
  "C.comment_text": { issue: "Unescaped delimiters in free text." },
  "C.comment_type": {
    codes: [["G", "generic / free text"], ["T", "test name"], ["P", "positive test"], ["N", "negative test"],
      ["I", "instrument flag comment"]],
  },

  "Q.starting_range_id": {
    std: "Patient ID^specimen ID^further components, or ALL.",
    conv: "^S-1: specimen ID in component 2, component 1 empty.",
    issue: "Specimen ID in component 1, or in component 3 with rack data.",
    ref: "The LIS simulator reads component 2, else component 1.",
  },
  "Q.ending_range_id": { conv: "Empty for a single specimen." },
  "Q.universal_test_id": { conv: "ALL, or empty: every test." },
  "Q.request_time_limits_nature": { codes: [["S", "specimen collection time"], ["R", "result test time"]] },
  "Q.request_status": {
    codes: [["C", "corrections"], ["P", "preliminary"], ["F", "final"], ["X", "cannot be done"], ["I", "in instrument pending"],
      ["S", "unfinalized"], ["M", "MIC"], ["R", "previously transmitted"], ["A", "abort the last request"],
      ["N", "new or changed results only"], ["O", "orders and demographics only"], ["D", "demographics only"]],
    conv: "Instruments asking for their work send O.",
  },

  "L.sequence_number": { std: "Always 1." },
  "L.termination_code": {
    codes: [["N", "normal"], ["T", "sender aborted"], ["R", "receiver requested abort"], ["E", "unknown system error"],
      ["Q", "error in the last request for information"], ["I", "no information for the last query"],
      ["F", "last request for information processed"]],
    issue: "Often N in every case, also in query answers.",
  },

  "S.record_type": { std: "Reserved for scientific data; hardly used in practice." },
  "M.record_type": { std: "Defined by the manufacturer: the fields after the sequence number are the maker's." },
};

/** @param {string} tag @param {string} [cls] @param {(Node | string)[]} children */
function el(tag, cls, ...children) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  e.append(...children);
  return e;
}

/** @param {Note | undefined} n */
function noteCell(n) {
  const td = el("td");
  if (!n) return td;
  if (n.std) td.append(el("div", "note-std", n.std));
  if (n.codes) td.append(el("div", "codes", n.codes.map(([c, m]) => `${c} ${m}`).join(" · ")));
  if (n.conv) td.append(el("div", "note-conv", n.conv));
  if (n.issue) td.append(el("div", "note-issue", n.issue));
  if (n.ref) td.append(el("div", "note-ref", n.ref));
  return td;
}

/** @param {RecordInfo[]} records */
function render(records) {
  const box = document.getElementById("field-tables");
  const filter = document.getElementById("filter");
  const noted = document.getElementById("noted");
  if (!box || !(filter instanceof HTMLInputElement) || !(noted instanceof HTMLInputElement)) return;
  const draw = () => {
    const words = filter.value.trim().toLowerCase();
    box.replaceChildren();
    for (const r of records) {
      const rows = r.fields.map((f, i) => ({ f, n: i + 1, note: NOTES[`${r.type}.${f}`] }))
        .filter(({ f, n, note }) => (!noted.checked || note) && (words === "" ||
          `${r.type}-${n} ${r.name} ${f.replaceAll("_", " ")} ${JSON.stringify(note ?? {})}`.toLowerCase().includes(words)));
      if (rows.length === 0) continue;
      const tbody = el("tbody");
      for (const { f, n, note } of rows) {
        tbody.append(el("tr", undefined, el("td", "num", `${r.type}-${n}`), el("td", undefined, f.replaceAll("_", " ")), noteCell(note)));
      }
      const head = el("tr");
      for (const t of ["Field", "Name", "Standard codes and notes"]) head.append(el("th", undefined, t));
      box.append(el("h3", undefined, `${r.type}: ${r.name}`), el("table", "results", el("thead", undefined, head), tbody));
    }
    if (box.childElementCount === 0) box.append(el("p", "muted", "No field matches."));
  };
  filter.addEventListener("input", draw);
  noted.addEventListener("change", draw);
  draw();
}

fetch("/api/lis02/records")
  .then((r) => r.json())
  .then(render)
  .catch((e) => {
    const box = document.getElementById("field-tables");
    if (box) box.textContent = `The record table could not be loaded: ${e}`;
  });

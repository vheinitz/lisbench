# lisbench – feature specification

Written 2026-10-25 by Claude Code for Valentin Heinitz. Request: "So wollen
wir nach Plan vorgehen? MArktanalyse, Related products, Dann features, dann
tests, Dann lib der Geräte …" and "ok, warte auf die Marktanalyse, dann immer
weiter nicht interactiv".

## 1. Purpose and boundary

A workbench for developers and integrators of laboratory instrument
interfaces: understand, simulate, observe, disturb and document a link
between an instrument and a LIS. **Not** clinical middleware: lisbench does
not route patient results in production, so it stays outside IVD software
regulation.

## 2. Position (market check of 2026-10-25)

Already solved well, therefore not rebuilt: plain serial-to-TCP transport
(ser2net, socat), HL7 editors (7edit, HL7 Soup), full interface engines (Mirth
Connect – proprietary since 4.6 –, Open Integration Engine), clinical
middleware (Data Innovations Instrument Manager, closed driver catalogue).

Gaps that lisbench addresses, in this order of weight:

1. An open, protocol-faithful LIS01-A2/LIS02-A2 toolkit for developers. Open
   libraries are stale or limited to one device; one popular agent
   acknowledges frames without checking the checksum.
2. A monitor/man-in-the-middle that decodes frames and fields live and has
   watches and triggers on field values. None found, also no Wireshark
   dissector.
3. Fault injection (NAK, timeouts, wrong checksums, frame numbers, contention).
   Commercial only.
4. An open, readable knowledge base of instrument profiles (implemented
   fields, deviations, known bugs, charset, examples with sources). The only
   comparable catalogue is closed; OpenELIS has profiles as code.

Not yet proven to be in demand, therefore late in the plan: the database
mapper and microcontroller simulators.

## 3. Use cases

| ID | Use case |
|---|---|
| U1 | Inspect a capture: frames, checksums, records, fields by name |
| U2 | Simulate an instrument against a LIS under test (results, queries) |
| U3 | Simulate a LIS against an instrument (orders, query answers) |
| U4 | Sit between instrument and LIS, log both directions, decode, watch fields, trigger on values |
| U5 | Inject faults to test the error handling of either side |
| U6 | Bridge serial↔TCP, record serial or TCP to file, swap client/server roles |
| U7 | Generate messages from a database through a declarative mapping |
| U8 | Look up an instrument profile; let it set tolerances and drive simulators |

## 4. Releases

| Release | Content | Acceptance |
|---|---|---|
| **v0.1** (done) | core: LIS01-A2 framing and link state machine, LIS02-A2 records, codec, validation, builder; CLI `frame/deframe/decode/validate` | tests in docs/spec/tests.md §2 green on GCC/Clang/MSVC, sanitizers clean |
| **v0.2** (done) | `io` (asio: serial, TCP client/server) and `sim`: instrument and LIS roles, scenarios as YAML, query answers, fault injection | end-to-end sim↔sim over TCP and over a pty pair |
| **v0.3** (done) | `monitor`: both directions with timestamps, decoded view, watches on fields (`R.value > 100`, `O.specimen_id == …`), capture file and replay, passive tap; Wireshark Lua dissector moved to later | replay of a recorded session gives identical decoded output |
| **v0.4** (done) | `bridge`: serial↔TCP, TCP client↔server (role swap), reconnect in serve mode; `record`: serial or TCP → capture/raw file; tap mode is part of `monitor` | byte-transparent for 200 kB random data each way |
| **v0.5** (done) | `mapper`: SQLite → messages by declarative YAML (nested queries with named scopes `${patient.pid}`, `${cfg.x}`/`${arg.x}` via lexer, bound parameters, `skip_if_empty`, write-back with bound parameters, stable message control ID); `lisbench map` to stdout, TCP/serial or files | mapper acceptance list, §5 |
| **v0.6** (done) | microcontroller instrument simulators: `LisDevice` library, Arduino Uno sketch (serial) and ESP32 sketch (Wi-Fi/TCP) playing an analyzer as LIS client; moved before the web UI because it lets everyone try the tools at once | host tests against the lisbench link; compiled for both boards in CI; first hardware run in v0.6.2, 24 h still open |
| v0.6.1 (done) | first instrument profile (Cepheid GeneXpert, from the public specification); local escape `Z` as UTF-16; configurable contention winner; link check in the monitor | examples and scenarios of every profile parsed and loaded in CI |
| v0.6.2 (done) | first hardware run (Uno-compatible board, 9600 baud): link re-acknowledges a repeated ENQ; INCOMPLETE shown in `sim`; readable `--trace` | normal, NAK, ENQ-NAK and timeout cases on the board |
| v0.6.3 (done, in v0.6.4) | triggers on watch hits (`exec` with the hit in the environment, `mark` in the capture, `stop`); fast test timing for board tests; serial backlog dropped on open | trigger, command queue and capture tests; on the board: exec, mark, stop, and no stale burst after a minute without listener |
| v0.6.4 (done) | `--profile <id>` for decode, validate, monitor and sim: text options, frame length and contention rule from `profile.yaml` (U8, first part) | profile tests; GeneXpert example decoded with `\Z` escapes by profile alone |
| v0.6.5 (done) | `lisbench conform`: LIS01-A2 conformance test suite, 14 test cases (section 7) | own LIS, own instrument and LisDevice pass all cases; a receiver that acknowledges everything fails TR02/TR03 |
| v0.6.6 (done) | receivers refuse restricted characters in frame text (link and LisDevice) | conform TR07 without note for all own implementations |
| **v0.7.0** (done) | web UI (`lisbench serve`, section 8): monitor, captures, conformance test, simulator; own HTTP/SSE server on asio, pages built into the binary | HTTP parser and limits unit-tested; API end to end (monitor between simulators, capture, conformance, simulator, bad file names, double start); checked in Chrome |
| v0.7.1 | Debian packages arm64/armhf/amd64 (Debian 13), systemd service, release assets from CI | built and tested in CI (ARM under qemu), amd64 installed, served and purged in CI; on a real Pi 3B+ still to be confirmed |
| v0.8 (done) to v0.10 | sessions: session files, shared test catalogue, LIS and instrument simulators as their own session types with order book and order-result matching, several instances, home tab; then separate order/result ports, monitor/conform sessions, export editor (section 9) | section 9.9 |
| later | library export (`find_package`) | |
| later | Wireshark Lua dissector; RP2040 passive RS-232 tap; HL7 v2 over MLLP (IHE LAW) as own module | |

WIP limit: no web UI before v0.3.

Found while building v0.2: a sender that gives up ends the transfer with EOT
as well, so on LIS01-A2 level an aborted transfer looks complete. Receivers
(and the monitor) must judge completeness by the L record.

## 5. Mapper acceptance (v0.5)

From the clean-room requirements in dev_plan.md:

1. Variables are scoped per query level; an inner column of the same name does
   not change the outer one; a missing or NULL column is an error with line
   number, never a leftover value.
2. A record that does not fit the hierarchy aborts the message with an error.
3. Every value, from the database or from configuration, goes through the
   same escaping; placeholders are resolved by a lexer, an unterminated
   placeholder is an error.
4. SQL uses bound parameters only.
5. The mapping file is read as UTF-8 (BOM tolerated) regardless of locale.
6. A record whose assigned fields are all empty can be skipped (`skip_if_empty`).
7. Timestamps come from the application in local time with offset, not from SQL arithmetic.
8. File output: temp file + rename; the name pattern must contain a unique
   part, checked at start.
9. Write-back after send: success means transport acknowledgement only
   (LIS02-A2 has no application-level acknowledgement); a failed write-back is
   reported, never silent.

## 6. Instrument profiles

One directory per instrument under `docs/devices/<vendor>-<model>/`:
`profile.yaml` (machine-readable: transport, LIS01-A2 deviations, delimiters,
charset, supported records and fields, timing), `README.md` (quirks, known
bugs, workarounds), `examples/` (messages with source: document, revision,
page). Sources allowed: freely downloadable manuals (linked, not copied),
open source, own reverse engineering, contributions that name their source.
Template: [docs/devices/TEMPLATE.md](../devices/TEMPLATE.md).

## 7. Conformance test suite (`lisbench conform`)

Added 2026-10-25. Request: "ja, erst conform, dann Webinterface" and
"ja, Standardterminologie verwenden". Terms after ISO/IEC 9646 (ITU-T X.290):
the **IUT** (implementation under test, a LIS or an instrument) is reached
over TCP or serial; lisbench is the **tester** and plays the other role. Each
**test case** checks one **test purpose** and ends with a **verdict**:
*pass*, *fail*, or *inconclusive* when the behaviour could not be observed
(the IUT sent nothing, the message had one frame only). A deviation from a
*should* requirement is *pass* with a note, never *fail*. An instrument
profile (`--profile`) serves as the **PICS**: where it states a permitted
option (e.g. ignoring the receiver interrupt), the observed behaviour is
compared with it. Rules are described in own words; no text of the standard
is copied and no section numbers are quoted.

Timing expectations come from LIS01-A2 (or `--fast-timing` for devices built
with the test preset). A measured time passes from 95 % of the nominal value;
an upper bound is checked where the rule has one.

| Test case | Test purpose (LIS01-A2) | Level |
|---|---|---|
| TR01 | IUT as receiver: accepts a valid message, every frame answered with ACK | shall |
| TR02 | rejects a frame with a wrong checksum with NAK | shall |
| TR03 | rejects a frame whose number is neither the last accepted nor the next one with NAK | shall |
| TR04 | accepts a repeated frame (same number as the last accepted, as after a lost ACK) with ACK | shall |
| TR05 | accepts intermediate frames (ETB), 240 characters of text and frame numbers wrapping from 7 to 0 | shall |
| TR06 | answers ENQ and every frame within the sender's 15 s reply timeout (largest reply time reported) | shall |
| TR07 | rejects a frame with a restricted character (LF) in its text | should |
| TS01 | IUT as sender: every frame well formed (checksum, number sequence from 1 modulo 8, at most 240 characters of text, ETB/ETX, CR LF) | shall |
| TS02 | repeats a frame answered with NAK, with the same frame number | shall |
| TS03 | gives up with EOT after six transmissions of a refused frame | shall |
| TS04 | gives up with EOT when a frame is not answered within 15 s | shall |
| TS05 | after ENQ is answered with NAK, waits at least 10 s before the next ENQ | shall |
| TS06 | contention (ENQ answered with ENQ): an instrument re-sends ENQ after at least 1 s; a LIS backs off and accepts the tester's ENQ | shall |
| TS07 | receiver interrupt (frame answered with EOT): the IUT may finish or stop; if it stops, no new ENQ for 15 s. Compared with the profile | shall |

For a LIS as IUT the TS cases need it to send: the tester sends a query
before each one and the LIS's answer is the message under test. An
instrument is expected to send by itself; the tester waits for its ENQ.
Traffic the IUT starts between cases is received and acknowledged normally.

Not in this suite (not observable on the link alone): duplicate suppression
in the IUT's application, the receiver's 30 s timeout, LIS02-A2 content.

## 8. Web UI (v0.7)

Added 2026-10-25. Decisions of the requester: HTTP base "Eigener kleiner
Server" (own small server on the existing asio, not Crow or cpp-httplib);
scope "Live-Monitor, Mitschnitte abspielen, Konformitätstest, Simulator
steuern".

`lisbench serve [--http 127.0.0.1:8080] [--captures DIR]` runs one daemon
with one event loop: the HTTP server, the monitor, the simulator and the
conformance tester share it, so no state crosses threads.

- Transport: HTTP/1.1. The browser sends commands as `POST /api/...` with
  JSON; live data comes as Server-Sent Events from `GET /api/events`
  (one-way is all the views need, and it needs no WebSocket handshake).
- Assets (HTML, CSS, script) are compiled into the binary, so the Pi
  package is one file. The script is plain JavaScript checked by the
  TypeScript compiler (`// @ts-check`, `tsc --noEmit` in CI): type checking
  without a build step for users.
- Security: listens on 127.0.0.1 unless told otherwise; another address
  prints a warning that anyone on that network can start connections from
  this machine. No authentication in v0.7.
- Limits: request line and headers 8 kB, body 1 MB, at most 16 event
  streams; a slow event reader is dropped instead of buffering without
  bound.

| View | What it does |
|---|---|
| Monitor | endpoints A and B (or tap), watches; live table of records and handshake, record fields on click, watch hits highlighted; start/stop, save to a capture in `--captures` |
| Captures | list of capture files in `--captures`; open one: the same table, marks shown |
| Conformance | IUT role, endpoint, fast timing, profile; progress live, report table, JSON download |
| Simulator | role, endpoint, message file or scenario from `--captures`/examples, faults; start/stop, transfers live |

Acceptance: HTTP parser and limits unit-tested; the API driven end-to-end
from a test (monitor between two simulators, conformance against the own
LIS, capture listing); runs on a Raspberry Pi 3B+ (Debian 13), reachable on
the LAN.

## 9. Sessions (v0.8 to v0.10)

Added 2026-10-25. Request: "pro session eine configdatei … in Webview
sollte sessionsmanagement implementiert werden, nicht pro mode, sondern auch
instanzen im Gleichen mode könnten mehrmals laufen … Jede Session ist ein
Tab … Die Modis sim-gerät und sim-lis sind so was von unterschiedlich, dass
es kein bloßes subparameter sie unterscheidet … Beim sim-LIS sollte man die
Proben und deren results sehen, Tests zu den Proben anlegen/zuordnen, zum
senden auf Anfragen Q – das alles tabellarisch … In webview braucht man die
uris … nicht sehen." Decisions: "gemeinsamer Katalog, Reihenfolge passt".

### 9.1 Terms

- **Session**: a named configuration of one *session type* plus, while it
  runs, its instance. Several sessions of the same type may run at once
  (e.g. five simulated instruments).
- **Session file**: `sessions/<name>.yaml` in the data directory. The web UI
  edits it; `lisbench run sessions/<name>.yaml` runs it without the UI. The
  older subcommands stay for scripts.
- **State file**: `sessions/<name>.state.json`, written by lisbench only:
  order book, results, counters. Kept apart so that the session file stays
  a hand-editable configuration.
- **Test catalogue**: `catalog.yaml` in the data directory, shared by all
  sessions.
- **Link**: one connection of a session, with a *role* (`main`, or `orders`
  and `results` when a LIS uses separate ports for the two directions of
  work).
- **Order item**: one test for one specimen in the LIS simulator's order
  book; it has the state machine of 9.5.
- **Order-result matching**: assigning a received result to an order item
  by specimen ID (O-3) and test code (fourth component of the universal test
  ID, O-5 / R-3). A result without a matching item is an **unsolicited
  result**; what happens to it is a policy of the LIS, not a rule of
  LIS02-A2, which allows unsolicited results.

### 9.2 Runtime

One process, one event loop (asio coroutines), as in v0.7: every session
instance is a group of coroutines; no fork, no threads (trigger commands
excepted). A session instance has the lifecycle `stopped → starting →
running → stopping → stopped`, or `failed` with a reason; each link reports
`waiting` (server or port open, no partner), `connected`, `closed`. Before
start, lisbench refuses a session whose serial port or TCP listen port is
already used by a running session. A `tcp-server` link serves one partner
at a time, as LIS01-A2 is point to point: each instrument gets a port of its
own. Live data is published per session; each
session keeps the last 5000 events in memory and can also save a capture.

### 9.3 Session file

```yaml
lisbench: session/1
name: bench-uno                 # unique, plain file name characters
type: device                    # lis | device | monitor | export | conform
profile: cepheid-genexpert      # optional instrument profile
timing: lis01-a2                # lis01-a2 | fast-test
links:
  - role: main                  # device: main; lis: main, or orders + results
    transport: serial           # serial | tcp-server | tcp-client
    serial: {port: /dev/ttyUSB1, baud: 9600, data_bits: 8, parity: none,
             stop_bits: 1, flow: none}
  # - role: results
  #   transport: tcp-server
  #   tcp: {address: 0.0.0.0, port: 15001}
device: {...}                   # 9.6, only for type device
lis: {...}                      # 9.5, only for type lis
```

Every value is typed and checked when the file is read, with the line of
the error; unknown keys are errors. URIs do not appear: lisbench derives the
endpoint from `transport` and its block.

### 9.4 Test catalogue

```yaml
lisbench: catalog/1
tests:
  - code: GLU                   # 4th component of the universal test ID
    name: Glucose
    kind: numeric               # numeric | qualitative
    unit: mg/dL
    reference: {low: 70, high: 110}
    decimals: 0
    simulate: {low: 60, high: 180}            # device: value range
  - code: FLUA
    name: Influenza A
    kind: qualitative
    values: [DETECTED, NOT DETECTED, INVALID] # device picks from these
    abnormal: [DETECTED]                      # flag A when sent
```

The LIS simulator offers these codes when tests are assigned to a specimen;
the instrument simulator uses `simulate` / `values` to produce results and
derives the abnormal flag (R-7: `L`, `H`, `N`, `A`) from `reference` or
`abnormal`.

### 9.5 LIS simulator

```yaml
lis:
  unsolicited: flag             # accept | flag | ignore
  answer_queries: true          # answer Q from the order book
  no_order_reply: y-record      # y-record (O-26 = Y) | empty (H, L only)
  send_mode: on-demand          # on-demand (button / API) | on-connect
```

Order book: a table of specimens (specimen ID, patient ID, name) with their
order items (test code from the catalogue) and per item the result: value,
unit, flags, result status (R-9), operator, time. O-6 priority goes out as
`R` (routine); the UI does not offer it (2026-10-26). The book is edited in
the UI and kept in the state file; "clear" empties it.

The book also builds itself from what the instrument sends (2026-10-26,
"Bitte die Tabellen … aus den Nachrichten aufbauen. Wenn Gerät es sendet und
sample oder test noch nicht da sind, in die db aufnehmen"): a Q for a
specimen not in the book adds it without items, a result adds specimen and
item as `unsolicited` below, and the patient (P-3, else P-4; P-6) is taken
over for a specimen that has none. Listening for a while and then adding or
removing catalogue tests per specimen gives a scenario without typing codes.

Order item states, named after the LIS02-A2 codes where one exists:

| State | Entered when |
|---|---|
| `new` | created in the UI (a separate `pending` state of the first draft is covered by `sent`/`answered`) |
| `sent` | an O record with the item went out (batch, action code O-12 = `N`); waiting for the instrument |
| `answered` | it went out as the answer to a Q for this specimen; waiting for the instrument |
| `preliminary` | a matching R with status `P` (or `I`, in instrument pending) |
| `final` | a matching R with status `F` |
| `corrected` | a matching R with status `C` after a final one |
| `cannot-be-done` | a matching R with status `X` |
| `cancelled` | the user cancelled it; an O with action code `C` went out |

A result for a `new` item (never sent) counts as unsolicited. With
`unsolicited: accept` the result creates the item in state `final` (or as
reported) marked "unsolicited"; `flag` does the same and marks it "for
review" until the user accepts or discards it; `ignore` only logs it. A Q for a
specimen without items is answered as set in `no_order_reply`.

With separate `orders` and `results` links, Q in and O out use `orders`, R
in uses `results`; with one `main` link both use it.

### 9.6 Instrument simulator

```yaml
device:
  mode: host-query              # unsolicited | host-query | batch
  specimens: {from: counter, prefix: S-, start: 1}   # or a list of IDs
  tests: [GLU]                  # unsolicited: tests to report per specimen
  interval: 30s                 # next specimen after the last one finished
  query_timeout: 60s            # host-query: no answer means no order
  measure_time: 2s              # between order and result
```

| Mode | Per specimen |
|---|---|
| `unsolicited` | send H P O R L with `tests`, values from the catalogue |
| `host-query` | send Q; on an answer with O records measure those tests and send the results; on "no order" or timeout skip it |
| `batch` | accept orders from the LIS at any time into its own list; work through the list |

Per specimen the instrument has `queued → queried → ordered | no-order |
timed-out → measuring → reported | failed`.

### 9.7 Other session types

`monitor` (two links, watches, triggers without `exec`, capture), `conform`
(one link, IUT role, cases; a run ends with a report), `export` (database
file, mapping file, schedule `manual | every <duration>`, target link,
send timeout and retries per message, write-back).

### 9.8 Web UI

- **Home tab**: running sessions (name, type, status, link states, messages
  in/out, last activity, last problem) with start/stop/open; saved sessions;
  **New session**: choose the type, then a typed form.
- **One tab per opened session**, its view given by the type: LIS: order
  book and results tables, add specimens, assign tests from the catalogue,
  send, cancel; instrument: specimen queue with states, live log; monitor:
  as v0.7.
- **Forms are typed**: transport as radio buttons; serial port from the
  list of ports found (with the USB vendor name); baud rate, data bits,
  parity as selections; numbers with ranges; test codes from the catalogue.
  No free text where a choice exists, no URIs.
- **Catalogue tab**: edit the shared test catalogue.
- **Export editor** (v0.10): open a database from the data directory, see
  tables and columns, assign columns or constants to record fields (field
  names from the LIS02-A2 tables), nested queries per level, preview with
  real rows, write-back statements; saved as a mapping file of section 5.

### 9.9 Releases

| Release | Content | Acceptance |
|---|---|---|
| v0.8 (done) | session files and state files, catalogue, session manager (several instances, port conflicts), home tab, LIS simulator (order book, item states, matching, unsolicited policy, queries), instrument simulator (`unsolicited`, `host-query`), `lisbench run`; Uno sketch with a host-query mode | unit tests of file checking, item states and matching; end to end: two instrument sessions and one LIS session on one loop, host query through to `final`; unsolicited result handled per policy; on the Uno: Q → O → R |
| v0.9 | separate orders/results links, `batch` mode, monitor and conform as session types | LIS with two ports against an instrument with two links |
| v0.10 | export sessions with the mapping editor and preview | a mapping built in the editor gives the same message as the hand-written file |

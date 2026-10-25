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
| v0.7 | web UI served by the daemon; package for Raspberry Pi 3B+ (Debian 13) | runs on a Pi 3B+, UI reachable on the LAN |
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

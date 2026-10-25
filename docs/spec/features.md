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
| v0.4 | `bridge`: serial↔TCP, serial→file, TCP→file, role swap, tap mode | loss-free under the lossy-wire test |
| v0.5 | `mapper`: SQLite → messages by declarative YAML (nested queries with own scopes, `${row.x}`/`${cfg.x}` via lexer, bound parameters, `skip_if_empty`, write-back with bound parameters, stable message control ID) | mapper acceptance list, §5 |
| v0.6 | web UI served by the daemon; package for Raspberry Pi 3B+ (Debian 13) | runs on a Pi 3B+, UI reachable on the LAN |
| v0.7 | microcontroller instrument simulators: Arduino sketch (serial, AVR) and ESP32 (Wi-Fi/TCP) playing an analyzer as LIS client | sketch talks to the PC monitor for 24 h without error |
| later | Wireshark Lua dissector; triggers that run actions on a watch hit; RP2040 passive RS-232 tap; HL7 v2 over MLLP (IHE LAW) as own module | |

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

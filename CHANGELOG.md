# Changelog

## 0.8.0 – 2026-10-26

Sessions (docs/spec/features.md section 9).

- Session files `<data>/sessions/<name>.yaml`: typed, checked with line
  numbers, unknown keys are errors, no URIs (transport serial, tcp-server or
  tcp-client with its own fields). State in `<name>.state.json`.
- Shared test catalogue `<data>/catalog.yaml`.
- LIS simulator: order book with specimens and order items, item states
  after the LIS02-A2 codes (new, sent, answered, preliminary, final,
  corrected, cannot-be-done, cancelled), batch orders and cancellation,
  query answers (O-26 = Q, or Y / header only when there is no order),
  order-result matching, unsolicited results accepted, kept for review or
  ignored. L-3 termination codes N, F, I.
- Instrument simulator: unsolicited and host-query modes, specimens from a
  counter or a list, results from the catalogue (stable per specimen).
- Several sessions on one event loop; port conflicts refused; links
  reconnect; `lisbench run <data>/sessions/*.yaml` without the UI.
- Web UI: home tab with all sessions live, a typed editor (serial ports
  from this computer with their USB names, choices instead of free text),
  one tab per session (kept across reloads), order book and results,
  catalogue editor. The v0.7 views moved under Tools.
- Uno sketch: host-query mode (`LISDEV_HOST_QUERY=1`). On the board against
  a LIS session: Q, O, R through to final; a specimen without an order is
  skipped.
- Fixed while testing: a serial link closed 30 ms after it opened (an `||`
  nested in another `||`); hidden form parts were shown (`display` beat
  `hidden`).
- `conform`: before the first case the tester waits until the IUT answers
  at all (a board that resets when its port is opened is deaf for a second
  or two); an unanswered ENQ is ended with EOT and tried again; TR06 counts
  only ACK, NAK and EOT as replies. Found on the Uno: TR01/TR02 were
  inconclusive and TR06 failed with the boot time; now 14 of 14 pass, twice.
- ESP32 sketch: Wi-Fi status every 2 s while not connected (network not
  found, password, ...), address and signal once connected; optional
  `WIFI_TX_POWER` for small ESP32-C3 boards.
- Spec section 9: sessions (session files, shared test catalogue, LIS and
  instrument simulators with order book and order-result matching).

## 0.7.1 – 2026-10-25

- First release with the web UI of 0.7.0 (see below).
- Debian packages for Raspberry Pi (arm64, armhf) and PC (amd64), built on
  Debian 13 in CI; ARM builds cross-compiled with Debian's GCC 14, their
  tests run under qemu. `/usr/bin/lisbench`, profiles in
  `/usr/share/lisbench/devices`, examples, and a hardened systemd service
  for the web UI (own user, group dialout, `/var/lib/lisbench`), not
  enabled on install; settings in `/etc/default/lisbench`.
- Releases are created by CI on a version tag, with the packages attached.
- `LISBENCH_DEVICES_DIR` (CMake cache variable): where `--profile` looks up
  profiles by id.
- Tests: MSVC mangled a raw string literal with `\"` inside a Catch2 macro;
  the web test keeps it in a variable.

## 0.7.0 – 2026-10-25 (not tagged; released with 0.7.1)

- `lisbench serve [--http 127.0.0.1:8080] [--captures DIR]`: web UI with
  four views. Monitor: endpoints, watches, profile, save; live table with
  record fields on click and watch hits marked. Captures: list and replay.
  Conformance: run the suite, live progress, verdict table, JSON download.
  Simulator: role, endpoint, message files, query answers, faults.
- Own small HTTP/1.1 server on the existing asio (decision of the
  requester): commands as POST with JSON, live data as Server-Sent Events,
  pages compiled into the binary (one file for the Pi). Limits on head, body
  and event streams; a slow event reader is dropped. Listens on 127.0.0.1
  by default and warns when it does not. Files only by plain name from the
  data directory; no command triggers from the browser.
- Watch expressions: `<`, `>`, `<=`, `>=` against a number no longer match a
  value that is not a number (`R.value > 100` hit "DETECTED^" as text; found
  in the web UI with GeneXpert-style values). Use `R.value.2` for a number in
  a component.
- `io::parse_fault` (the `on:which:do[:ms]` form) moved from the command
  line into the library, shared by `sim` and the web UI.

## 0.6.6 – 2026-10-25

- Receivers refuse a frame with a restricted character in its text (SOH to
  ACK, LF, DLE to ETB) with NAK, as LIS01-A2 expects: `lis01::Link` (used
  by `sim`, `map` and `conform`'s reference IUTs) and `devsim/LisDevice`. Found by `lisbench conform`
  TR07, which lisbench itself had passed only with a note. An instrument
  that puts LF into frames is now refused; tell us if one needs a tolerance.
- Monitor/decoder: such a frame is shown with a note, not hidden.
- `conform` TS01 counts restricted characters in the IUT's frames.

## 0.6.5 – 2026-10-25

- `lisbench conform --iut computer|instrument --endpoint URI`: LIS01-A2
  conformance test suite with 14 test cases (receiver: checksum, frame
  number, repeated frame, long messages, reply time, restricted characters;
  sender: frame format, NAK repetition, six attempts, reply timeout, busy
  wait, contention, receiver interrupt). Verdicts pass / fail / inconclusive
  after ISO/IEC 9646; a "should" gives a note, not a fail. `--profile` acts
  as PICS (receiver interrupt, contention rule), `--fast-timing` for devices
  built with the test preset, `--json` for the results; exit code 1 when a
  case fails.
- Profiles: `lis01.honours_receiver_interrupt`.

## 0.6.4 – 2026-10-25

- `--profile <id or path>` for `decode`, `validate`, `monitor` and `sim`:
  charset, non-ASCII mode, local escape, frame length and contention rule
  come from `docs/devices/<id>/profile.yaml` (search path `$LISBENCH_DEVICES`,
  then the source tree). Options given on the command line win.

## 0.6.3 – 2026-10-25 (not tagged; released with 0.6.4)

- `monitor --trigger "WATCH => ACTION"`: `exec CMD` runs a command with the
  hit in `LISBENCH_WATCH`, `LISBENCH_VALUE`, `LISBENCH_RECORD`,
  `LISBENCH_FROM`, `LISBENCH_TIME` (never pasted into the command line);
  commands run in order on a worker thread with a bounded queue. `mark TEXT`
  writes a mark line into the capture (`--save`), shown on replay. `stop`
  ends the monitor. Works on `--replay` as well.
- Watch expressions: a value that starts with `<`, `>`, `=` or `!` must be
  quoted (`R.value >> 100` was silently a text comparison).
- Fast test timing: `lisbench sim --fast-timing` and the Uno sketch built
  with `LISDEV_FAST_TEST=1` use 1 s pauses and short timeouts (2 s reply,
  4 s receive, 1 s busy). On the board: 15 messages in 20 s instead of one
  per 30 s; NAK, ENQ-NAK and late-ACK cases in seconds. Not LIS01-A2.
- Uno sketch: the pause counts from the end of the last transfer, not from
  its start (with millis() read after the transfer ended; otherwise the
  unsigned difference wrapped and the pause was skipped).
- Serial ports: on opening, bytes are read and dropped until the line has
  been quiet for 30 ms (at most 2 s). An Arduino left running without a
  listener had queued several kB of ENQ/EOT pairs; the USB adapter delivers
  them at once, even after a flush, and they were answered with a burst of
  stale ACKs.

## 0.6.2 – 2026-10-25

First run on hardware: Uno-compatible board (ATmega328P, FTDI) with the
`uno_analyzer` sketch against `lisbench sim --role computer` at 9600 baud.
Normal transfers, NAK on frames (repeated, duplicates dropped), NAK on the
ENQ (retry after 10 s) and a late ACK (sender aborts after 15 s, resends the
same sample) all behaved as LIS01-A2 requires. Found and fixed:

- Link: an ENQ arriving before the first frame of a transfer is answered
  with ACK again (the sender missed our ACK). Before, it was dropped as
  unexpected and the transfer failed after 15 s.
- `sim`: a transfer ended by EOT without L record is shown as INCOMPLETE,
  an empty one as `empty, no frames`.
- `sim --trace`: received bytes are grouped into one line per frame or
  control character; at 9600 baud they arrived as one line per byte.

## 0.6.1 – 2026-10-25

- First instrument profile: `docs/devices/cepheid-genexpert/` (README,
  profile.yaml, own example messages, LIS and instrument scenarios), written
  from the public interface specification; not yet checked on an instrument.
- Codec: `local_escape: utf16-hex` / `--local-escape utf16-hex` reads the
  local escape `Z` as UTF-16 code units in hex and writes characters outside
  the code page that way. In strict mode `Z` with the default `keep` is no
  longer an "unknown escape".
- Link: `Timing::contention_winner` (scenario `timing: {contention_winner:
  computer}`) for instruments that yield to the LIS on contention.
- Monitor: ENQ, ACK, EOT without frames is reported as `empty transfer, no
  frames` instead of an incomplete transfer.
- Tests: examples and scenarios under `docs/devices` are parsed, validated
  and loaded in CI.

## 0.6.0 – 2026-10-25

- `devsim/LisDevice`: Arduino library, instrument side of LIS01-A2 in one
  C++11 header without heap; sketches for Arduino Uno (serial) and ESP32
  (Wi-Fi/TCP). Tested on the PC against the lisbench link; CI compiles both
  sketches.
- Roadmap: web UI moved to 0.7.

## 0.5.0 – 2026-10-25

- `lisbench map`: messages from an SQLite database through a declarative
  YAML mapping; nested queries with named scopes, placeholders resolved by a
  lexer, bound parameters only, `skip_if_empty`, write-back (`on_success`,
  `on_failure`), stable message control ID, local time stamps; output to
  stdout, TCP/serial (with the simulator's link) or files (temp + rename).
- `--version` comes from the CMake project version.

## 0.4.0 – 2026-10-25

- `lisbench bridge`: join two endpoints byte-transparently (serial <-> TCP,
  client <-> server), optional capture, reconnect and accept again with `--serve`.
- `lisbench record`: write what one endpoint sends to a capture (replayable
  with `monitor --replay`) and/or a raw file.
- clang-tidy: `readability-qualified-auto` off (its fix breaks MSVC builds).

## 0.3.0 – 2026-10-25

- `lisbench monitor`: relay between two endpoints (or passive tap) with live
  decoding of both directions, statistics, slow replies, transfers without L,
  watch expressions on fields, capture files (JSON lines) and replay.
- Core: `lis01::StreamDecoder` (passive decoder; `deframe` now uses it),
  `lis02::parse_record`, watch expressions.

## 0.2.0 – 2026-10-25

- `lisbench sim`: instrument or LIS simulator over TCP (client/server) and
  serial ports; YAML scenarios (send, expect, wait), automatic query answers,
  fault injection (refuse, corrupt, drop, delay).
- asio-based session driving the protocol core; examples in `examples/`.
- `printable` shows `<` as it is.

## 0.1.0 – 2026-10-25

First release: LIS01-A2 framing and link state machine, LIS02-A2 records,
codec, validation and builder; command line tool `frame`, `deframe`, `decode`,
`validate`.

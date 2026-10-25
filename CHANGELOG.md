# Changelog

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

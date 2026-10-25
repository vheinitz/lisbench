# Changelog

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
- Monitor: ENQ, ACK, EOT without frames is reported as `empty transfer (link
  check)` instead of an incomplete transfer.
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

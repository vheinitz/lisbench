# Changelog

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

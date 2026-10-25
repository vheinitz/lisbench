# Changelog

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

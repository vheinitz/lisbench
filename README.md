# lisbench

Tools for developing and testing laboratory instrument interfaces that speak
**CLSI LIS01-A2** (formerly ASTM E1381, the low-level protocol with ENQ/ACK,
frames and checksums) and **CLSI LIS02-A2** (formerly ASTM E1394, the record
format H/P/O/R/C/Q/L).

lisbench is a workbench for the people who build and integrate these
interfaces. It is not middleware for clinical operation and must not be used
to route patient results in production.

Status: **v0.5, protocol core, command line tool, simulator, monitor,
bridges and database mapper.** Web UI and microcontroller simulators are
planned; see [docs/spec/features.md](docs/spec/features.md).
A spare-time project.

## What works now

- LIS01-A2 framing: split records into frames of at most 240 text bytes,
  frame numbers modulo 8, checksum, ETB/ETX; decoding with exact error reasons.
- LIS01-A2 link state machine without I/O: establishment, contention
  (instrument has priority), busy NAK, retransmission (six attempts), timeouts
  (15 s / 30 s), receiver interrupt (EOT instead of ACK), disconnect, cancel.
  Every submitted message gets exactly one result, on every path.
- LIS02-A2 records: delimiters from the header, fields/repeats/components,
  escapes `&F& &S& &R& &E& &Xhh&`, all field names of H, P, O, R, C, Q, L, S
  in one table.
- Character sets: Latin-1 or UTF-8 on the wire; non-ASCII as `&Xhh&` (7-bit),
  raw 8-bit, or transliterated to ASCII with a report of dropped characters.
- Validation of record order, hierarchy and sequence numbers; a builder that
  refuses records that do not fit instead of dropping them.
- Reassembly of records from a raw one-direction capture.
- Simulator for an instrument or a LIS over TCP (client or server) or a
  serial port: scenarios in YAML (send, expect, wait), automatic answers to
  queries, and fault injection on the wire (refuse, corrupt, drop or delay
  ACK, NAK, ENQ, EOT or frames). See [examples/](examples/).
- Monitor between instrument and LIS (TCP or serial, or a passive tap on two
  receive lines): decodes both directions, flags bad and repeated frames,
  NAKs, slow replies and transfers without L record, evaluates watch
  expressions on field values (`R.value > 100`, `O.universal_test_id.4 ==
  TSH`), saves the raw traffic and replays it offline.
- Bridges without decoding: serial ↔ TCP, TCP client ↔ server (role swap),
  and recording of one endpoint to a capture or raw file.
- Mapper from an SQLite database to messages, described in YAML: nested
  queries with their own scopes, bound parameters, escaping of every value,
  records that do not fit the hierarchy are errors, write-back after
  sending, output to TCP, serial or files. See [examples/mapper/](examples/mapper/).

## Build

Requires CMake ≥ 3.25 and a C++23 compiler (GCC ≥ 13, Clang ≥ 19 or MSVC 2022).
Dependencies (asio, yaml-cpp, SQLite, CLI11, nlohmann/json, Catch2) are fetched at
configure time.

```console
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Command line

```console
lisbench frame   FILE [--raw]       records (one per line) -> frames ENQ ... EOT
lisbench deframe FILE               raw capture -> records, checks every frame
lisbench decode  FILE [--json]      records or capture -> fields by name
lisbench validate FILE              order, hierarchy, sequence numbers
lisbench sim SCENARIO.yaml          run a simulator scenario
lisbench sim --endpoint URI ...     the same from options (--send, --receive,
                                    --answer-queries, --fault, --trace)
lisbench monitor --a URI --b URI    pass traffic through and show it decoded
                                    (--watch, --save, --decode, --verbose, --tap)
lisbench monitor --replay FILE      the same for a saved capture
lisbench bridge --a URI --b URI     join two endpoints, bytes unchanged (--serve, --save)
lisbench record --from URI --to F   write what an endpoint sends to a file (--raw)
lisbench map MAPPING.yaml --db DB   messages from a database (--each, --arg, --to)
```

Endpoints: `tcp://host:port`, `tcp-listen://:port`,
`serial:/dev/ttyUSB0?baud=9600&data=8&parity=none&stop=1` (`serial:COM3?...` on Windows).

`decode` and `validate` take `--charset latin1|utf8`, `--non-ascii
ascii-hex|raw|transliterate` and `--strict`. Example:

```console
$ lisbench frame tests/corpus/valid_host_query.astm
<ENQ>
<STX>1H|\^&|||lisbench-sim|||||LIS||P|LIS2-A2|20261025120500<CR><ETX>1F<CR><LF>
<STX>2Q|1|^S-1002||ALL||||||||O<CR><ETX>5D<CR><LF>
<STX>3L|1|N<CR><ETX>06<CR><LF>
<EOT>
```

## Tests

Unit tests, a property test over all record lengths 0–2000, a lossy-wire
simulation (drops and bit errors, 200 seeds) for the link, an exhaustive
character test U+0000–U+00FF in every mode, randomised round-trip tests,
simulator-against-simulator tests over TCP and over a pseudo-terminal null
modem with injected faults, and libFuzzer targets in `fuzz/`. CI builds with GCC 13/14, Clang 19 and MSVC,
runs ASan/UBSan, clang-tidy and the fuzzers. Details in
[docs/spec/tests.md](docs/spec/tests.md).

## Related projects

[python-astm](https://github.com/kxepal/python-astm) (Python library),
[OpenELIS analyzer bridge](https://github.com/DIGI-UW/openelis-analyzer-bridge)
(delivery into OpenELIS), [ser2net](https://github.com/cminyard/ser2net)
(serial over TCP), Mirth Connect / Open Integration Engine (interface engines).

## Licence

MIT, see [LICENSE](LICENSE). The CLSI standards are not part of this
repository; they are available from CLSI.

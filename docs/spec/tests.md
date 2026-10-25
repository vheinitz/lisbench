# lisbench – test concept

Written 2026-10-25 by Claude Code for Valentin Heinitz (request: "Dann
features, dann tests …").

## 1. Principles

- Every protocol rule has a test that fails when the rule is broken.
- No sleeping: the link is a state machine without I/O; tests drive it with a
  simulated clock and deliver timer expiries explicitly.
- Test data is synthetic or from public manuals with source; never captures
  from customer sites.
- Invariants are checked twice: in randomised Catch2 tests (every build) and
  in libFuzzer targets (CI, 60 s each).

## 2. Core (v0.1)

| Area | Test | File |
|---|---|---|
| Checksum | values computed outside the code base | test_lis01_frame.cpp |
| Framing | property over record lengths 0–2000: texts concatenate to the input, ETX only on the last frame, ≤ 247 bytes, checksums verify, numbers modulo 8 | test_lis01_frame.cpp |
| Framing | 240-byte record is one ETX frame; wrap 7→0; malformed frames give the exact error | test_lis01_frame.cpp |
| Capture | deframe drops retransmissions, reports corruption | test_lis01_frame.cpp |
| Link | delivery, multi-frame records, queue order, contention, busy NAK, six attempts then EOT, reply timeout then EOT, stale timers, interrupt request, disconnect/resume, cancel, receiver NAK on checksum and frame number, duplicate frame, receive timeout | test_lis01_link.cpp |
| Link | lossy wire, 200 seeds: drops and bit errors in both directions; each message reported exactly once, nothing corrupted accepted, both sides end neutral | test_lis01_link.cpp |
| Codec | every code point U+0000–U+00FF in every mode; hex escapes carry bytes; outside Latin-1; strict vs. lenient escapes; invalid UTF-8 | test_lis02_codec.cpp |
| Records | split, serialize, trailing empty fields, type not assignable, field table | test_lis02_message.cpp |
| Validation | hierarchy, sequence per parent, missing L, record before H, multi-message transfer | test_lis02_message.cpp |
| Builder | numbering, refusal of misplaced records | test_lis02_message.cpp |
| Corpus | valid files validate, invalid files are caught, parse/serialize and frame/deframe round trips | test_corpus.cpp |
| Robustness | randomised parse→serialize→parse, deframe→frame→deframe, garbage into a sending link | test_robustness.cpp |

Limit of the standard, documented in the lossy-wire test: two bit errors in
one frame can cancel out in the modulo-256 checksum. The test injects at most
one error per frame.

## 3. Simulator (v0.2)

| Area | Test | File |
|---|---|---|
| Endpoints | URI parsing incl. serial options, error cases, printing | test_io.cpp |
| Faults | nth/every selection, ACK→NAK, corrupted frame fails only its checksum, delay | test_io.cpp |
| Scenarios | full YAML scenario, durations, errors with line numbers | test_scenario.cpp |
| End to end | instrument ↔ LIS over TCP: upload, query answered, refused and corrupted frames repeated, dropped ACK → timeout, six NAKs → rejected | test_sim_e2e.cpp |
| End to end | instrument ↔ LIS over two serial ports joined by pseudo terminals (Linux) | test_serial_e2e.cpp |

## 4. Monitor (v0.3)

| Area | Test | File |
|---|---|---|
| Decoder | handshake, frames, records, noise in wire order, byte by byte; retransmission, bad frame, EOT inside a record, unfinished frame | test_lis01_decoder.cpp |
| Watches | parsing and errors; numeric vs. text comparison, components, whole fields, regex, contains, other record types | test_watch.cpp |
| Monitor | normal transfer with reply times and a watch hit, visibility per verbosity; aborted transfer without L, NAK, slow reply; capture line round trip | test_monitor.cpp |
| End to end | instrument sim → relay with monitor → LIS sim over TCP, a corrupted frame on the way; statistics on both sides | test_monitor_e2e.cpp |

Checked by hand for this release: a live session and its replay from the
saved capture give identical output.

## 5. Later releases
- v0.3: replay of recorded sessions into the monitor gives byte-identical
  decoded output (golden files); watch expressions have a parser test suite.
- v0.5: the mapper acceptance list in features.md §5, each item one test,
  against an in-memory SQLite database.
- v0.7: hardware-in-the-loop job on a self-hosted runner: Arduino/ESP32
  simulator ↔ monitor on a Raspberry Pi.

## 7. Tooling in CI

GCC 13 and 14, Clang 19, MSVC 2022 with warnings as errors; ASan + UBSan;
clang-tidy 19 (`bugprone`, `cppcoreguidelines`, `modernize`, `performance`,
`readability`, exceptions listed in `.clang-tidy`); clang-format check;
three libFuzzer targets.

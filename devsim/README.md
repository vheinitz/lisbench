# devsim – instruments on microcontrollers

`LisDevice/` is an Arduino library: the instrument side of CLSI LIS01-A2 in
one header, C++11, no heap, no standard library. It sends a message
(records come from a callback and are framed on the fly), repeats refused
frames up to six times, times out after 15 s, gives way on contention only
as far as the standard says (the instrument has priority), and accepts
transfers from the LIS. The same header is tested on the PC against the
lisbench link (`tests/test_devsim.cpp`).

Examples:

| Sketch | Board | Link | Sends |
|---|---|---|---|
| `uno_analyzer` | Arduino Uno | USB serial, 9600 8N1 | glucose result 30 s after the last transfer, or on pin 2 |
| `esp32_analyzer` | ESP32 | Wi-Fi, TCP client | TSH result every 30 s; prints what the LIS sends |

```console
arduino-cli compile --fqbn arduino:avr:uno --library devsim/LisDevice devsim/LisDevice/examples/uno_analyzer
arduino-cli upload  --fqbn arduino:avr:uno -p /dev/ttyACM0 devsim/LisDevice/examples/uno_analyzer
lisbench sim --role computer --endpoint "serial:/dev/ttyACM0?baud=9600"
```

Host-query mode: build with `-DLISDEV_HOST_QUERY=1` (or set it at the top
of the sketch). For each specimen the Uno sends a query (Q), measures the
tests the LIS orders and reports them; a specimen without an order, or
without an answer in time, is skipped. Run it against a LIS session of
`lisbench serve` with an order book, e.g. specimens S-00001 and S-00003.

For quick tests build the Uno sketch with short timing and run lisbench
with the same preset: 1 s pause between messages, 2 s reply timeout, 4 s
receive timeout, 1 s busy retry. Not LIS01-A2, so never against a real LIS.

```console
arduino-cli compile --fqbn arduino:avr:uno --library devsim/LisDevice \
  --build-property "compiler.cpp.extra_flags=-DLISDEV_FAST_TEST=1" devsim/LisDevice/examples/uno_analyzer
lisbench sim --role computer --endpoint "serial:/dev/ttyUSB0?baud=9600" --fast-timing --fault ack:3:delay:3000
```

(Arduino IDE: set `LISDEV_FAST_TEST` to 1 at the top of the sketch.) A
fault case then takes seconds: NAK on the ENQ is retried after 1 s, a late
ACK makes the sender give up after 2 s.

Boards with an FTDI or CH340 USB chip (older Uno clones, Nano, Duemilanove)
appear as `/dev/ttyUSB0` instead. Another program holding the port (a
terminal, a serial plotter) makes uploads fail or hang; `fuser -v
/dev/ttyUSB0` shows who has it open.

For the ESP32 copy `config.example.h` to `config.h` and enter Wi-Fi and LIS
address (config.h is not committed), then run
`lisbench sim --role computer --endpoint tcp-listen://:15000 --serve`.

Limits: records up to 256 bytes (`LISDEV_RECORD_MAX`); a longer record makes
`start()` refuse the message instead of cutting it. Tested on the PC and
compiled for both boards in CI. `uno_analyzer` ran on an Uno-compatible
board (ATmega328P with FTDI USB) on 2026-10-25 against `lisbench sim`:
normal transfers, injected NAKs on frames and on the ENQ, and a late ACK
(abort after 15 s, same sample sent again) behaved as LIS01-A2 requires.
The ESP32 sketch has not run on hardware yet.

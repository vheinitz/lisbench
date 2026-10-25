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
| `uno_analyzer` | Arduino Uno | USB serial, 9600 8N1 | glucose result every 30 s or on pin 2 |
| `esp32_analyzer` | ESP32 | Wi-Fi, TCP client | TSH result every 30 s; prints what the LIS sends |

```console
arduino-cli compile --fqbn arduino:avr:uno --library devsim/LisDevice devsim/LisDevice/examples/uno_analyzer
arduino-cli upload  --fqbn arduino:avr:uno -p /dev/ttyACM0 devsim/LisDevice/examples/uno_analyzer
lisbench sim --role computer --endpoint "serial:/dev/ttyACM0?baud=9600"
```

For the ESP32 copy `config.example.h` to `config.h` and enter Wi-Fi and LIS
address (config.h is not committed), then run
`lisbench sim --role computer --endpoint tcp-listen://:15000 --serve`.

Limits: records up to 256 bytes (`LISDEV_RECORD_MAX`); a longer record makes
`start()` refuse the message instead of cutting it. Tested on the PC and
compiled for both boards in CI; not yet run on hardware by the author of
this file.

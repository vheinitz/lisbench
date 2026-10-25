// An analyzer on an Arduino Uno using CLSI LIS01-A2 / LIS02-A2, in one of two
// modes (docs/spec/features.md, 9.6):
//
//   unsolicited (default)  sends a glucose result for specimen S-00001, S-00002, ...
//   host query             asks the LIS for each specimen (Q), measures the tests
//                          the LIS orders, and reports them; a specimen without
//                          an order, or without an answer in time, is skipped
//
// The next specimen comes 30 seconds after the previous transfer ended (or
// when pin 2 is pulled to GND). The Uno's USB serial port is the instrument
// interface; let lisbench play the LIS:
//
//   lisbench sim --role computer --endpoint "serial:/dev/ttyACM0?baud=9600"
//   or a LIS session in lisbench serve (with an order book for host query)
//
// Build options (set below, or pass -D...=1):
//   LISDEV_HOST_QUERY  1: host query mode
//   LISDEV_FAST_TEST   1: 1 s pause and short timeouts (2 s reply, 4 s receive,
//                      1 s busy); lisbench with --fast-timing / timing fast-test.
//                      Not LIS01-A2: use the normal build against a real LIS.
//
// The LED blinks once for every delivered message, three times on failure.
// All values are invented.

#include <LisDevice.h>

#ifndef LISDEV_FAST_TEST
#define LISDEV_FAST_TEST 0
#endif
#ifndef LISDEV_HOST_QUERY
#define LISDEV_HOST_QUERY 0
#endif

namespace {

const uint8_t button_pin = 2;
const uint8_t led_pin = LED_BUILTIN;

const unsigned long pause_ms = LISDEV_FAST_TEST ? 1000 : 30000;     // after the end of a transfer
const unsigned long answer_ms = LISDEV_FAST_TEST ? 5000 : 60000;    // host query: wait for the LIS
const unsigned long measure_ms = LISDEV_FAST_TEST ? 1000 : 2000;    // host query: "measuring"

lisdev::Timing make_timing() {
    lisdev::Timing t;  // LIS01-A2 values
#if LISDEV_FAST_TEST
    // Same values as lisbench --fast-timing.
    t.reply_timeout_ms = 2000;
    t.receive_timeout_ms = 4000;
    t.busy_retry_ms = 1000;
    t.contention_ms = 200;
#endif
    return t;
}

const char header[] PROGMEM = "H|\\^&|||uno-analyzer^0.8|||||LIS||P|LIS2-A2";
const char terminator[] PROGMEM = "L|1|N";

// What the device is doing with the current specimen.
enum Phase { pausing, querying, awaiting, measuring, reporting };
Phase phase = pausing;

const uint8_t max_tests = 4;
const uint8_t max_code = 8;
char tests[max_tests][max_code + 1];
uint8_t test_count = 0;
bool answer_complete = false;

char rx_record[96];  // the record being received, cut if longer (only O and L matter)
uint8_t rx_len = 0;

unsigned int sample_no = 1;
unsigned long phase_since = 0;
unsigned long led_off_at = 0;
uint8_t blinks_left = 0;

size_t copy_P(char* buf, size_t cap, const char* text) {
    strncpy_P(buf, text, cap);
    return strlen_P(text);
}

// A value that stays the same for every call of the same message (frames may be repeated).
int value_for(uint8_t index) {
    return 60 + static_cast<int>((sample_no * 37U + index * 11U) % 120U);
}

size_t result_record(uint8_t i, char* buf, size_t cap) {
    const int v = value_for(i);
    if (strcmp(tests[i], "GLU") == 0) {
        return static_cast<size_t>(snprintf(buf, cap, "R|1|^^^GLU|%d|mg/dL|70-110|%s||F", v,
                                            v > 110 ? "H" : (v < 70 ? "L" : "N")));
    }
    return static_cast<size_t>(snprintf(buf, cap, "R|1|^^^%s|%d|||||F", tests[i], v % 100));
}

size_t make_record(void*, size_t index, char* buf, size_t cap) {
    if (index == 0) {
        return copy_P(buf, cap, header);
    }
    if (phase == querying) {
        if (index == 1) {
            return static_cast<size_t>(snprintf(buf, cap, "Q|1|^S-%05u||ALL||||||||O", sample_no));
        }
        return copy_P(buf, cap, terminator);
    }
    // Results: P, then O and R per test, then L.
    if (index == 1) {
        return static_cast<size_t>(snprintf(buf, cap, "P|1||PID-%04u", sample_no));
    }
    const size_t k = index - 2;
    if (k / 2 < test_count) {
        const uint8_t t = static_cast<uint8_t>(k / 2);
        if (k % 2 == 0) {
            return static_cast<size_t>(snprintf(buf, cap, "O|%u|S-%05u||^^^%s|R||||||||||Serum||||||||||F",
                                                t + 1, sample_no, tests[t]));
        }
        return result_record(t, buf, cap);
    }
    return copy_P(buf, cap, terminator);
}

size_t record_count(void*) {
    return phase == querying ? 3 : 3 + 2 * static_cast<size_t>(test_count);
}

void write_bytes(void*, const uint8_t* data, size_t n) { Serial.write(data, n); }

void blink(uint8_t times) {
    blinks_left = static_cast<uint8_t>(times * 2);
    led_off_at = millis();
}

// Field `n` (1-based) of a record with the default delimiters, and its
// component `c` (1-based; 0: the whole field). Empty if absent.
void field_of(const char* record, uint8_t n, uint8_t c, char* out, size_t cap) {
    out[0] = '\0';
    const char* p = record;
    for (uint8_t f = 1; f < n; ++f) {
        p = strchr(p, '|');
        if (p == nullptr) {
            return;
        }
        ++p;
    }
    for (uint8_t k = 1; c > 1 && k < c; ++k) {
        const char* next = strpbrk(p, "^|");
        if (next == nullptr || *next == '|') {
            return;
        }
        p = next + 1;
    }
    size_t len = strcspn(p, c > 0 ? "^|" : "|");
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
}

// A complete record from the LIS while waiting for its answer.
void take_record(const char* record) {
    if (record[0] == 'L') {
        answer_complete = true;
        return;
    }
    if (record[0] != 'O' || test_count >= max_tests) {
        return;
    }
    char specimen[16];
    char wanted[16];
    char report[4];
    char action[4];
    char test[max_code + 1];
    field_of(record, 3, 0, specimen, sizeof specimen);
    snprintf(wanted, sizeof wanted, "S-%05u", sample_no);
    field_of(record, 26, 0, report, sizeof report);
    field_of(record, 12, 0, action, sizeof action);
    field_of(record, 5, 4, test, sizeof test);
    // Y / Z: no order on record; C: a cancellation, not work to do.
    if (strcmp(specimen, wanted) != 0 || report[0] == 'Y' || report[0] == 'Z' || action[0] == 'C' ||
        test[0] == '\0') {
        return;
    }
    for (uint8_t i = 0; i < test_count; ++i) {
        if (strcmp(tests[i], test) == 0) {
            return;
        }
    }
    strcpy(tests[test_count++], test);
}

void received(void*, const char* text, size_t n, bool record_end) {
    for (size_t i = 0; i < n && rx_len < sizeof rx_record - 1; ++i) {
        rx_record[rx_len++] = text[i];
    }
    if (record_end) {
        rx_record[rx_len] = '\0';
        if (phase == awaiting) {
            take_record(rx_record);
        }
        rx_len = 0;
    }
}

void next_specimen() {
    ++sample_no;
    phase = pausing;
    phase_since = millis();
}

void finished(void*, lisdev::Outcome outcome) {
    if (outcome != lisdev::delivered) {
        blink(3);
        phase = pausing;  // the same specimen again after the pause
        phase_since = millis();
        return;
    }
    blink(1);
    if (phase == querying) {
        phase = awaiting;
        test_count = 0;
        answer_complete = false;
        phase_since = millis();
        return;
    }
    next_specimen();
}

lisdev::Callbacks callbacks = {nullptr, write_bytes, record_count, make_record, received, finished};
lisdev::Device device(callbacks, make_timing());

}  // namespace

void setup() {
    Serial.begin(9600);
    pinMode(button_pin, INPUT_PULLUP);
    pinMode(led_pin, OUTPUT);
}

void loop() {
    const unsigned long now = millis();
    while (Serial.available() > 0) {
        device.on_byte(static_cast<uint8_t>(Serial.read()), now);
    }
    device.poll(now);

    // millis() again: a callback may have set phase_since after `now` was taken.
    const unsigned long in_phase = millis() - phase_since;
    switch (phase) {
        case pausing:
            if ((in_phase >= pause_ms || digitalRead(button_pin) == LOW) && device.state() == lisdev::idle) {
#if LISDEV_HOST_QUERY
                phase = querying;
#else
                strcpy(tests[0], "GLU");
                test_count = 1;
                phase = reporting;
#endif
                if (!device.start(now)) {
                    phase = pausing;
                }
            }
            break;
        case awaiting:
            if (answer_complete && device.state() == lisdev::idle) {
                if (test_count == 0) {
                    next_specimen();  // no order: skip it
                } else {
                    phase = measuring;
                    phase_since = millis();
                }
            } else if (in_phase >= answer_ms && device.state() == lisdev::idle) {
                next_specimen();  // no answer in time: skip it
            }
            break;
        case measuring:
            if (in_phase >= measure_ms && device.state() == lisdev::idle) {
                phase = reporting;
                if (!device.start(now)) {
                    phase = measuring;
                }
            }
            break;
        case querying:
        case reporting:
            break;  // finished() moves on
    }

    if (blinks_left > 0 && now - led_off_at >= 150) {
        digitalWrite(led_pin, (blinks_left % 2) == 0 ? HIGH : LOW);
        --blinks_left;
        led_off_at = now;
    }
}

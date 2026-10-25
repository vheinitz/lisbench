// An analyzer on an Arduino Uno that sends a glucose result 30 seconds after
// the previous transfer ended (or when pin 2 is pulled to GND) using CLSI
// LIS01-A2 / LIS02-A2.
//
// The Uno's USB serial port is the instrument interface. Plug it into the PC
// and let lisbench play the LIS:
//
//   lisbench sim --role computer --endpoint "serial:/dev/ttyACM0?baud=9600"
//   lisbench monitor --a "serial:/dev/ttyACM0?baud=9600" --b tcp://lis-host:5000
//
// Fast test build: set LISDEV_FAST_TEST to 1 below (or pass
// -DLISDEV_FAST_TEST=1). Then the pause is 1 s and the timeouts are short
// (2 s reply, 4 s receive, 1 s busy); run lisbench with --fast-timing. This is
// not LIS01-A2: use the normal build against a real LIS.
//
// The LED blinks once for every delivered message, three times on failure.
// All values are invented.

#include <LisDevice.h>

namespace {

const uint8_t button_pin = 2;
const uint8_t led_pin = LED_BUILTIN;
#ifndef LISDEV_FAST_TEST
#define LISDEV_FAST_TEST 0
#endif

const unsigned long pause_ms = LISDEV_FAST_TEST ? 1000 : 30000;  // after the end of a transfer

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

const char header[] PROGMEM = "H|\\^&|||uno-analyzer^0.6|||||LIS||P|LIS2-A2";
const char terminator[] PROGMEM = "L|1|N";

unsigned int sample_no = 1;
unsigned long last_end = 0;
unsigned long led_off_at = 0;
uint8_t blinks_left = 0;

size_t copy_P(char* buf, size_t cap, const char* text) {
    strncpy_P(buf, text, cap);
    return strlen_P(text);
}

size_t make_record(void*, size_t index, char* buf, size_t cap) {
    switch (index) {
        case 0: return copy_P(buf, cap, header);
        case 1: return static_cast<size_t>(snprintf(buf, cap, "P|1||PID-%04u", sample_no));
        case 2: return static_cast<size_t>(snprintf(buf, cap, "O|1|S-%05u||^^^GLU|R||||||||||Serum", sample_no));
        case 3: {
            // Same value for every call of the same message (frames may be repeated).
            const int value = 70 + static_cast<int>((sample_no * 37U) % 80U);
            return static_cast<size_t>(snprintf(buf, cap, "R|1|^^^GLU|%d|mg/dL|70-110|%s||F", value,
                                                value > 110 ? "H" : "N"));
        }
        default: return copy_P(buf, cap, terminator);
    }
}

size_t record_count(void*) { return 5; }

void write_bytes(void*, const uint8_t* data, size_t n) { Serial.write(data, n); }

void blink(uint8_t times) {
    blinks_left = static_cast<uint8_t>(times * 2);
    led_off_at = millis();
}

void finished(void*, lisdev::Outcome outcome) {
    last_end = millis();
    if (outcome == lisdev::delivered) {
        ++sample_no;
        blink(1);
    } else {
        blink(3);
    }
}

lisdev::Callbacks callbacks = {nullptr, write_bytes, record_count, make_record, nullptr, finished};
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

    // millis() again: finished() may have set last_end after `now` was taken.
    const bool due = millis() - last_end >= pause_ms || digitalRead(button_pin) == LOW;
    if (due && device.state() == lisdev::idle) {
        (void)device.start(now);
    }

    if (blinks_left > 0 && now - led_off_at >= 150) {
        digitalWrite(led_pin, (blinks_left % 2) == 0 ? HIGH : LOW);
        --blinks_left;
        led_off_at = now;
    }
}

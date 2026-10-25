// An analyzer on an Arduino Uno that sends a glucose result every 30 seconds
// (or when pin 2 is pulled to GND) using CLSI LIS01-A2 / LIS02-A2.
//
// The Uno's USB serial port is the instrument interface. Plug it into the PC
// and let lisbench play the LIS:
//
//   lisbench sim --role computer --endpoint "serial:/dev/ttyACM0?baud=9600"
//   lisbench monitor --a "serial:/dev/ttyACM0?baud=9600" --b tcp://lis-host:5000
//
// The LED blinks once for every delivered message, three times on failure.
// All values are invented.

#include <LisDevice.h>

namespace {

const uint8_t button_pin = 2;
const uint8_t led_pin = LED_BUILTIN;
const unsigned long interval_ms = 30000;

const char header[] PROGMEM = "H|\\^&|||uno-analyzer^0.6|||||LIS||P|LIS2-A2";
const char terminator[] PROGMEM = "L|1|N";

unsigned int sample_no = 1;
unsigned long last_start = 0;
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
    if (outcome == lisdev::delivered) {
        ++sample_no;
        blink(1);
    } else {
        blink(3);
    }
}

lisdev::Callbacks callbacks = {nullptr, write_bytes, record_count, make_record, nullptr, finished};
lisdev::Device device(callbacks);

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

    const bool due = now - last_start >= interval_ms || digitalRead(button_pin) == LOW;
    if (due && device.state() == lisdev::idle && device.start(now)) {
        last_start = now;
    }

    if (blinks_left > 0 && now - led_off_at >= 150) {
        digitalWrite(led_pin, (blinks_left % 2) == 0 ? HIGH : LOW);
        --blinks_left;
        led_off_at = now;
    }
}

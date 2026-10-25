// An analyzer on an ESP32 that connects to a LIS over Wi-Fi/TCP and sends a
// result message every 30 seconds (CLSI LIS01-A2 / LIS02-A2). Messages from
// the LIS (orders, query answers) are printed on the USB serial console.
//
//   lisbench sim --role computer --endpoint tcp-listen://:15000 --serve
//
// Settings in config.h (see config.example.h). All values are invented.

#include <LisDevice.h>
#include <WiFi.h>

#if __has_include("config.h")
#include "config.h"
#else
#include "config.example.h"
#endif

namespace {

const unsigned long interval_ms = 30000;

WiFiClient lis;
unsigned int sample_no = 1;
unsigned long last_start = 0;
unsigned long last_connect = 0;

size_t make_record(void*, size_t index, char* buf, size_t cap) {
    switch (index) {
        case 0: return static_cast<size_t>(snprintf(buf, cap, "H|\\^&|||esp32-analyzer^0.6|||||LIS||P|LIS2-A2"));
        case 1: return static_cast<size_t>(snprintf(buf, cap, "P|1||PID-%04u", sample_no));
        case 2: return static_cast<size_t>(snprintf(buf, cap, "O|1|S-%05u||^^^TSH|R||||||||||Serum", sample_no));
        case 3: {
            const unsigned value = 4U + (sample_no * 13U) % 40U;  // 0.4 .. 4.3 mIU/L
            return static_cast<size_t>(snprintf(buf, cap, "R|1|^^^TSH|%u.%u|mIU/L|0.4-4.0|%s||F", value / 10U,
                                                value % 10U, value > 40U ? "H" : "N"));
        }
        default: return static_cast<size_t>(snprintf(buf, cap, "L|1|N"));
    }
}

size_t record_count(void*) { return 5; }

void write_bytes(void*, const uint8_t* data, size_t n) { lis.write(data, n); }

void received(void*, const char* text, size_t n, bool record_end) {
    Serial.write(reinterpret_cast<const uint8_t*>(text), n);
    if (record_end) {
        Serial.println();
    }
}

void finished(void*, lisdev::Outcome outcome) {
    static const char* const names[] = {"delivered", "rejected", "timeout"};
    Serial.printf("message %u: %s\n", sample_no, names[outcome]);
    if (outcome == lisdev::delivered) {
        ++sample_no;
    }
}

lisdev::Callbacks callbacks = {nullptr, write_bytes, record_count, make_record, received, finished};
lisdev::Device device(callbacks);

}  // namespace

void setup() {
    Serial.begin(115200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("esp32-analyzer: connecting to Wi-Fi");
}

void loop() {
    const unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        delay(200);
        return;
    }
    if (!lis.connected()) {
        if (now - last_connect >= 5000) {
            last_connect = now;
            Serial.printf("connecting to LIS %s:%d\n", LIS_HOST, LIS_PORT);
            if (lis.connect(LIS_HOST, LIS_PORT)) {
                lis.setNoDelay(true);
            }
        }
        return;
    }
    while (lis.available() > 0) {
        device.on_byte(static_cast<uint8_t>(lis.read()), now);
    }
    device.poll(now);
    if (now - last_start >= interval_ms && device.state() == lisdev::idle && device.start(now)) {
        last_start = now;
    }
}

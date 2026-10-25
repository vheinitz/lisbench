// The microcontroller instrument (devsim/LisDevice) against the lisbench
// link on a simulated wire with a simulated clock.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../devsim/LisDevice/src/LisDevice.h"
#include "lisbench/lis01_link.hpp"

using namespace lisbench;

namespace {

struct Bench {
    // Device side
    std::vector<std::string> records;
    std::vector<lisdev::Outcome> outcomes;
    std::string received_text;
    std::vector<std::string> received_records;
    // Wire
    std::deque<std::uint8_t> to_lis;
    std::deque<std::uint8_t> to_device;
    std::function<bool(std::uint8_t&)> device_fault;  // false: drop
    // LIS side
    lis01::Link lis{lis01::Role::computer};
    std::vector<bytes> lis_records;
    std::vector<lis01::ev::SendFinished> lis_results;
    std::optional<std::pair<lis01::TimerId, std::uint32_t>> lis_timer;
    std::uint32_t now{1000};

    lisdev::Callbacks callbacks() {
        return {
            this,
            [](void* c, const std::uint8_t* d, std::size_t n) {
                auto* b = static_cast<Bench*>(c);
                for (std::size_t i = 0; i < n; ++i) {
                    auto byte = d[i];  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
                    if (!b->device_fault || b->device_fault(byte)) {
                        b->to_lis.push_back(byte);
                    }
                }
            },
            [](void* c) { return static_cast<Bench*>(c)->records.size(); },
            [](void* c, std::size_t i, char* buf, std::size_t cap) {
                const auto& r = static_cast<Bench*>(c)->records.at(i);
                r.copy(buf, std::min(r.size(), cap));
                return r.size();
            },
            [](void* c, const char* text, std::size_t n, bool end) {
                auto* b = static_cast<Bench*>(c);
                b->received_text.append(text, n);
                if (end) {
                    b->received_records.push_back(std::exchange(b->received_text, {}));
                }
            },
            [](void* c, lisdev::Outcome o) { static_cast<Bench*>(c)->outcomes.push_back(o); },
        };
    }

    void handle(const lis01::Events& ev) {
        for (const auto& e : ev) {
            if (const auto* s = std::get_if<lis01::ev::Send>(&e)) {
                for (const auto b : s->data) {
                    to_device.push_back(std::to_integer<std::uint8_t>(b));
                }
            } else if (const auto* t = std::get_if<lis01::ev::ArmTimer>(&e)) {
                lis_timer = std::pair{t->id, now + static_cast<std::uint32_t>(t->after.count())};
            } else if (std::holds_alternative<lis01::ev::CancelTimer>(e)) {
                lis_timer.reset();
            } else if (const auto* r = std::get_if<lis01::ev::RecordReceived>(&e)) {
                lis_records.push_back(r->record);
            } else if (const auto* f = std::get_if<lis01::ev::SendFinished>(&e)) {
                lis_results.push_back(*f);
            }
        }
    }

    // Runs until both sides are quiet, advancing the clock in 10 ms steps.
    void run(lisdev::Device& device, std::uint32_t max_ms = 120'000) {
        const auto end = now + max_ms;
        while (now < end) {
            bool moved = false;
            while (!to_lis.empty()) {
                const auto b = static_cast<std::byte>(to_lis.front());
                to_lis.pop_front();
                lis01::Events ev;
                lis.on_bytes(bytes_view{&b, 1}, ev);
                handle(ev);
                moved = true;
            }
            while (!to_device.empty()) {
                const auto b = to_device.front();
                to_device.pop_front();
                device.on_byte(b, now);
                moved = true;
            }
            if (moved) {
                continue;
            }
            if (device.state() == lisdev::idle && lis.state() == lis01::State::neutral &&
                !lis_timer && lis.queued() == 0) {
                return;
            }
            now += 10;
            device.poll(now);
            if (lis_timer && now >= lis_timer->second) {
                const auto id = lis_timer->first;
                lis_timer.reset();
                lis01::Events ev;
                lis.on_timer(id, ev);
                handle(ev);
            }
        }
    }

    [[nodiscard]] std::vector<std::string> lis_text() const {
        std::vector<std::string> out;
        for (const auto& r : lis_records) {
            out.push_back(to_string(r));
        }
        return out;
    }
};

}  // namespace

TEST_CASE("devsim: the device delivers a message, long records split into frames") {
    Bench b;
    // 250 bytes: two frames, within LISDEV_RECORD_MAX (256).
    b.records = {
        "H|\\^&|||uno-analyzer", "P|1",  "O|1|S-1", std::string("C|1|I|") + std::string(244, 'x'),
        "R|1|^^^GLU|105|mg/dL",  "L|1|N"};
    lisdev::Device device{b.callbacks()};
    REQUIRE(device.start(b.now));
    CHECK_FALSE(device.start(b.now));  // busy
    b.run(device);
    REQUIRE(b.outcomes.size() == 1);
    CHECK(b.outcomes[0] == lisdev::delivered);
    CHECK(b.lis_text() == b.records);
}

TEST_CASE("devsim: a record longer than LISDEV_RECORD_MAX is refused, not cut") {
    Bench b;
    b.records = {"H|\\^&", std::string("C|1|I|") + std::string(300, 'x'), "L|1|N"};
    lisdev::Device device{b.callbacks()};
    CHECK_FALSE(device.start(b.now));
    CHECK(b.to_lis.empty());
    CHECK(device.state() == lisdev::idle);
}

TEST_CASE("devsim: refused frames are repeated, six refusals end the transfer") {
    Bench b;
    b.records = {"H|\\^&", "L|1|N"};
    lisdev::Device device{b.callbacks()};

    SECTION("one corrupted frame") {
        int frames = 0;
        b.device_fault = [&frames](std::uint8_t& c) {
            if (c == lisdev::ctl::stx && ++frames == 1) {
                return true;
            }
            if (frames == 1 && c == 'H') {
                c = 'X';  // checksum no longer matches
            }
            return true;
        };
        REQUIRE(device.start(b.now));
        b.run(device);
        CHECK(b.outcomes == std::vector{lisdev::delivered});
        CHECK(b.lis_text() == b.records);
    }
    SECTION("every frame corrupted") {
        b.device_fault = [](std::uint8_t& c) {
            if (c == 'H') {
                c = 'X';
            }
            return true;
        };
        REQUIRE(device.start(b.now));
        b.run(device);
        CHECK(b.outcomes == std::vector{lisdev::rejected});
        CHECK(b.lis_records.empty());
    }
}

TEST_CASE("devsim: no answer means timeout after 15 s") {
    Bench b;
    b.records = {"H|\\^&", "L|1|N"};
    b.device_fault = [](std::uint8_t&) { return false; };  // the LIS never hears anything
    lisdev::Device device{b.callbacks()};
    const auto started = b.now;
    REQUIRE(device.start(b.now));
    b.run(device);
    CHECK(b.outcomes == std::vector{lisdev::timeout});
    CHECK(b.now - started >= 15'000);
}

TEST_CASE("devsim: the device receives what the LIS sends") {
    Bench b;
    lisdev::Device device{b.callbacks()};
    lis01::Message orders;
    for (const auto* r : {"H|\\^&|||LIS", "P|1", "O|1|S-7||^^^ANA", "L|1|F"}) {
        orders.records.push_back(to_bytes(r));
    }
    orders.records.push_back(to_bytes(std::string("C|1|L|") + std::string(500, 'y')));
    lis01::Events ev;
    (void)b.lis.submit(orders, ev);
    b.handle(ev);
    b.run(device);
    REQUIRE(b.lis_results.size() == 1);
    CHECK(b.lis_results[0].outcome == lis01::Outcome::delivered);
    REQUIRE(b.received_records.size() == 5);
    CHECK(b.received_records[2] == "O|1|S-7||^^^ANA");
    CHECK(b.received_records[4].size() == 506);
}

TEST_CASE("devsim: contention, the instrument goes first and the LIS gets its turn") {
    Bench b;
    b.records = {"H|\\^&|||instrument", "L|1|N"};
    lisdev::Device device{b.callbacks()};
    lis01::Message from_lis;
    from_lis.records = {to_bytes("H|\\^&|||LIS"), to_bytes("L|1|N")};
    lis01::Events ev;
    (void)b.lis.submit(from_lis, ev);  // LIS sends ENQ ...
    REQUIRE(device.start(b.now));      // ... and so does the device
    b.handle(ev);
    b.run(device);
    CHECK(b.outcomes == std::vector{lisdev::delivered});
    REQUIRE(b.lis_results.size() == 1);
    CHECK(b.lis_results[0].outcome == lis01::Outcome::delivered);
    CHECK(b.lis_text().front() == "H|\\^&|||instrument");
    CHECK(b.received_records.front() == "H|\\^&|||LIS");
}

TEST_CASE("devsim: a frame with a restricted character in its text is refused") {
    Bench b;
    lisdev::Device device{b.callbacks()};
    const auto feed = [&](const bytes& data) {
        for (const auto x : data) {
            device.on_byte(std::to_integer<std::uint8_t>(x), b.now);
        }
    };
    const auto reply = [&] {
        REQUIRE(b.to_lis.size() == 1);
        const auto r = b.to_lis.front();
        b.to_lis.clear();
        return r;
    };
    feed({lis01::ctl::enq});
    CHECK(reply() == 0x06);
    feed(lis01::encode({.number = 1, .last = true, .text = to_bytes("H|\\^&\n|LIS\r")}));
    CHECK(reply() == 0x15);  // NAK: LF is restricted in frame text
    CHECK(b.received_records.empty());
    feed(lis01::encode({.number = 1, .last = true, .text = to_bytes("H|\\^&|LIS\r")}));
    CHECK(reply() == 0x06);
    REQUIRE(b.received_records.size() == 1);
    CHECK(b.received_records[0] == "H|\\^&|LIS");
}

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "lisbench/unframed.hpp"

using namespace lisbench;
using namespace lisbench::lis01;
using namespace std::chrono_literals;

namespace {

Message message_of(std::initializer_list<std::string_view> records) {
    Message m;
    for (const auto r : records) {
        m.records.push_back(to_bytes(r));
    }
    return m;
}

template <typename T>
std::vector<T> all(const Events& events) {
    std::vector<T> out;
    for (const auto& e : events) {
        if (const auto* p = std::get_if<T>(&e)) {
            out.push_back(*p);
        }
    }
    return out;
}

std::vector<std::string> records(const Events& events) {
    std::vector<std::string> out;
    for (const auto& r : all<ev::RecordReceived>(events)) {
        out.push_back(to_string(r.record));
    }
    return out;
}

std::vector<std::string> decoded_records(const std::vector<UnframedEvent>& events) {
    std::vector<std::string> out;
    for (const auto& e : events) {
        if (const auto* r = std::get_if<unframed::Record>(&e)) {
            out.push_back(to_string(r->text));
        }
    }
    return out;
}

template <typename T>
std::size_t count(const std::vector<UnframedEvent>& events) {
    return static_cast<std::size_t>(std::ranges::count_if(
        events, [](const UnframedEvent& e) { return std::holds_alternative<T>(e); }));
}

}  // namespace

TEST_CASE("low level: names", "[unframed]") {
    CHECK(to_string(LowLevel::lis01_a2) == "lis01-a2");
    CHECK(to_string(LowLevel::none) == "none");
    CHECK(to_string(LowLevel::mllp) == "mllp");
    CHECK(low_level_from("mllp") == LowLevel::mllp);
    CHECK(low_level_from("none") == LowLevel::none);
    CHECK(low_level_from("lis01-a2") == LowLevel::lis01_a2);
    CHECK_FALSE(low_level_from("lis01"));
}

TEST_CASE("unframed: encoding without LIS01, records end with CR", "[unframed]") {
    const auto m = message_of({"H|\\^&", "L|1|N"});
    CHECK(to_string(encode_unframed(LowLevel::none, m)) == "H|\\^&\rL|1|N\r");
    CHECK(to_string(encode_unframed(LowLevel::mllp, m)) == "\x0bH|\\^&\rL|1|N\r\x1c\r");
}

TEST_CASE("unframed decoder, none: a message ends with its L record", "[unframed]") {
    UnframedDecoder d{LowLevel::none};
    std::vector<UnframedEvent> out;
    d.feed(to_bytes("H|\\^&\r\nP|1\rO|1|S-1"), out);
    CHECK(decoded_records(out) == std::vector<std::string>{"H|\\^&", "P|1"});
    CHECK(d.in_message());
    d.feed(to_bytes("\rL|1|N\r"), out);
    CHECK(decoded_records(out) == std::vector<std::string>{"H|\\^&", "P|1", "O|1|S-1", "L|1|N"});
    REQUIRE(count<unframed::MessageEnd>(out) == 1);
    CHECK(std::get<unframed::MessageEnd>(out.back()).complete);
    CHECK_FALSE(d.in_message());
}

TEST_CASE("unframed decoder, none: H before L ends the message as incomplete", "[unframed]") {
    UnframedDecoder d{LowLevel::none};
    std::vector<UnframedEvent> out;
    d.feed(to_bytes("H|\\^&\rP|1\rH|\\^&\rL|1|N\r"), out);
    std::vector<bool> ends;
    for (const auto& e : out) {
        if (const auto* m = std::get_if<unframed::MessageEnd>(&e)) {
            ends.push_back(m->complete);
        }
    }
    CHECK(ends == std::vector<bool>{false, true});
}

TEST_CASE("unframed decoder, none: LIS01-A2 control characters are reported once", "[unframed]") {
    UnframedDecoder d{LowLevel::none};
    std::vector<UnframedEvent> out;
    d.feed(to_bytes("\x05\x02"
                    "1H|\\^&\r\x03"
                    "45\r\n"),
           out);
    CHECK(count<unframed::Problem>(out) == 1);
    CHECK(std::get<unframed::Problem>(out.front()).text.find("LIS01-A2") != std::string::npos);
}

TEST_CASE("unframed decoder, mllp: block from VT to FS CR, bytes outside are noise", "[unframed]") {
    UnframedDecoder d{LowLevel::mllp};
    std::vector<UnframedEvent> out;
    d.feed(to_bytes("xy\x0bH|\\^&\rL|1|N\r\x1c"), out);
    CHECK(count<unframed::Noise>(out) == 1);
    CHECK(decoded_records(out) == std::vector<std::string>{"H|\\^&", "L|1|N"});
    REQUIRE(count<unframed::MessageEnd>(out) == 1);
    CHECK(std::get<unframed::MessageEnd>(out.back()).complete);
    d.feed(to_bytes("\r"), out);  // the CR after FS belongs to the block
    CHECK(count<unframed::Noise>(out) == 1);
    CHECK(count<unframed::Problem>(out) == 0);
}

TEST_CASE("unframed link, mllp: a LIS01-A2 partner is pointed out at once", "[unframed]") {
    UnframedLink link{LowLevel::mllp};
    Events ev;
    link.on_bytes(to_bytes("\x05"), ev);
    const auto w = all<ev::Warning>(ev);
    REQUIRE(w.size() == 1);
    CHECK(w[0].text.find("does the partner use LIS01-A2?") != std::string::npos);
}

TEST_CASE("unframed decoder, mllp: VT inside a block breaks it off", "[unframed]") {
    UnframedDecoder d{LowLevel::mllp};
    std::vector<UnframedEvent> out;
    d.feed(to_bytes("\x0bH|\\^&\r\x0bH|\\^&\rL|1|N\r\x1c\r"), out);
    std::vector<bool> ends;
    for (const auto& e : out) {
        if (const auto* m = std::get_if<unframed::MessageEnd>(&e)) {
            ends.push_back(m->complete);
        }
    }
    CHECK(ends == std::vector<bool>{false, true});
}

TEST_CASE("unframed decoder: end of stream reports an unfinished message", "[unframed]") {
    UnframedDecoder d{LowLevel::none};
    std::vector<UnframedEvent> out;
    d.feed(to_bytes("H|\\^&\rP|1"), out);
    d.finish(out);
    CHECK(decoded_records(out) == std::vector<std::string>{"H|\\^&", "P|1"});
    REQUIRE(count<unframed::MessageEnd>(out) == 1);
    CHECK_FALSE(std::get<unframed::MessageEnd>(out.back()).complete);
}

TEST_CASE("unframed link: a message is written at once, no acknowledgement", "[unframed]") {
    UnframedLink link{LowLevel::none};
    Events ev;
    const auto id = link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    REQUIRE(all<ev::Send>(ev).size() == 1);
    CHECK(to_string(all<ev::Send>(ev)[0].data) == "H|\\^&\rL|1|N\r");
    REQUIRE(all<ev::SendFinished>(ev).size() == 1);
    CHECK(all<ev::SendFinished>(ev)[0].id == id);
    CHECK(all<ev::SendFinished>(ev)[0].outcome == Outcome::delivered);
}

TEST_CASE("unframed link: received records and the end of the message", "[unframed]") {
    UnframedLink link{LowLevel::mllp};
    Events ev;
    link.on_bytes(to_bytes("\x0bH|\\^&\rQ|1|^S-1\r"), ev);
    CHECK(records(ev) == std::vector<std::string>{"H|\\^&", "Q|1|^S-1"});
    CHECK(link.state() == State::receiving);
    CHECK(all<ev::ArmTimer>(ev).size() == 1);
    link.on_bytes(to_bytes("L|1|N\r\x1c\r"), ev);
    REQUIRE(all<ev::ReceiveFinished>(ev).size() == 1);
    CHECK(all<ev::ReceiveFinished>(ev)[0].complete);
    CHECK(link.state() == State::neutral);
    CHECK_FALSE(all<ev::CancelTimer>(ev).empty());
}

TEST_CASE("unframed link: a message that stops coming ends after the receive timeout",
          "[unframed]") {
    UnframedLink link{LowLevel::none, {.receive_timeout = 5s}};
    Events ev;
    link.on_bytes(to_bytes("H|\\^&\rP|1\r"), ev);
    const auto timers = all<ev::ArmTimer>(ev);
    REQUIRE(!timers.empty());
    CHECK(timers.back().after == 5s);
    Events later;
    link.on_timer(timers.back().id, later);
    REQUIRE(all<ev::ReceiveFinished>(later).size() == 1);
    CHECK_FALSE(all<ev::ReceiveFinished>(later)[0].complete);
    CHECK(link.state() == State::neutral);
    // The next message starts afresh.
    Events next;
    link.on_bytes(to_bytes("H|\\^&\rL|1|N\r"), next);
    CHECK(records(next) == std::vector<std::string>{"H|\\^&", "L|1|N"});
}

TEST_CASE("unframed link: messages wait while disconnected and go out on resume", "[unframed]") {
    UnframedLink link{LowLevel::none};
    Events ev;
    link.on_bytes(to_bytes("H|\\^&\r"), ev);
    link.on_disconnect(ev);
    REQUIRE(all<ev::ReceiveFinished>(ev).size() == 1);
    CHECK_FALSE(all<ev::ReceiveFinished>(ev)[0].complete);
    Events queued;
    const auto id = link.submit(message_of({"H|\\^&", "L|1|N"}), queued);
    CHECK(all<ev::Send>(queued).empty());
    CHECK(link.queued() == 1);
    Events resumed;
    link.resume(resumed);
    CHECK(all<ev::Send>(resumed).size() == 1);
    REQUIRE(all<ev::SendFinished>(resumed).size() == 1);
    CHECK(all<ev::SendFinished>(resumed)[0].id == id);
}

TEST_CASE("unframed link: cancel_all finishes every queued message", "[unframed]") {
    UnframedLink link{LowLevel::none};
    Events ev;
    link.on_disconnect(ev);
    (void)link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    (void)link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    Events done;
    link.cancel_all(done);
    const auto f = all<ev::SendFinished>(done);
    REQUIRE(f.size() == 2);
    CHECK(f[0].outcome == Outcome::cancelled);
    CHECK(f[1].outcome == Outcome::cancelled);
}

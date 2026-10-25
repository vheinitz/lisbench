#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>

#include "lisbench/lis01_link.hpp"

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

std::string record_of_length(std::size_t n, char fill) {
    std::string r = "C|1|I|";
    r.resize(n, fill);
    return r;
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

bytes sent(const Events& events) {
    bytes out;
    for (const auto& s : all<ev::Send>(events)) {
        out.insert(out.end(), s.data.begin(), s.data.end());
    }
    return out;
}

std::optional<TimerId> armed(const Events& events) {
    const auto timers = all<ev::ArmTimer>(events);
    if (timers.empty()) {
        return std::nullopt;
    }
    return timers.back().id;
}

void feed(Link& link, std::byte b, Events& out) {
    link.on_bytes(bytes_view{&b, 1}, out);
}

// Two links on a simulated wire with a simulated clock. Bytes arrive
// immediately and in order; a fault hook may drop or alter them.
class Wire {
public:
    struct Side {
        explicit Side(Link l) : link{std::move(l)} {}
        Link link;
        std::optional<std::pair<TimerId, milliseconds>> timer;
        std::map<MessageId, std::vector<Outcome>> finished;
        std::vector<bytes> records;
        std::vector<ev::ReceiveFinished> receive_ends;
        std::vector<ev::SendFinished> send_results;
        int warnings{0};
    };

    using Fault = std::function<bool(int from, std::byte& b)>;  // false = drop

    Wire(Role a, Role b, Timing timing = {})
        : sides_{Side{Link{a, timing}}, Side{Link{b, timing}}} {}

    Side& operator[](int i) { return sides_.at(static_cast<std::size_t>(i)); }
    [[nodiscard]] milliseconds now() const { return now_; }
    void set_fault(Fault f) { fault_ = std::move(f); }

    MessageId submit(int side, Message m) {
        Events ev;
        const auto id = (*this)[side].link.submit(std::move(m), ev);
        handle(side, ev);
        return id;
    }

    void handle(int who, const Events& events) {
        auto& s = (*this)[who];
        for (const auto& e : events) {
            if (const auto* send = std::get_if<ev::Send>(&e)) {
                for (auto b : send->data) {
                    if (!fault_ || fault_(who, b)) {
                        in_flight_.emplace_back(1 - who, b);
                    }
                }
            } else if (const auto* arm = std::get_if<ev::ArmTimer>(&e)) {
                s.timer = std::pair{arm->id, now_ + arm->after};
            } else if (std::holds_alternative<ev::CancelTimer>(e)) {
                s.timer.reset();
            } else if (const auto* fin = std::get_if<ev::SendFinished>(&e)) {
                s.finished[fin->id].push_back(fin->outcome);
                s.send_results.push_back(*fin);
            } else if (const auto* rec = std::get_if<ev::RecordReceived>(&e)) {
                s.records.push_back(rec->record);
            } else if (const auto* end = std::get_if<ev::ReceiveFinished>(&e)) {
                s.receive_ends.push_back(*end);
            } else if (std::holds_alternative<ev::Warning>(e)) {
                ++s.warnings;
            }
        }
    }

    // Runs until nothing is in flight and no timer is armed, or until
    // the time limit.
    void run(milliseconds limit = 3'600'000ms) {
        for (int guard = 0; guard < 1'000'000; ++guard) {
            if (!in_flight_.empty()) {
                const auto [to, b] = in_flight_.front();
                in_flight_.pop_front();
                Events ev;
                feed((*this)[to].link, b, ev);
                handle(to, ev);
                continue;
            }
            int next = -1;
            for (int i = 0; i < 2; ++i) {
                const auto& t = (*this)[i].timer;
                if (t && (next < 0 || t->second < (*this)[next].timer->second)) {
                    next = i;
                }
            }
            if (next < 0 || (*this)[next].timer->second > limit) {
                return;
            }
            const auto [id, at] = *(*this)[next].timer;
            now_ = at;
            (*this)[next].timer.reset();
            Events ev;
            (*this)[next].link.on_timer(id, ev);
            handle(next, ev);
        }
        FAIL("simulation did not settle");
    }

private:
    std::array<Side, 2> sides_;
    std::deque<std::pair<int, std::byte>> in_flight_;
    milliseconds now_{0};
    Fault fault_;
};

}  // namespace

TEST_CASE("message is delivered from instrument to computer") {
    Wire wire{Role::instrument, Role::computer};
    const auto id = wire.submit(
        0, message_of({"H|\\^&", "P|1", "O|1|S1||^^^GLU", "R|1|^^^GLU|5.4|mmol/L", "L|1|N"}));
    wire.run();

    CHECK(wire[0].finished.at(id) == std::vector{Outcome::delivered});
    REQUIRE(wire[1].records.size() == 5);
    CHECK(to_string(wire[1].records[3]) == "R|1|^^^GLU|5.4|mmol/L");
    REQUIRE(wire[1].receive_ends.size() == 1);
    CHECK(wire[1].receive_ends[0].complete);
    CHECK(wire[0].link.state() == State::neutral);
    CHECK(wire[1].link.state() == State::neutral);
    CHECK(wire[0].warnings == 0);
    CHECK(wire[1].warnings == 0);
}

TEST_CASE("long records span frames and frame numbers wrap") {
    Wire wire{Role::instrument, Role::computer};
    const auto big = record_of_length(5000, 'x');  // 21 frames
    const auto id = wire.submit(0, message_of({"H|\\^&", big, "L|1|N"}));
    wire.run();
    CHECK(wire[0].finished.at(id) == std::vector{Outcome::delivered});
    REQUIRE(wire[1].records.size() == 3);
    CHECK(to_string(wire[1].records[1]) == big);
}

TEST_CASE("queued messages are sent one after another, in order") {
    Wire wire{Role::computer, Role::instrument};
    const auto first = wire.submit(0, message_of({"H|\\^&", "L|1|N"}));
    const auto second = wire.submit(0, message_of({"H|\\^&|||second", "L|1|N"}));
    wire.run();
    CHECK(wire[0].finished.at(first) == std::vector{Outcome::delivered});
    CHECK(wire[0].finished.at(second) == std::vector{Outcome::delivered});
    REQUIRE(wire[1].records.size() == 4);
    CHECK(to_string(wire[1].records[2]) == "H|\\^&|||second");
    CHECK(wire[1].receive_ends.size() == 2);
}

TEST_CASE("contention: the instrument goes first, the computer afterwards") {
    Wire wire{Role::instrument, Role::computer};
    const auto from_instrument = wire.submit(0, message_of({"H|\\^&|||instrument", "L|1|N"}));
    const auto from_computer = wire.submit(1, message_of({"H|\\^&|||computer", "L|1|N"}));
    wire.run();
    CHECK(wire[0].finished.at(from_instrument) == std::vector{Outcome::delivered});
    CHECK(wire[1].finished.at(from_computer) == std::vector{Outcome::delivered});
    REQUIRE(wire[1].records.size() == 2);
    CHECK(to_string(wire[1].records[0]) == "H|\\^&|||instrument");
    REQUIRE(wire[0].records.size() == 2);
    CHECK(to_string(wire[0].records[0]) == "H|\\^&|||computer");
}

TEST_CASE("contention: the computer goes first when both agree it wins") {
    Wire wire{Role::instrument, Role::computer, Timing{.contention_winner = Role::computer}};
    const auto from_instrument = wire.submit(0, message_of({"H|\\^&|||instrument", "L|1|N"}));
    const auto from_computer = wire.submit(1, message_of({"H|\\^&|||computer", "L|1|N"}));
    wire.run();
    CHECK(wire[0].finished.at(from_instrument) == std::vector{Outcome::delivered});
    CHECK(wire[1].finished.at(from_computer) == std::vector{Outcome::delivered});
    REQUIRE(wire[0].records.size() == 2);
    REQUIRE(wire[1].records.size() == 2);
    CHECK(to_string(wire[0].records[0]) == "H|\\^&|||computer");
    CHECK(to_string(wire[1].records[0]) == "H|\\^&|||instrument");

    // Who yields, seen on one link at a time.
    const Timing computer_wins{.contention_winner = Role::computer};
    for (const auto role : {Role::instrument, Role::computer}) {
        Link link{role, computer_wins};
        Events ev;
        (void)link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
        ev.clear();
        feed(link, ctl::enq, ev);
        CHECK(link.state() == (role == Role::computer ? State::contention_wait : State::neutral));
        REQUIRE(all<ev::ArmTimer>(ev).size() == 1);
        CHECK(all<ev::ArmTimer>(ev)[0].after == (role == Role::computer ? 1s : 20s));
    }
}

TEST_CASE("receiver: a repeated ENQ before the first frame is acknowledged again") {
    // The sender did not get our ACK (lost, or turned into NAK on the way)
    // and asks again; found against an Arduino with a NAK injected on the ENQ reply.
    Link link{Role::computer};
    Events ev;
    feed(link, ctl::enq, ev);
    CHECK(sent(ev) == bytes{ctl::ack});
    ev.clear();
    feed(link, ctl::enq, ev);
    CHECK(sent(ev) == bytes{ctl::ack});
    CHECK(all<ev::Warning>(ev).empty());
    CHECK(link.state() == State::receiving);
    REQUIRE(all<ev::ArmTimer>(ev).size() == 1);  // receive timeout starts again

    // After the first frame, ENQ is a protocol error and is not acknowledged.
    ev.clear();
    for (const auto& f : frames_for(message_of({"H|\\^&", "L|1|N"}))) {
        const auto raw = encode(f);
        link.on_bytes(raw, ev);
    }
    ev.clear();
    feed(link, ctl::enq, ev);
    CHECK(sent(ev).empty());
    CHECK(all<ev::Warning>(ev).size() == 1);
}

TEST_CASE("sender: ENQ answered with NAK is retried after busy_retry") {
    Link link{Role::instrument};
    Events ev;
    (void)link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    CHECK(sent(ev) == bytes{ctl::enq});

    ev.clear();
    feed(link, ctl::nak, ev);
    CHECK(link.state() == State::busy_wait);
    REQUIRE(all<ev::ArmTimer>(ev).size() == 1);
    CHECK(all<ev::ArmTimer>(ev)[0].after == 10s);

    const auto timer = *armed(ev);
    ev.clear();
    link.on_timer(timer, ev);
    CHECK(sent(ev) == bytes{ctl::enq});
    CHECK(link.state() == State::establishing);
}

TEST_CASE("sender: a frame refused six times ends the transfer with EOT") {
    Link link{Role::instrument};
    Events ev;
    const auto id = link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    ev.clear();
    feed(link, ctl::ack, ev);  // establishment
    int transmissions = static_cast<int>(all<ev::Send>(ev).size());
    for (int i = 0; i < 6; ++i) {
        ev.clear();
        feed(link, ctl::nak, ev);
        if (i < 5) {
            transmissions += static_cast<int>(all<ev::Send>(ev).size());
        }
    }
    CHECK(transmissions == 6);
    CHECK(sent(ev) == bytes{ctl::eot});
    const auto results = all<ev::SendFinished>(ev);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == id);
    CHECK(results[0].outcome == Outcome::rejected);
    CHECK(link.state() == State::neutral);
}

TEST_CASE("sender: no reply ends the transfer with EOT and timeout, exactly once") {
    Link link{Role::instrument};
    Events ev;
    const auto id = link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    REQUIRE(all<ev::ArmTimer>(ev).at(0).after == 15s);

    SECTION("during establishment") {
        const auto timer = *armed(ev);
        ev.clear();
        link.on_timer(timer, ev);
    }
    SECTION("while waiting for a frame reply") {
        ev.clear();
        feed(link, ctl::ack, ev);
        const auto timer = *armed(ev);
        ev.clear();
        link.on_timer(timer, ev);
    }
    CHECK(sent(ev) == bytes{ctl::eot});
    const auto results = all<ev::SendFinished>(ev);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == id);
    CHECK(results[0].outcome == Outcome::timeout);

    // A late reply after the timeout changes nothing.
    ev.clear();
    feed(link, ctl::ack, ev);
    CHECK(all<ev::SendFinished>(ev).empty());
}

TEST_CASE("sender: stale timers are ignored") {
    Link link{Role::instrument};
    Events ev;
    (void)link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    const auto first = *armed(ev);
    ev.clear();
    feed(link, ctl::ack, ev);  // re-arms
    ev.clear();
    link.on_timer(first, ev);
    CHECK(ev.empty());
    CHECK(link.state() == State::sending);
}

TEST_CASE("sender: EOT instead of ACK is an interrupt request, the transfer goes on") {
    Link link{Role::instrument};
    Events ev;
    (void)link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    feed(link, ctl::ack, ev);
    feed(link, ctl::eot, ev);  // answers frame 1
    feed(link, ctl::ack, ev);  // answers frame 2
    const auto results = all<ev::SendFinished>(ev);
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == Outcome::delivered);
    CHECK(results[0].interrupt_requested);
}

TEST_CASE("disconnect ends the running transfer; queued messages wait for resume") {
    Link link{Role::computer};
    Events ev;
    const auto running = link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    const auto queued = link.submit(message_of({"H|\\^&", "L|1|N"}), ev);
    feed(link, ctl::ack, ev);
    ev.clear();
    link.on_disconnect(ev);
    auto results = all<ev::SendFinished>(ev);
    REQUIRE(results.size() == 1);
    CHECK(results[0].id == running);
    CHECK(results[0].outcome == Outcome::disconnected);
    CHECK(link.queued() == 1);

    ev.clear();
    Events more;
    (void)link.submit(message_of({"H|\\^&", "L|1|N"}), more);
    CHECK(sent(more).empty());  // still disconnected

    link.resume(ev);
    CHECK(sent(ev) == bytes{ctl::enq});

    ev.clear();
    link.cancel(queued, ev);
    link.cancel_all(ev);
    results = all<ev::SendFinished>(ev);
    CHECK(results.size() == 2);
    CHECK(std::ranges::all_of(results,
                              [](const auto& r) { return r.outcome == Outcome::cancelled; }));
    // Each started transfer is terminated with EOT; cancelling the running
    // one lets the next queued message start before cancel_all ends it.
    CHECK(sent(ev) == bytes{ctl::eot, ctl::enq, ctl::eot});
}

TEST_CASE("receiver: checks, duplicates and timeouts") {
    Link link{Role::computer};
    Events ev;
    feed(link, ctl::enq, ev);
    CHECK(sent(ev) == bytes{ctl::ack});
    REQUIRE(all<ev::ArmTimer>(ev).at(0).after == 30s);

    const auto frame1 = encode(Frame{.number = 1, .last = true, .text = to_bytes("H|\\^&\r")});
    const auto frame2 = encode(Frame{.number = 2, .last = true, .text = to_bytes("L|1|N\r")});

    SECTION("bad checksum is answered with NAK") {
        auto bad = frame1;
        bad[3] = std::byte{'#'};
        ev.clear();
        link.on_bytes(bad, ev);
        CHECK(sent(ev) == bytes{ctl::nak});
        CHECK(all<ev::RecordReceived>(ev).empty());
    }
    SECTION("unexpected frame number is answered with NAK") {
        ev.clear();
        link.on_bytes(frame2, ev);
        CHECK(sent(ev) == bytes{ctl::nak});
    }
    SECTION("a repeated frame is acknowledged and dropped") {
        ev.clear();
        link.on_bytes(frame1, ev);
        link.on_bytes(frame1, ev);
        CHECK(sent(ev) == bytes{ctl::ack, ctl::ack});
        CHECK(all<ev::RecordReceived>(ev).size() == 1);
        CHECK(all<ev::Warning>(ev).size() == 1);
    }
    SECTION("silence ends the reception") {
        ev.clear();
        link.on_bytes(frame1, ev);
        const auto timer = *armed(ev);
        ev.clear();
        link.on_timer(timer, ev);
        const auto ends = all<ev::ReceiveFinished>(ev);
        REQUIRE(ends.size() == 1);
        CHECK_FALSE(ends[0].complete);
        CHECK(link.state() == State::neutral);
    }
    SECTION("not ready: ENQ is refused") {
        Link busy{Role::computer};
        busy.set_receiver_ready(false);
        Events e2;
        feed(busy, ctl::enq, e2);
        CHECK(sent(e2) == bytes{ctl::nak});
        CHECK(busy.state() == State::neutral);
    }
}

TEST_CASE("property: with a lossy wire every message gets exactly one result") {
    // Random drops and bit errors in both directions, at most one bit error
    // per frame: two errors in one frame can cancel out in the LIS01-A2
    // checksum - a limit of the standard, not of this code. Whatever happens: each submitted
    // message is reported exactly once, nothing corrupted is accepted, and
    // both links come to rest.
    for (std::uint32_t seed = 1; seed <= 200; ++seed) {
        Wire wire{Role::instrument, Role::computer};
        std::mt19937 rng{seed};
        std::bernoulli_distribution drop{0.005};
        std::bernoulli_distribution flip{0.01};
        std::array<bool, 2> flipped_in_frame{};
        wire.set_fault([&](int from, std::byte& b) {
            auto& flipped = flipped_in_frame.at(static_cast<std::size_t>(from));
            if (b == ctl::stx) {
                flipped = false;
            }
            if (drop(rng)) {
                return false;
            }
            if (flip(rng) && !flipped) {
                b ^= std::byte{0x40};
                flipped = true;
            }
            return true;
        });

        std::set<bytes> legal;
        std::vector<MessageId> ids0;
        std::vector<MessageId> ids1;
        for (int m = 0; m < 4; ++m) {
            auto msg = message_of({"H|\\^&", "P|1", "O|1|S" + std::to_string(m)});
            msg.records.push_back(
                to_bytes(record_of_length(300 + 97 * static_cast<std::size_t>(m), 'a')));
            msg.records.push_back(to_bytes("L|1|N"));
            for (const auto& r : msg.records) {
                legal.insert(r);
            }
            ids0.push_back(wire.submit(0, msg));
            if (m % 2 == 0) {
                ids1.push_back(wire.submit(1, msg));
            }
        }
        wire.run();

        for (const auto id : ids0) {
            REQUIRE(wire[0].finished[id].size() == 1);
        }
        for (const auto id : ids1) {
            REQUIRE(wire[1].finished[id].size() == 1);
        }
        CHECK(wire[0].finished.size() == ids0.size());
        CHECK(wire[1].finished.size() == ids1.size());
        for (int side = 0; side < 2; ++side) {
            for (const auto& r : wire[side].records) {
                REQUIRE(legal.contains(r));
            }
            CHECK(wire[side].link.state() == State::neutral);
        }
    }
}

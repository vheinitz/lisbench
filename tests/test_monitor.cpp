#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include "lisbench/lis01_link.hpp"
#include "lisbench/monitor/capture.hpp"
#include "lisbench/monitor/monitor.hpp"

using namespace lisbench;
using namespace lisbench::monitor;
using namespace std::chrono_literals;

namespace {

bytes frames(std::initializer_list<std::string_view> records) {
    lis01::Message m;
    for (const auto r : records) {
        m.records.push_back(to_bytes(r));
    }
    bytes out;
    for (const auto& f : lis01::frames_for(m)) {
        const auto raw = lis01::encode(f);
        out.insert(out.end(), raw.begin(), raw.end());
    }
    return out;
}

std::size_t count(const std::vector<Event>& ev, Kind k) {
    return static_cast<std::size_t>(
        std::ranges::count_if(ev, [k](const Event& e) { return e.kind == k; }));
}

const bytes enq{lis01::ctl::enq};
const bytes ack{lis01::ctl::ack};
const bytes nak{lis01::ctl::nak};
const bytes eot{lis01::ctl::eot};

}  // namespace

TEST_CASE("monitor: a normal transfer with handshake on both sides") {
    Options o;
    o.watches.push_back(watch::parse("R.value > 100").value());
    Monitor m{o};
    const auto t0 = Clock::time_point{} + 1000h;
    std::vector<Event> ev;
    const auto add = [&ev](std::vector<Event> more) {
        ev.insert(ev.end(), more.begin(), more.end());
    };
    add(m.feed(Side::a, t0, enq));
    add(m.feed(Side::b, t0 + 5ms, ack));
    add(m.feed(Side::a, t0 + 6ms, frames({"H|\\^&", "P|1", "O|1|S1", "R|1|^^^GLU|150", "L|1|N"})));
    add(m.feed(Side::b, t0 + 10ms, ack));
    add(m.feed(Side::a, t0 + 11ms, eot));

    CHECK(count(ev, Kind::record) == 5);
    CHECK(count(ev, Kind::watch) == 1);
    CHECK(count(ev, Kind::transfer) == 1);
    CHECK(m.stats(Side::a).transfers == 1);
    CHECK(m.stats(Side::a).incomplete_transfers == 0);
    CHECK(m.stats(Side::b).max_reply == 5ms);

    // Default view hides handshake and frames, verbose shows them.
    std::size_t shown = 0;
    std::size_t verbose = 0;
    for (const auto& e : ev) {
        shown += format(e, o, {}).size();
        verbose += format(e, o, {.verbose = true}).size();
    }
    CHECK(shown == 7);  // 5 records, 1 watch, 1 transfer
    CHECK(verbose > shown);
}

TEST_CASE("monitor: aborted transfer, NAK and slow reply are reported") {
    Options o;
    o.slow_reply = 100ms;
    Monitor m{o};
    const auto t0 = Clock::time_point{} + 1000h;
    std::vector<Event> ev;
    const auto add = [&ev](std::vector<Event> more) {
        ev.insert(ev.end(), more.begin(), more.end());
    };
    add(m.feed(Side::a, t0, enq));
    add(m.feed(Side::b, t0 + 500ms, ack));  // slow
    add(m.feed(Side::a, t0 + 501ms, frames({"H|\\^&"})));
    add(m.feed(Side::b, t0 + 502ms, nak));
    add(m.feed(Side::a, t0 + 503ms, eot));  // sender gives up: no L record

    CHECK(count(ev, Kind::slow_reply) == 1);
    CHECK(m.stats(Side::b).naks_sent == 1);
    CHECK(m.stats(Side::a).incomplete_transfers == 1);
    const auto transfer =
        std::ranges::find_if(ev, [](const Event& e) { return e.kind == Kind::transfer; });
    REQUIRE(transfer != ev.end());
    CHECK(transfer->text.contains("INCOMPLETE"));
}

TEST_CASE("monitor: ENQ, ACK, EOT is a link check, not an aborted transfer") {
    Monitor m{Options{}};
    const auto t0 = Clock::time_point{} + 1000h;
    std::vector<Event> ev;
    for (auto [side, dt, b] : {std::tuple{Side::a, 0ms, enq}, std::tuple{Side::b, 3ms, ack},
                               std::tuple{Side::a, 4ms, eot}}) {
        const auto more = m.feed(side, t0 + dt, b);
        ev.insert(ev.end(), more.begin(), more.end());
    }
    CHECK(m.stats(Side::a).incomplete_transfers == 0);
    CHECK(m.stats(Side::a).transfers == 0);
    const auto transfer =
        std::ranges::find_if(ev, [](const Event& e) { return e.kind == Kind::transfer; });
    REQUIRE(transfer != ev.end());
    CHECK(transfer->text == "empty transfer, no frames");
}

TEST_CASE("capture lines round-trip") {
    const CaptureLine line{.at = Clock::time_point{} + 1'761'350'000'123'456us,
                           .from = Side::b,
                           .data = to_bytes("\x02"
                                            "1L|1\r\x03"
                                            "2A\r\n")};
    const auto text = to_capture_line(line);
    const auto back = parse_capture_line(text);
    REQUIRE(back);
    CHECK(back->at == line.at);
    CHECK(back->from == Side::b);
    CHECK(back->data == line.data);
    CHECK_FALSE(parse_capture_line("{}"));
    CHECK_FALSE(parse_capture_line(R"({"t":"x","from":"a","hex":"00"})"));
    CHECK_FALSE(parse_capture_line(R"({"t":"2026-10-25T01:00:00Z","from":"c","hex":"00"})"));
    CHECK_FALSE(parse_capture_line(R"({"t":"2026-10-25T01:00:00Z","from":"a","hex":"0"})"));
}

TEST_CASE("replay through capture lines gives the same output as the live feed") {
    Options o;
    o.watches.push_back(watch::parse("R.value > 1").value());
    const auto t0 = Clock::time_point{} + 1'761'350'000s;
    const std::vector<CaptureLine> session{
        {.at = t0, .from = Side::a, .data = enq},
        {.at = t0 + 1ms, .from = Side::b, .data = ack},
        {.at = t0 + 2ms,
         .from = Side::a,
         .data = frames({"H|\\^&", "P|1", "O|1|S1", "R|1|^^^A|5", "L|1|N"})},
        {.at = t0 + 3ms, .from = Side::b, .data = nak},
        {.at = t0 + 4ms, .from = Side::a, .data = eot},
    };
    const auto render = [&o](Monitor& m, const std::vector<CaptureLine>& lines) {
        std::vector<std::string> out;
        for (const auto& l : lines) {
            for (const auto& e : m.feed(l.from, l.at, l.data)) {
                for (auto& s : format(e, o, {.verbose = true, .decode = true})) {
                    out.push_back(std::move(s));
                }
            }
        }
        return out;
    };
    Monitor live{o};
    Monitor replayed{o};
    std::vector<CaptureLine> read_back;
    for (const auto& l : session) {
        read_back.push_back(parse_capture_line(to_capture_line(l)).value());
    }
    const auto a = render(live, session);
    const auto b = render(replayed, read_back);
    CHECK(a.size() > 10);
    CHECK(a == b);
}

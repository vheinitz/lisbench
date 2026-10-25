#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

#include "lisbench/io/command.hpp"
#include "lisbench/lis01_frame.hpp"
#include "lisbench/monitor/capture.hpp"
#include "lisbench/monitor/monitor.hpp"
#include "lisbench/monitor/trigger.hpp"

using namespace lisbench;
using namespace std::chrono_literals;

TEST_CASE("trigger: watch => action") {
    const auto exec = monitor::parse_trigger("R.value > 100 => exec notify-send \"high\"");
    REQUIRE(exec);
    CHECK(exec->watch.text == "R.value > 100");
    REQUIRE(std::holds_alternative<monitor::Exec>(exec->action));
    CHECK(std::get<monitor::Exec>(exec->action).command == "notify-send \"high\"");

    const auto mark = monitor::parse_trigger("R.abnormal_flags == H => mark high value");
    REQUIRE(mark);
    CHECK(std::get<monitor::Mark>(mark->action).text == "high value");
    const auto bare_mark = monitor::parse_trigger("R.abnormal_flags == H => mark");
    REQUIRE(bare_mark);
    CHECK(std::get<monitor::Mark>(bare_mark->action).text == "R.abnormal_flags == H");

    const auto stop = monitor::parse_trigger("O.specimen_id == S-1 =>stop");
    REQUIRE(stop);
    CHECK(std::holds_alternative<monitor::Stop>(stop->action));

    CHECK_FALSE(monitor::parse_trigger("R.value > 100"));              // no action
    CHECK_FALSE(monitor::parse_trigger("R.value > 100 => beep"));      // unknown action
    CHECK_FALSE(monitor::parse_trigger("R.value > 100 => exec"));      // no command
    CHECK_FALSE(monitor::parse_trigger("R.value >> 100 => stop"));     // bad watch
    CHECK_FALSE(monitor::parse_trigger("R.value > 100 => stop now"));  // stop takes nothing
}

TEST_CASE("monitor: a watch event names its watch and the value") {
    monitor::Options o;
    o.watches.push_back(watch::parse("R.value > 100").value());
    o.watches.push_back(watch::parse("R.abnormal_flags == H").value());
    monitor::Monitor m{o};
    lis01::Message msg;
    for (const auto r : {"H|\\^&", "P|1", "O|1|S1", "R|1|^^^GLU|150|mg/dL||H", "L|1|N"}) {
        msg.records.push_back(to_bytes(r));
    }
    bytes wire{lis01::ctl::enq};
    for (const auto& f : lis01::frames_for(msg)) {
        const auto raw = lis01::encode(f);
        wire.insert(wire.end(), raw.begin(), raw.end());
    }
    wire.push_back(lis01::ctl::eot);
    std::vector<std::pair<std::size_t, std::string>> hits;
    for (const auto& e : m.feed(io::Side::a, monitor::Clock::now(), wire)) {
        if (e.kind == monitor::Kind::watch) {
            REQUIRE(e.watch);
            hits.emplace_back(*e.watch, e.value);
        }
    }
    REQUIRE(hits.size() == 2);
    CHECK(hits[0] == std::pair<std::size_t, std::string>{0, "150"});
    CHECK(hits[1] == std::pair<std::size_t, std::string>{1, "H"});
}

TEST_CASE("capture: marks round-trip and are told apart from data") {
    const auto at = monitor::Clock::time_point{} + 1000h;
    const auto text = monitor::to_capture_line({.at = at, .from = io::Side::b, .mark = "high"});
    const auto line = monitor::parse_capture_line(text);
    REQUIRE(line);
    CHECK(line->mark == "high");
    CHECK(line->data.empty());
    CHECK(line->from == io::Side::b);
    const auto data = monitor::parse_capture_line(
        monitor::to_capture_line({.at = at, .from = io::Side::a, .data = {std::byte{5}}}));
    REQUIRE(data);
    CHECK(data->mark.empty());
    CHECK(data->data == bytes{std::byte{5}});
}

#if !defined(_WIN32)
TEST_CASE("command queue: runs commands in order with the given environment") {
    const auto dir = std::filesystem::temp_directory_path() / "lisbench_trigger_test";
    std::filesystem::create_directories(dir);
    const auto out = dir / "out.txt";
    std::filesystem::remove(out);

    std::vector<io::CommandResult> results;
    std::mutex m;
    {
        io::CommandQueue q{4, [&](const io::CommandResult& r) {
                               const std::scoped_lock lock{m};
                               results.push_back(r);
                           }};
        // The value contains shell syntax; it must arrive as data.
        CHECK(q.submit({.command = "printf '%s|' \"$LISBENCH_VALUE\" >> '" + out.string() + "'",
                        .env = {{"LISBENCH_VALUE", "1; echo injected"}}}));
        CHECK(q.submit({.command = "printf '%s' \"$LISBENCH_VALUE\" >> '" + out.string() + "'",
                        .env = {{"LISBENCH_VALUE", "second"}}}));
        CHECK(q.submit({.command = "exit 3", .env = {}}));
        q.drain();
    }
    std::ifstream in(out);
    const std::string got{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(got == "1; echo injected|second");
    REQUIRE(results.size() == 3);
    CHECK(results[0].exit_code == 0);
    CHECK(results[2].exit_code == 3);
    std::filesystem::remove_all(dir);
}

TEST_CASE("command queue: a full queue refuses instead of blocking") {
    std::atomic<int> done{0};
    io::CommandQueue q{1, [&](const io::CommandResult&) { ++done; }};
    CHECK(q.submit({.command = "sleep 0.3", .env = {}}));
    std::this_thread::sleep_for(50ms);  // the worker has taken the first one
    CHECK(q.submit({.command = "true", .env = {}}));
    CHECK_FALSE(q.submit({.command = "true", .env = {}}));  // capacity 1 is taken
    q.drain();
    CHECK(done == 2);
}
#endif

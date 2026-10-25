#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "lisbench/sim/scenario.hpp"

using namespace lisbench;
using namespace lisbench::sim;
using namespace std::chrono_literals;

TEST_CASE("durations") {
    CHECK(parse_duration("500ms").value() == 500ms);
    CHECK(parse_duration("15s").value() == 15s);
    CHECK(parse_duration("2m").value() == 2min);
    CHECK(parse_duration("3").value() == 3s);
    CHECK_FALSE(parse_duration("fast"));
    CHECK_FALSE(parse_duration("5h"));
    CHECK_FALSE(parse_duration("-1s"));
}

TEST_CASE("a full scenario is parsed") {
    const auto s = parse_scenario(R"(
name: upload with faults
role: instrument
endpoint: tcp://127.0.0.1:15000
timing: {reply_timeout: 2s, receive_timeout: 3s, max_frame_attempts: 3}
faults:
  - {on: ack, nth: 2, do: nak}
  - {on: frame, every: 5, do: corrupt}
  - {on: eot, do: delay, ms: 100}
answer_queries: valid_order_download.astm
steps:
  - send: valid_result_upload.astm
  - send: {records: ["H|\\^&", "L|1|N"]}
    expect: rejected
  - expect: {within: 5s, records: 7, contains: ["O|1|"], valid: true}
  - wait: 250ms
)",
                                  LISBENCH_CORPUS_DIR);
    REQUIRE(s);
    CHECK(s->name == "upload with faults");
    CHECK(s->timing.reply_timeout == 2s);
    CHECK(s->timing.receive_timeout == 3s);
    CHECK(s->timing.max_frame_attempts == 3);
    REQUIRE(s->faults.size() == 3);
    CHECK(s->faults[1].every == 5);
    CHECK(s->faults[2].delay == 100ms);
    REQUIRE(s->answer_queries);
    CHECK(s->answer_queries->size() == 7);
    REQUIRE(s->steps.size() == 4);
    CHECK(std::get<SendStep>(s->steps[0]).records.size() == 7);
    CHECK(std::get<SendStep>(s->steps[1]).expect == lis01::Outcome::rejected);
    const auto& x = std::get<ExpectStep>(s->steps[2]);
    CHECK(x.within == 5s);
    CHECK(x.records == 7U);
    CHECK(x.valid);
    CHECK(std::get<WaitStep>(s->steps[3]).duration == 250ms);
}

TEST_CASE("scenario errors name the line") {
    const auto check_error = [](std::string_view yaml, std::string_view part) {
        const auto s = parse_scenario(yaml, ".");
        REQUIRE_FALSE(s);
        INFO(s.error());
        CHECK(s.error().contains(part));
    };
    check_error("role: instrument\n", "endpoint");
    check_error("endpoint: tcp://h:1\nrole: robot\n", "line 2");
    check_error("endpoint: tcp://h:1\nsteps:\n  - send: no_such_file.astm\n", "line 3");
    check_error("endpoint: tcp://h:1\nfaults:\n  - {on: frame, do: nak}\n", "only applies");
    check_error("endpoint: tcp://h:1\nsteps:\n  - wait: soon\n", "duration");
    check_error("endpoint: [unclosed\n", "line");
}

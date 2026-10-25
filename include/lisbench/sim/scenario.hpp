#pragma once

// Simulator scenarios: an instrument or a LIS that connects (or listens),
// sends messages, waits for transfers and checks them, optionally with
// faults on the wire. Written as YAML:
//
//   name: results upload with one refused frame
//   role: instrument                    # or computer (the LIS side)
//   endpoint: tcp://127.0.0.1:15000     # see io/endpoint.hpp
//   timing: {reply_timeout: 15s, receive_timeout: 30s, busy_retry: 10s,
//            max_frame_attempts: 6}
//   faults:
//     - {on: ack, nth: 2, do: nak}      # targets ack nak enq eot frame
//     - {on: frame, every: 5, do: corrupt}
//     - {on: ack, nth: 3, do: delay, ms: 20000}
//   answer_queries: orders.astm         # reply to every transfer with a Q record
//   serve: false                        # listen: accept the next connection afterwards
//   steps:
//     - send: results.astm              # file relative to the scenario
//       expect: delivered               # outcome, default delivered
//     - send: {records: ["H|\\^&", "L|1|N"]}
//     - expect: {within: 30s, records: 3, contains: ["O|1|"], valid: true}
//     - wait: 2s
//
// Without steps the simulator stays connected until the peer disconnects.

#include <chrono>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lisbench/io/endpoint.hpp"
#include "lisbench/io/faults.hpp"
#include "lisbench/lis01_link.hpp"

namespace lisbench::sim {

using std::chrono::milliseconds;

struct SendStep {
    std::vector<std::string> records{};  // wire text, without CR
    std::string source{};                // file name or "inline", for reports
    lis01::Outcome expect{lis01::Outcome::delivered};
};

struct ExpectStep {
    milliseconds within{30'000};
    std::optional<std::size_t> records{};
    std::vector<std::string> contains{};
    bool valid{false};  // LIS02-A2 validation must find no error
    bool complete{true};
};

struct WaitStep {
    milliseconds duration{0};
};

using Step = std::variant<SendStep, ExpectStep, WaitStep>;

struct Scenario {
    std::string name{};
    lis01::Role role{lis01::Role::instrument};
    io::Endpoint endpoint{};
    lis01::Timing timing{};
    std::vector<io::Fault> faults{};
    std::optional<std::vector<std::string>> answer_queries{};
    bool serve{false};
    std::vector<Step> steps{};
};

[[nodiscard]] std::expected<milliseconds, std::string> parse_duration(std::string_view text);

// Records of a message file: wire text separated by CR, LF or CR LF.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> read_records(
    const std::filesystem::path& file);

// Relative file names are resolved against base_dir.
[[nodiscard]] std::expected<Scenario, std::string> parse_scenario(
    std::string_view yaml, const std::filesystem::path& base_dir);
[[nodiscard]] std::expected<Scenario, std::string> load_scenario(const std::filesystem::path& file);

}  // namespace lisbench::sim

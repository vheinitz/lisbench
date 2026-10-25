#pragma once

// The tester of the conformance suite: plays the other role towards the IUT
// on an open stream and runs the test cases of suite.hpp. It works on bytes,
// not through lis01::Link, because it has to break the rules on purpose
// (wrong checksums, wrong frame numbers, no reply).

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/conform/suite.hpp"
#include "lisbench/io/session.hpp"
#include "lisbench/lis01_link.hpp"

namespace lisbench::conform {

// The role of the implementation under test.
enum class Iut : std::uint8_t { computer, instrument };

struct Options {
    Iut iut{Iut::computer};
    lis01::Timing timing{};                          // nominal values the IUT has to keep
    std::vector<std::string> only{};                 // test case ids; empty: all
    std::chrono::milliseconds wait_for_iut{60'000};  // TS: how long to wait for the IUT to send
    // PICS (from the instrument profile): does the IUT stop on a receiver interrupt?
    std::optional<bool> honours_receiver_interrupt{};
};

using Progress = std::function<void(std::string_view)>;

// Runs the selected test cases and returns their results in suite order.
asio::awaitable<std::vector<Result>> run(io::Stream stream, Options options, Progress progress);

}  // namespace lisbench::conform

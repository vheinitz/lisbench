#pragma once

// Capture files: one JSON object per line,
//   {"t":"2026-10-25T00:53:16.704123Z","from":"a","hex":"0231..."}
// so that a recorded session can be replayed into the monitor offline.

#include <chrono>
#include <expected>
#include <string>
#include <string_view>

#include "lisbench/io/relay.hpp"

namespace lisbench::monitor {

struct CaptureLine {
    std::chrono::system_clock::time_point at{};
    io::Side from{io::Side::a};
    bytes data{};
};

[[nodiscard]] std::string to_capture_line(const CaptureLine& line);
[[nodiscard]] std::expected<CaptureLine, std::string> parse_capture_line(std::string_view text);

}  // namespace lisbench::monitor

#pragma once

// Two streams joined: everything from A goes to B and back, unchanged, and
// every chunk is reported. With forward = false nothing is passed on (a
// passive tap listening on two receive lines).

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "lisbench/io/session.hpp"

namespace lisbench::io {

enum class Side : std::uint8_t { a, b };

struct RelayHandlers {
    std::function<void(Side from, std::chrono::system_clock::time_point at, bytes_view data)>
        on_data;
    std::function<void(Side closed, const std::string& reason)> on_closed;
};

// Returns when either side is closed; the other side is closed as well.
asio::awaitable<void> relay(Stream a, Stream b, RelayHandlers handlers, bool forward = true);

// Reads one stream until it closes (recording an instrument's output).
asio::awaitable<void> drain(Stream s, RelayHandlers handlers);

}  // namespace lisbench::io

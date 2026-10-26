#pragma once

// Where a link goes and how it carries messages, written as a URI:
//   tcp://host:port                  connect
//   tcp-listen://[host]:port         accept one connection at a time
//   serial:/dev/ttyUSB0?baud=9600&data=8&parity=none&stop=1&flow=none
//   serial:COM3?baud=19200&parity=even&data=7
//   tcp://host:port?low-level=mllp   without LIS01-A2 (none, mllp; see unframed.hpp)
// Serial defaults: 9600 baud, 8 data bits, no parity, 1 stop bit, no flow
// control (8N1). The low-level protocol defaults to LIS01-A2 everywhere.

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <variant>

#include "lisbench/unframed.hpp"

namespace lisbench::io {

struct Tcp {
    std::string host{};
    std::uint16_t port{0};
    bool listen{false};
    LowLevel low_level{LowLevel::lis01_a2};

    friend bool operator==(const Tcp&, const Tcp&) = default;
};

enum class Parity : std::uint8_t { none, odd, even };
enum class StopBits : std::uint8_t { one, one_half, two };
enum class FlowControl : std::uint8_t { none, software, hardware };

struct Serial {
    std::string device{};
    unsigned baud{9600};
    unsigned data_bits{8};
    Parity parity{Parity::none};
    StopBits stop_bits{StopBits::one};
    FlowControl flow{FlowControl::none};
    LowLevel low_level{LowLevel::lis01_a2};

    friend bool operator==(const Serial&, const Serial&) = default;
};

using Endpoint = std::variant<Tcp, Serial>;

[[nodiscard]] std::expected<Endpoint, std::string> parse_endpoint(std::string_view uri);
[[nodiscard]] std::string to_string(const Endpoint& endpoint);
[[nodiscard]] LowLevel low_level_of(const Endpoint& endpoint) noexcept;

}  // namespace lisbench::io

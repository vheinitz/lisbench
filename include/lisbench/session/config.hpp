#pragma once

// Session files (docs/spec/features.md, 9.3): one named configuration of a
// session type. Typed and checked; URIs are derived, never written.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "lisbench/io/endpoint.hpp"

namespace lisbench::session {

using std::chrono::milliseconds;

// gateway: forwards whole messages between an instrument side and a LIS
// side, each with its own transport and low-level protocol (9.10).
enum class SessionType : std::uint8_t { lis, device, gateway };
enum class Transport : std::uint8_t { serial, tcp_server, tcp_client };

[[nodiscard]] std::string_view to_string(SessionType t) noexcept;
[[nodiscard]] std::string_view to_string(Transport t) noexcept;

struct LinkConfig {
    std::string role{"main"};  // main, or orders / results (LIS with two ports); gateway:
                               // instrument and lis
    Transport transport{Transport::tcp_server};
    LowLevel low_level{LowLevel::lis01_a2};
    io::Serial serial{};          // its low_level is not used: see above
    std::string host{"0.0.0.0"};  // tcp-server: address to listen on; tcp-client: partner
    std::uint16_t port{15000};

    [[nodiscard]] io::Endpoint endpoint() const;
    // For the UI: "serial /dev/ttyUSB0 9600 8N1", "waits on port 15000", ...
    [[nodiscard]] std::string label() const;
    friend bool operator==(const LinkConfig&, const LinkConfig&) = default;

private:
    [[nodiscard]] std::string transport_label() const;
};

enum class Unsolicited : std::uint8_t { accept, flag, ignore };
enum class NoOrderReply : std::uint8_t { y_record, empty };

struct LisConfig {
    Unsolicited unsolicited{Unsolicited::flag};
    bool answer_queries{true};
    NoOrderReply no_order_reply{NoOrderReply::y_record};
    friend bool operator==(const LisConfig&, const LisConfig&) = default;
};

enum class DeviceMode : std::uint8_t { unsolicited, host_query };

struct DeviceConfig {
    DeviceMode mode{DeviceMode::unsolicited};
    std::string prefix{"S-"};  // specimen IDs from a counter ...
    unsigned start{1};
    std::vector<std::string> specimens{};  // ... or this list, if not empty
    std::vector<std::string> tests{};      // unsolicited: reported per specimen
    milliseconds interval{30'000};
    milliseconds query_timeout{60'000};
    milliseconds measure_time{2'000};
    friend bool operator==(const DeviceConfig&, const DeviceConfig&) = default;
};

struct SessionConfig {
    std::string name{};
    SessionType type{SessionType::lis};
    std::string profile{};
    bool fast_timing{false};
    std::vector<LinkConfig> links{};
    LisConfig lis{};
    DeviceConfig device{};
    friend bool operator==(const SessionConfig&, const SessionConfig&) = default;
};

[[nodiscard]] bool valid_session_name(std::string_view name) noexcept;

// YAML (also JSON, which is YAML). Errors name the line.
[[nodiscard]] std::expected<SessionConfig, std::string> parse_session(std::string_view yaml);
[[nodiscard]] std::expected<SessionConfig, std::string> load_session(
    const std::filesystem::path& file);
[[nodiscard]] std::string to_yaml(const SessionConfig& config);
// The same as typed JSON, for the web UI's forms; parse_session reads it back.
[[nodiscard]] nlohmann::json config_json(const SessionConfig& config);

}  // namespace lisbench::session

#pragma once

#include <asio.hpp>

#include <string>
#include <vector>

#include "common.hpp"
#include "lisbench/io/session.hpp"

namespace lisbench::cli {

struct MonitorFlags {
    std::string a;
    std::string b;
    std::string replay;
    std::string labels{"A,B"};
    std::vector<std::string> watches;
    std::vector<std::string> triggers;
    std::string save;
    bool tap{false};
    bool decode{false};
    bool verbose{false};
    long slow_reply_ms{15'000};
    TextFlags text;
};

int cmd_monitor(const MonitorFlags& flags);

// Listens (tcp-listen) or connects/opens, and says so on stdout.
asio::awaitable<io::Stream> open_endpoint(io::Endpoint endpoint, std::string label);

struct BridgeFlags {
    std::string a;
    std::string b;
    bool serve{false};
    std::string save;
};

struct RecordFlags {
    std::string from;
    std::string to;   // capture file (JSON lines)
    std::string raw;  // raw bytes
    bool serve{false};
};

int cmd_bridge(const BridgeFlags& flags);
int cmd_record(const RecordFlags& flags);

}  // namespace lisbench::cli

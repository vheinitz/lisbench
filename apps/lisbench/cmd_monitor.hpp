#pragma once

#include <string>
#include <vector>

#include "common.hpp"

namespace lisbench::cli {

struct MonitorFlags {
    std::string a;
    std::string b;
    std::string replay;
    std::string labels{"A,B"};
    std::vector<std::string> watches;
    std::string save;
    bool tap{false};
    bool decode{false};
    bool verbose{false};
    long slow_reply_ms{15'000};
    TextFlags text;
};

int cmd_monitor(const MonitorFlags& flags);

}  // namespace lisbench::cli

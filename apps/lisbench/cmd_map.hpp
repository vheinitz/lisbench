#pragma once

#include <string>
#include <vector>

namespace lisbench::cli {

struct MapFlags {
    std::string mapping;
    std::string db;
    std::vector<std::string> args;
    std::vector<std::string> cfg;
    std::string each;
    std::string to;
    std::string role{"instrument"};
    bool keep_going{false};
};

int cmd_map(const MapFlags& flags);

}  // namespace lisbench::cli

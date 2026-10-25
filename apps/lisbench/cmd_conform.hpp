#pragma once

#include <string>
#include <vector>

namespace lisbench::cli {

struct ConformFlags {
    std::string iut;       // computer or instrument
    std::string endpoint;  // where the IUT is
    std::vector<std::string> only;
    std::string profile;  // PICS and timing details
    bool fast_timing{false};
    int wait_s{60};
    std::string json;  // write the results here as well
};

int cmd_conform(const ConformFlags& flags);

}  // namespace lisbench::cli

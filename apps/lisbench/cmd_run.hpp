#pragma once

#include <string>
#include <vector>

namespace lisbench::cli {

struct RunFlags {
    std::vector<std::string> files;  // <data>/sessions/<name>.yaml
};

int cmd_run(const RunFlags& flags);

}  // namespace lisbench::cli

#pragma once

#include <string>

namespace lisbench::cli {

struct ServeFlags {
    std::string http{"127.0.0.1:8080"};
    std::string captures;  // data directory: captures, message files, scenarios
};

int cmd_serve(const ServeFlags& flags);

}  // namespace lisbench::cli

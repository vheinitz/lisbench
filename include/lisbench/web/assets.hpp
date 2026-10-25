#pragma once

// The web UI's files (web/), compiled into the binary by cmake/embed.cmake.

#include <span>
#include <string_view>

namespace lisbench::web {

struct Asset {
    std::string_view path;  // as served, e.g. "/index.html"
    std::string_view content_type;
    std::string_view data;
};

[[nodiscard]] std::span<const Asset> assets() noexcept;

}  // namespace lisbench::web

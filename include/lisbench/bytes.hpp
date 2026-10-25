#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lisbench {

using bytes = std::vector<std::byte>;
using bytes_view = std::span<const std::byte>;

[[nodiscard]] inline bytes to_bytes(std::string_view text) {
    bytes out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

[[nodiscard]] inline std::string to_string(bytes_view data) {
    std::string out(data.size(), '\0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[i] = static_cast<char>(std::to_integer<unsigned char>(data[i]));
    }
    return out;
}

}  // namespace lisbench

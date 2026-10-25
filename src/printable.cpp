#include "lisbench/printable.hpp"

#include <array>
#include <format>
#include <string_view>

namespace lisbench {

namespace {

constexpr std::array<std::string_view, 32> control_names{
    "NUL", "SOH", "STX", "ETX", "EOT", "ENQ", "ACK", "BEL", "BS",  "HT",  "LF",
    "VT",  "FF",  "CR",  "SO",  "SI",  "DLE", "DC1", "DC2", "DC3", "DC4", "NAK",
    "SYN", "ETB", "CAN", "EM",  "SUB", "ESC", "FS",  "GS",  "RS",  "US",
};

}  // namespace

std::string printable(bytes_view data) {
    std::string out;
    out.reserve(data.size());
    for (const auto b : data) {
        const auto c = std::to_integer<unsigned char>(b);
        if (c < control_names.size()) {
            out += std::format("<{}>", control_names.at(c));
        } else if (c >= 0x7F) {
            out += std::format("<{:02X}>", c);
        } else if (c == '<') {
            out += "<<";  // keeps the rendering unambiguous
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

}  // namespace lisbench

#include "lisbench/monitor/capture.hpp"

#include <nlohmann/json.hpp>

#include <format>

#include "lisbench/compat.hpp"

namespace lisbench::monitor {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string to_capture_line(const CaptureLine& line) {
    std::string hex;
    hex.reserve(line.data.size() * 2);
    for (const auto b : line.data) {
        hex += std::format("{:02x}", std::to_integer<unsigned>(b));
    }
    const auto t = std::chrono::floor<std::chrono::microseconds>(line.at);
    const nlohmann::json j{{"t", std::format("{:%FT%T}Z", t)},
                           {"from", line.from == io::Side::a ? "a" : "b"},
                           {"hex", hex}};
    return j.dump();
}

std::expected<CaptureLine, std::string> parse_capture_line(std::string_view text) {
    const auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("t") || !j.contains("from") ||
        !j.contains("hex")) {
        return std::unexpected("not a capture line");
    }
    CaptureLine line;
    const auto from = j["from"].get<std::string>();
    if (from != "a" && from != "b") {
        return std::unexpected("from must be a or b");
    }
    line.from = from == "a" ? io::Side::a : io::Side::b;

    const auto t = compat::parse_utc(j["t"].get<std::string>());
    if (!t) {
        return std::unexpected("bad time stamp");
    }
    line.at = *t;

    const auto hex = j["hex"].get<std::string>();
    if (hex.size() % 2 != 0) {
        return std::unexpected("odd number of hex digits");
    }
    line.data.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hex_value(hex[i]);
        const int lo = hex_value(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return std::unexpected("bad hex digit");
        }
        line.data.push_back(static_cast<std::byte>((hi << 4) | lo));
    }
    return line;
}

}  // namespace lisbench::monitor

#pragma once

// The shared test catalogue (docs/spec/features.md, 9.4): what tests exist,
// how their results look, and how an instrument simulator produces them.

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lisbench::session {

struct TestDef {
    enum class Kind : std::uint8_t { numeric, qualitative };

    std::string code{};  // 4th component of the universal test ID
    std::string name{};
    Kind kind{Kind::numeric};
    std::string unit{};
    std::optional<double> low{};  // reference range
    std::optional<double> high{};
    int decimals{0};
    std::optional<double> simulate_low{};  // instrument simulator: value range
    std::optional<double> simulate_high{};
    std::vector<std::string> values{};    // qualitative: possible results
    std::vector<std::string> abnormal{};  // qualitative: these get flag A

    friend bool operator==(const TestDef&, const TestDef&) = default;
};

struct Catalog {
    std::vector<TestDef> tests{};

    [[nodiscard]] const TestDef* find(std::string_view code) const noexcept;
    friend bool operator==(const Catalog&, const Catalog&) = default;
};

// Checked: types, unknown keys, duplicate codes, ranges; errors name the line.
[[nodiscard]] std::expected<Catalog, std::string> parse_catalog(std::string_view yaml);
[[nodiscard]] std::expected<Catalog, std::string> load_catalog(const std::filesystem::path& file);
[[nodiscard]] std::string to_yaml(const Catalog& catalog);
// As JSON for the web UI; parse_catalog reads it back (JSON is YAML).
[[nodiscard]] nlohmann::json catalog_json(const Catalog& catalog);

// R-7 abnormal flag for a result value: L, H or N for numbers with a
// reference range, A or N for qualitative results; empty when unknown.
[[nodiscard]] std::string abnormal_flag(const TestDef& test, std::string_view value);

}  // namespace lisbench::session

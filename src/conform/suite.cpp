#include "lisbench/conform/suite.hpp"

#include <algorithm>
#include <format>

namespace lisbench::conform {

std::string_view to_string(Verdict v) noexcept {
    switch (v) {
        case Verdict::pass: return "pass";
        case Verdict::fail: return "FAIL";
        case Verdict::inconclusive: return "inconclusive";
    }
    return "?";
}

std::string_view to_string(Level l) noexcept {
    return l == Level::shall ? "shall" : "should";
}

const TestCase* find(std::string_view id) noexcept {
    const auto it = std::ranges::find(suite, id, &TestCase::id);
    return it == suite.end() ? nullptr : &*it;
}

std::vector<std::string> report(const std::vector<Result>& results) {
    std::vector<std::string> out;
    std::array<int, 3> counts{};
    for (const auto& r : results) {
        const auto* tc = find(r.id);
        out.push_back(std::format("{:<5} {:<12} {:<6} {}", r.id, to_string(r.verdict),
                                  tc != nullptr ? to_string(tc->level) : "",
                                  tc != nullptr ? tc->purpose : ""));
        if (!r.detail.empty()) {
            out.push_back(std::format("{:25}{}", "", r.detail));
        }
        ++counts.at(static_cast<std::size_t>(r.verdict));
    }
    out.push_back(
        std::format("{} pass, {} fail, {} inconclusive", counts[0], counts[1], counts[2]));
    return out;
}

bool within(std::chrono::milliseconds measured, std::chrono::milliseconds nominal,
            std::optional<double> upper) noexcept {
    const auto m = static_cast<double>(measured.count());
    const auto n = static_cast<double>(nominal.count());
    return m >= 0.95 * n && (!upper || m <= *upper * n);
}

}  // namespace lisbench::conform

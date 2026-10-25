#pragma once

// Stand-ins for C++23 library features that GCC 13 does not ship yet. Each
// one is used through this header only, so dropping GCC 13 means replacing
// the call sites mechanically and deleting this file.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace lisbench::compat {

// std::ranges::to<std::vector>() (GCC 14).
template <std::ranges::input_range R>
[[nodiscard]] auto to_vector(R&& range) {
    std::vector<std::ranges::range_value_t<R>> out{};
    if constexpr (std::ranges::sized_range<R>) {
        out.reserve(std::ranges::size(range));
    }
    std::ranges::copy(std::forward<R>(range), std::back_inserter(out));
    return out;
}

// std::chrono::parse("%FT%TZ", ...) (GCC 14) for "2026-10-25T00:53:16.704123Z";
// the fraction may have 0..6 digits.
[[nodiscard]] inline std::optional<std::chrono::sys_time<std::chrono::microseconds>> parse_utc(
    std::string_view s) {
    const auto number = [&s](std::size_t pos, std::size_t len) -> std::optional<int> {
        if (pos + len > s.size()) {
            return std::nullopt;
        }
        const auto part = s.substr(pos, len);
        const auto* const end = std::to_address(part.end());
        int v = 0;
        const auto [ptr, ec] = std::from_chars(std::to_address(part.begin()), end, v);
        if (ec != std::errc{} || ptr != end) {
            return std::nullopt;
        }
        return v;
    };
    if (s.size() < 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':' || s.back() != 'Z') {
        return std::nullopt;
    }
    const auto y = number(0, 4);
    const auto mo = number(5, 2);
    const auto d = number(8, 2);
    const auto h = number(11, 2);
    const auto mi = number(14, 2);
    const auto sec = number(17, 2);
    if (!y || !mo || !d || !h || !mi || !sec || *h > 23 || *mi > 59 || *sec > 60) {
        return std::nullopt;
    }
    int micros = 0;
    if (s.size() > 20) {
        if (s[19] != '.' || s.size() - 21 > 6 || s.size() < 22) {
            return std::nullopt;
        }
        const auto digits = s.size() - 21;
        const auto frac = number(20, digits);
        if (!frac) {
            return std::nullopt;
        }
        micros = *frac;
        for (auto i = digits; i < 6; ++i) {
            micros *= 10;
        }
    }
    const std::chrono::year_month_day date{std::chrono::year{*y},
                                           std::chrono::month{static_cast<unsigned>(*mo)},
                                           std::chrono::day{static_cast<unsigned>(*d)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*h} + std::chrono::minutes{*mi} +
           std::chrono::seconds{*sec} + std::chrono::microseconds{micros};
}

}  // namespace lisbench::compat

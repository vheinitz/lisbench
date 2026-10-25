#pragma once

// Stand-ins for C++23 library features that GCC 13 does not ship yet. Each
// one is used through this header only, so dropping GCC 13 means replacing
// the call sites mechanically and deleting this file.

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>
#include <vector>

namespace lisbench::compat {

// std::ranges::to<std::vector>() (GCC 14).
template <std::ranges::input_range R>
[[nodiscard]] auto to_vector(R&& range) {
    std::vector<std::ranges::range_value_t<R>> out;
    if constexpr (std::ranges::sized_range<R>) {
        out.reserve(std::ranges::size(range));
    }
    std::ranges::copy(std::forward<R>(range), std::back_inserter(out));
    return out;
}

}  // namespace lisbench::compat

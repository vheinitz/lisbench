#pragma once

// Watch expressions on LIS02-A2 fields, for the monitor:
//
//   R.value > 100                      field by name (see lis02_records)
//   R.4 >= 5.5                         field by number
//   O.universal_test_id.4 == TSH       component (1-based)
//   P.patient_name contains "Doe"
//   R.abnormal_flags matches "^(H|HH)$"
//
// Operators: == != < > <= >= contains matches. Values may be quoted. If both
// sides are numbers they are compared as numbers, otherwise as text; but
// < > <= >= against a number never match a value that is not a number. Without
// a component the whole field is used, components joined by the component
// delimiter.

#include <cstdint>
#include <expected>
#include <optional>
#include <regex>
#include <string>
#include <string_view>

#include "lisbench/lis02_message.hpp"

namespace lisbench::watch {

enum class Op : std::uint8_t { eq, ne, lt, gt, le, ge, contains, matches };

struct Expression {
    std::string text{};
    char record_type{'R'};
    std::size_t field{0};
    std::optional<std::size_t> component{};
    Op op{Op::eq};
    std::string value{};
    std::optional<std::regex> regex{};
};

[[nodiscard]] std::expected<Expression, std::string> parse(std::string_view text);

// The value the expression looks at (empty if the record has no such field).
[[nodiscard]] std::string value_of(const Expression& e, const lis02::Record& r,
                                   const lis02::Delimiters& d);

[[nodiscard]] bool matches(const Expression& e, const lis02::Record& r, const lis02::Delimiters& d);

}  // namespace lisbench::watch

#include "lisbench/watch.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <compare>
#include <format>
#include <utility>

namespace lisbench::watch {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::optional<std::size_t> to_size(std::string_view s) {
    std::size_t v = 0;
    const auto* end = s.data() + s.size();
    // from_chars is bounded by end; no terminator is needed.
    const auto [ptr, ec] =
        std::from_chars(s.data(), end, v);  // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return v;
}

std::optional<double> to_number(std::string_view s) {
    s = trim(s);
    double v = 0;
    const auto* end = s.data() + s.size();
    // from_chars is bounded by end; no terminator is needed.
    const auto [ptr, ec] =
        std::from_chars(s.data(), end, v);  // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (s.empty() || ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return v;
}

// Longest operators first, so that "<=" is not read as "<".
constexpr std::array<std::pair<std::string_view, Op>, 8> operators{{
    {"contains", Op::contains},
    {"matches", Op::matches},
    {"==", Op::eq},
    {"!=", Op::ne},
    {"<=", Op::le},
    {">=", Op::ge},
    {"<", Op::lt},
    {">", Op::gt},
}};

}  // namespace

std::expected<Expression, std::string> parse(std::string_view text) {
    Expression e{.text = std::string{trim(text)}};
    const std::string_view t = e.text;
    const auto space = t.find_first_of(" \t=!<>");
    if (space == std::string_view::npos) {
        return std::unexpected(
            std::format("'{}': expected <record>.<field> <operator> <value>", t));
    }
    const auto path = t.substr(0, space);
    auto rest = trim(t.substr(space));

    // Path: TYPE.field[.component]
    const auto dot = path.find('.');
    if (dot != 1) {
        return std::unexpected(
            std::format("'{}': path must start with a record type, e.g. R.value", path));
    }
    const auto kind = lis02::record_type(path.front());
    if (!kind) {
        return std::unexpected(std::format("'{}': unknown record type", path.substr(0, 1)));
    }
    e.record_type = path.front();
    auto field_text = path.substr(2);
    if (const auto second = field_text.find('.'); second != std::string_view::npos) {
        const auto comp = to_size(field_text.substr(second + 1));
        if (!comp || *comp == 0) {
            return std::unexpected(std::format("'{}': component must be a number from 1", path));
        }
        e.component = comp;
        field_text = field_text.substr(0, second);
    }
    if (const auto n = to_size(field_text)) {
        e.field = *n;
    } else if (const auto by_name = lis02::field_number(*kind, field_text)) {
        e.field = *by_name;
    } else {
        return std::unexpected(
            std::format("'{}': {} has no field '{}'", path, lis02::to_string(*kind), field_text));
    }
    if (e.field < 2) {
        return std::unexpected(std::format("'{}': field numbers start at 2", path));
    }

    const auto op = std::ranges::find_if(
        operators, [&rest](const auto& o) { return rest.starts_with(o.first); });
    if (op == operators.end()) {
        return std::unexpected(std::format("'{}': unknown operator", rest));
    }
    e.op = op->second;
    auto value = trim(rest.substr(op->first.size()));
    if (!value.empty() && std::string_view{"<>=!"}.contains(value.front())) {
        // "R.value >> 100" is a typo, not a comparison with "> 100"; quote it if meant.
        return std::unexpected(
            std::format("'{}': value starts with an operator character; quote it", t));
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    e.value = std::string{value};
    if (e.op == Op::matches) {
        try {
            e.regex.emplace(e.value);
        } catch (const std::regex_error& err) {
            return std::unexpected(
                std::format("'{}': invalid regular expression: {}", e.value, err.what()));
        }
    }
    return e;
}

std::string value_of(const Expression& e, const lis02::Record& r, const lis02::Delimiters& d) {
    if (e.component) {
        return r.component(e.field, 0, *e.component - 1);
    }
    const auto* f = r.field(e.field);
    if (f == nullptr) {
        return {};
    }
    std::string out;
    for (std::size_t rep = 0; rep < f->size(); ++rep) {
        if (rep > 0) {
            out += d.repeat;
        }
        for (std::size_t c = 0; c < (*f)[rep].size(); ++c) {
            if (c > 0) {
                out += d.component;
            }
            out += (*f)[rep][c];
        }
    }
    return out;
}

bool matches(const Expression& e, const lis02::Record& r, const lis02::Delimiters& d) {
    if (r.type != e.record_type) {
        return false;
    }
    const auto actual = value_of(e, r, d);
    switch (e.op) {
        case Op::contains: return actual.contains(e.value);
        case Op::matches: return e.regex && std::regex_search(actual, *e.regex);
        default: break;
    }
    const auto a = to_number(actual);
    const auto b = to_number(e.value);
    const bool ordering = e.op == Op::lt || e.op == Op::gt || e.op == Op::le || e.op == Op::ge;
    if (ordering && b && !a) {
        return false;  // "DETECTED" > 100 is not a hit, whatever the text order says
    }
    const auto cmp = [&]() -> std::partial_ordering {
        if (a && b) {
            return *a <=> *b;
        }
        return actual <=> e.value;
    }();
    switch (e.op) {
        case Op::eq: return cmp == 0;
        case Op::ne: return cmp != 0;
        case Op::lt: return cmp < 0;
        case Op::gt: return cmp > 0;
        case Op::le: return cmp <= 0;
        case Op::ge: return cmp >= 0;
        default: return false;
    }
}

}  // namespace lisbench::watch

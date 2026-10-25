#include "lisbench/mapper/template.hpp"

#include <algorithm>
#include <format>
#include <ranges>
#include <utility>

namespace lisbench::mapper {

namespace {

bool valid_name(std::string_view s) {
    return !s.empty() && std::ranges::all_of(s, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

}  // namespace

bool Template::is_literal() const noexcept {
    return std::ranges::all_of(
        pieces, [](const Piece& p) { return std::holds_alternative<std::string>(p); });
}

std::expected<Template, std::string> parse_template(std::string_view text, std::size_t line) {
    Template t{.line = line};
    std::string literal;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '$' || i + 1 >= text.size()) {
            literal += c;
            continue;
        }
        if (text[i + 1] == '$') {
            literal += '$';
            ++i;
            continue;
        }
        if (text[i + 1] != '{') {
            literal += c;
            continue;
        }
        const auto end = text.find('}', i + 2);
        if (end == std::string_view::npos) {
            return std::unexpected(std::format(
                "line {}: placeholder at column {} is not closed with }}", line, i + 1));
        }
        auto inner = text.substr(i + 2, end - i - 2);
        Placeholder p;
        if (inner.ends_with('?')) {
            p.optional = true;
            inner.remove_suffix(1);
        }
        if (const auto dot = inner.find('.'); dot != std::string_view::npos) {
            p.scope = std::string{inner.substr(0, dot)};
            p.name = std::string{inner.substr(dot + 1)};
            if (!valid_name(p.scope) || !valid_name(p.name)) {
                return std::unexpected(
                    std::format("line {}: invalid placeholder ${{{}}}", line, inner));
            }
        } else {
            p.name = std::string{inner};
            if (p.name != "now" && p.name != "message_id" && p.name != "error") {
                return std::unexpected(
                    std::format("line {}: unknown placeholder ${{{}}} (use scope.column, cfg.x, "
                                "arg.x, now, message_id, error)",
                                line, inner));
            }
        }
        if (!literal.empty()) {
            t.pieces.emplace_back(std::exchange(literal, {}));
        }
        t.pieces.emplace_back(std::move(p));
        i = end;
    }
    if (!literal.empty()) {
        t.pieces.emplace_back(std::move(literal));
    }
    return t;
}

std::expected<std::string, std::string> render(const Template& t, const Context& ctx) {
    std::string out;
    for (const auto& piece : t.pieces) {
        if (const auto* s = std::get_if<std::string>(&piece)) {
            out += *s;
            continue;
        }
        const auto& p = std::get<Placeholder>(piece);
        const auto fail = [&](std::string_view why) -> std::unexpected<std::string> {
            const auto name = p.scope.empty() ? p.name : p.scope + "." + p.name;
            return std::unexpected(std::format("line {}: ${{{}}}: {}", t.line, name, why));
        };
        if (p.scope.empty()) {
            if (p.name == "now") {
                out += ctx.now;
            } else if (p.name == "message_id") {
                out += ctx.message_id;
            } else {
                out += ctx.error;
            }
            continue;
        }
        if (p.scope == "cfg" || p.scope == "arg") {
            const auto& map = p.scope == "cfg" ? ctx.cfg : ctx.args;
            const auto it = map.find(p.name);
            if (it == map.end()) {
                if (p.optional) {
                    continue;
                }
                return fail(p.scope == "cfg" ? "no such configuration value" : "no such argument");
            }
            out += it->second;
            continue;
        }
        // Rows: the named query level, innermost first. Each level has its
        // own columns; a column of an inner query never hides an outer one.
        const auto row = std::ranges::find(ctx.rows | std::views::reverse, p.scope, &Row::scope);
        if (row == (ctx.rows | std::views::reverse).end()) {
            return fail("no enclosing query with this name");
        }
        const auto col = row->columns.find(p.name);
        if (col == row->columns.end()) {
            if (p.optional) {
                continue;
            }
            return fail("the query has no such column");
        }
        if (const auto& value = col->second) {
            out += *value;
        } else if (!p.optional) {
            return fail("is NULL (write ${scope.column?} if empty is fine)");
        }
    }
    return out;
}

}  // namespace lisbench::mapper

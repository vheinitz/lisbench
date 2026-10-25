#pragma once

// Value templates of the mapper: text with placeholders.
//
//   ${patient.pid}     column pid of the current row of the query named patient
//   ${result.flag?}    the same, but NULL or missing gives "" instead of an error
//   ${cfg.sender}      configuration value
//   ${arg.id}          argument of this run (--arg id=5, or a row of --each)
//   ${now}             local time, YYYYMMDDHHMMSS
//   ${message_id}      stable ID of this message (same arguments, same ID)
//   ${error}           failure text (only in on_failure)
//   $$                 a literal $
//
// Placeholders are resolved by this lexer, never by text replacement: a
// value that itself contains "${" is not looked at again.

#include <cstddef>
#include <expected>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lisbench::mapper {

struct Placeholder {
    std::string scope{};  // empty for built-ins (now, message_id, error)
    std::string name{};
    bool optional{false};

    friend bool operator==(const Placeholder&, const Placeholder&) = default;
};

using Piece = std::variant<std::string, Placeholder>;

struct Template {
    std::vector<Piece> pieces{};
    std::size_t line{0};  // in the mapping file, for messages

    [[nodiscard]] bool is_literal() const noexcept;
};

[[nodiscard]] std::expected<Template, std::string> parse_template(std::string_view text,
                                                                  std::size_t line = 0);

using Columns = std::map<std::string, std::optional<std::string>>;  // nullopt = NULL

struct Row {
    std::string scope{};
    Columns columns{};
};

struct Context {
    std::map<std::string, std::string> cfg{};
    std::map<std::string, std::string> args{};
    std::vector<Row> rows{};  // outermost first
    std::string now{};
    std::string message_id{};
    std::string error{};
};

[[nodiscard]] std::expected<std::string, std::string> render(const Template& t, const Context& ctx);

}  // namespace lisbench::mapper

#pragma once

// CLSI LIS02-A2 records and messages: parse, serialize, validate, build.
//
// A message is H ... L. Field -> repeats -> components; values are UTF-8.
// Field numbers are 1-based as in the standard; field 1 is the record type.
//
// Hierarchy and sequence numbers:
//   H                 no sequence number
//   P                 counted per message
//     O               counted per patient
//       R             counted per order
//   C, M, S           attach to the preceding record, counted per parent
//   Q                 counted per message (request: H Q L)
//   L                 sequence number 1

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/lis02_codec.hpp"
#include "lisbench/lis02_records.hpp"

namespace lisbench::lis02 {

using Repeat = std::vector<std::string>;  // components
using Field = std::vector<Repeat>;        // repeats; empty = empty field

[[nodiscard]] Field make_field(std::string value);
[[nodiscard]] Field make_field(std::initializer_list<std::string> components);

struct Record {
    char type{'?'};
    std::vector<Field> fields;  // fields[0] is field 1 (record type)

    [[nodiscard]] std::optional<RecordType> kind() const noexcept { return record_type(type); }

    // Empty string for missing fields, repeats or components.
    [[nodiscard]] const std::string& component(std::size_t number, std::size_t repeat = 0,
                                               std::size_t comp = 0) const noexcept;
    [[nodiscard]] const Field* field(std::size_t number) const noexcept;

    void set(std::size_t number, Field value);

    // Drops trailing empty fields; LIS02-A2 allows omitting them.
    void trim();

    friend bool operator==(const Record&, const Record&) = default;
};

struct Message {
    Delimiters delimiters{};
    std::vector<Record> records{};
};

struct Error {
    std::size_t record{};  // index in the message
    std::string text{};
};

struct ParseOptions {
    TextOptions text{};
    // Lenient parsing keeps unknown record types and uses default delimiters
    // when the first record is not a header.
    bool strict{false};
};

// Parses records as received (without CR). Delimiters come from the header.
[[nodiscard]] std::expected<Message, Error> parse(std::span<const std::string> records,
                                                  const ParseOptions& options = {});

// Parses one record with known delimiters (a header record sets its own).
[[nodiscard]] std::expected<Record, Error> parse_record(std::string_view wire, const Delimiters& d,
                                                        const ParseOptions& options = {});

// Splits wire text at CR (LF tolerated) and parses it.
[[nodiscard]] std::expected<Message, Error> parse_text(std::string_view text,
                                                       const ParseOptions& options = {});

[[nodiscard]] std::expected<std::string, Error> serialize(const Record& record, const Delimiters& d,
                                                          const TextOptions& options = {});
[[nodiscard]] std::expected<std::vector<std::string>, Error> serialize(
    const Message& message, const TextOptions& options = {});

enum class Severity : std::uint8_t { warning, error };

struct Issue {
    std::size_t record{};
    Severity severity{};
    std::string text{};
};

struct ValidateOptions {
    // Some instruments send O directly after H (e.g. QC results without patient).
    bool allow_order_without_patient{false};
};

// Checks record order, hierarchy and sequence numbers. Several messages in
// one transfer (H..L H..L) are accepted.
[[nodiscard]] std::vector<Issue> validate(const Message& message,
                                          const ValidateOptions& options = {});

// Builds a message record by record; puts each record into the hierarchy and
// assigns its sequence number. A record that does not fit is an error, never
// silently dropped.
class Builder {
public:
    explicit Builder(Delimiters d = {}, ValidateOptions options = {});

    // The returned reference is valid until the next add() or finish().
    [[nodiscard]] std::expected<std::reference_wrapper<Record>, std::string> add(RecordType type);

    // Appends L if missing and returns the message.
    [[nodiscard]] Message finish();

private:
    Message message_{};
    ValidateOptions options_{};
    bool has_header_{false};
    bool terminated_{false};
    int patients_{0};
    int orders_{0};
    int results_{0};
    int queries_{0};
    bool has_patient_{false};
    bool has_order_{false};
};

}  // namespace lisbench::lis02

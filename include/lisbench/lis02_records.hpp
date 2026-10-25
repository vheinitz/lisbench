#pragma once

// CLSI LIS02-A2 (formerly ASTM E1394) record types and their field lists.
// This table is the single place where field positions and names live; the
// mapper, the decoder output and the validator all look them up here.
// Field 1 of every record is the record type ID.

#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace lisbench::lis02 {

enum class RecordType : char {
    header = 'H',
    patient = 'P',
    order = 'O',
    result = 'R',
    comment = 'C',
    query = 'Q',
    terminator = 'L',
    scientific = 'S',
    manufacturer = 'M',
};

[[nodiscard]] std::optional<RecordType> record_type(char c) noexcept;
[[nodiscard]] std::string_view to_string(RecordType t) noexcept;

struct RecordInfo {
    RecordType type;
    std::string_view name;
    // snake_case field names, index 0 = field 1 (record type).
    std::span<const std::string_view> fields;
    bool has_sequence;  // field 2 is a sequence number (all but H)
};

[[nodiscard]] const RecordInfo& info(RecordType t) noexcept;

// 1-based field number for a field name of a record type.
[[nodiscard]] std::optional<std::size_t> field_number(RecordType t, std::string_view name) noexcept;

// Field name for a 1-based field number; empty if beyond the standard list
// (manufacturer records and device extensions).
[[nodiscard]] std::string_view field_name(RecordType t, std::size_t number) noexcept;

}  // namespace lisbench::lis02

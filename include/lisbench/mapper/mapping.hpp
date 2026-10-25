#pragma once

// Declarative mapping from a database to LIS02-A2 messages.
//
//   mapping: 1
//   config: {sender: lisbench}                 # ${cfg.sender}; --cfg overrides
//   text: {charset: latin1, non_ascii: ascii-hex}
//   message:
//     - H: {message_control_id: "${message_id}", sender_name: "${cfg.sender}",
//           processing_id: P, version: LIS2-A2, timestamp: "${now}"}
//     - query: SELECT pid, name FROM patients WHERE pid = :pid
//       as: patient                            # columns as ${patient.pid}
//       params: {pid: "${arg.pid}"}            # bound, never pasted into SQL
//       each:
//         - P: {laboratory_patient_id: "${patient.pid}", patient_name: "${patient.name}"}
//         - query: SELECT test, value, flag FROM results WHERE pid = :pid
//           as: result
//           params: {pid: "${patient.pid}"}
//           each:
//             - O: {specimen_id: "${result.sample}"}
//             - R: {universal_test_id: ["", "", "", "${result.test}"],   # components
//                   value: "${result.value}", abnormal_flags: "${result.flag?}"}
//             - C: {comment_text: "${result.note?}", skip_if_empty: true}
//     - L: {termination_code: N}
//   on_success: {sql: "UPDATE results SET sent_at = :now WHERE pid = :pid",
//                params: {now: "${now}", pid: "${arg.pid}"}}
//   on_failure: {sql: "UPDATE results SET error = :e WHERE pid = :pid",
//                params: {e: "${error}", pid: "${arg.pid}"}}
//
// skip_if_empty drops a record when every field with a placeholder is empty
// (constant fields do not count). Fields by name (lis02_records) or number. Record type and
// sequence numbers are set by the hierarchy and cannot be assigned. A record that does not fit the
// hierarchy is an error, never dropped.

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lisbench/lis02_message.hpp"
#include "lisbench/mapper/sqlite.hpp"
#include "lisbench/mapper/template.hpp"

namespace lisbench::mapper {

struct FieldValue {
    std::size_t number{0};
    std::vector<Template> components{};
};

struct RecordNode {
    lis02::RecordType type{lis02::RecordType::comment};
    std::vector<FieldValue> fields{};
    bool skip_if_empty{false};
    std::size_t line{0};
};

struct QueryNode;
using Node = std::variant<RecordNode, QueryNode>;

struct QueryNode {
    std::string sql{};
    std::string as{"row"};
    std::map<std::string, Template> params{};
    std::vector<Node> each{};
    std::size_t line{0};
};

struct WriteBack {
    std::string sql{};
    std::map<std::string, Template> params{};
    std::size_t line{0};
};

struct Mapping {
    std::string source{};  // file name, part of the message ID
    std::map<std::string, std::string> cfg{};
    lis02::TextOptions text{};
    std::vector<Node> message{};
    std::optional<WriteBack> on_success{};
    std::optional<WriteBack> on_failure{};
};

[[nodiscard]] std::expected<Mapping, std::string> parse_mapping(std::string_view yaml,
                                                                std::string source);
// Read as UTF-8 whatever the locale; a BOM is tolerated.
[[nodiscard]] std::expected<Mapping, std::string> load_mapping(const std::filesystem::path& file);

[[nodiscard]] std::expected<lis02::Message, std::string> build_message(const Mapping& mapping,
                                                                       Database& db, Context ctx);

[[nodiscard]] std::expected<int, std::string> write_back(const WriteBack& wb, Database& db,
                                                         const Context& ctx);

// Same source and arguments give the same ID, so that a message sent twice
// can be recognised by the receiver (LIS02-A2 has no application-level
// acknowledgement: success only means the frames were acknowledged).
[[nodiscard]] std::string stable_message_id(std::string_view source,
                                            const std::map<std::string, std::string>& args);

// YYYYMMDDHHMMSS in the local time zone of this computer.
[[nodiscard]] std::string local_timestamp(std::chrono::system_clock::time_point t);

}  // namespace lisbench::mapper

#include "lisbench/lis02_records.hpp"

#include <algorithm>

namespace lisbench::lis02 {

namespace {

using namespace std::string_view_literals;

// Field lists follow the record definitions in CLSI LIS02-A2, sections 7-15.
constexpr std::array header_fields{
    "record_type"sv,
    "delimiter_definition"sv,
    "message_control_id"sv,
    "access_password"sv,
    "sender_name"sv,
    "sender_address"sv,
    "reserved"sv,
    "sender_telephone"sv,
    "sender_characteristics"sv,
    "receiver_id"sv,
    "comment"sv,
    "processing_id"sv,
    "version"sv,
    "timestamp"sv,
};

constexpr std::array patient_fields{
    "record_type"sv,
    "sequence_number"sv,
    "practice_patient_id"sv,
    "laboratory_patient_id"sv,
    "patient_id_3"sv,
    "patient_name"sv,
    "mothers_maiden_name"sv,
    "birthdate"sv,
    "sex"sv,
    "race_ethnic_origin"sv,
    "address"sv,
    "reserved"sv,
    "telephone"sv,
    "attending_physician"sv,
    "special_field_1"sv,
    "special_field_2"sv,
    "height"sv,
    "weight"sv,
    "diagnosis"sv,
    "active_medications"sv,
    "diet"sv,
    "practice_field_1"sv,
    "practice_field_2"sv,
    "admission_discharge_dates"sv,
    "admission_status"sv,
    "location"sv,
    "alternative_diagnostic_code_nature"sv,
    "alternative_diagnostic_code"sv,
    "religion"sv,
    "marital_status"sv,
    "isolation_status"sv,
    "language"sv,
    "hospital_service"sv,
    "hospital_institution"sv,
    "dosage_category"sv,
};

constexpr std::array order_fields{
    "record_type"sv,
    "sequence_number"sv,
    "specimen_id"sv,
    "instrument_specimen_id"sv,
    "universal_test_id"sv,
    "priority"sv,
    "requested_at"sv,
    "collected_at"sv,
    "collection_end_at"sv,
    "collection_volume"sv,
    "collector_id"sv,
    "action_code"sv,
    "danger_code"sv,
    "clinical_information"sv,
    "received_at"sv,
    "specimen_descriptor"sv,
    "ordering_physician"sv,
    "physician_telephone"sv,
    "user_field_1"sv,
    "user_field_2"sv,
    "laboratory_field_1"sv,
    "laboratory_field_2"sv,
    "reported_at"sv,
    "instrument_charge"sv,
    "instrument_section_id"sv,
    "report_type"sv,
    "reserved"sv,
    "collection_location"sv,
    "nosocomial_infection_flag"sv,
    "specimen_service"sv,
    "specimen_institution"sv,
};

constexpr std::array result_fields{
    "record_type"sv,   "sequence_number"sv,       "universal_test_id"sv, "value"sv,
    "units"sv,         "reference_ranges"sv,      "abnormal_flags"sv,    "abnormality_nature"sv,
    "result_status"sv, "normatives_changed_at"sv, "operator_id"sv,       "started_at"sv,
    "completed_at"sv,  "instrument_id"sv,
};

constexpr std::array comment_fields{
    "record_type"sv, "sequence_number"sv, "comment_source"sv, "comment_text"sv, "comment_type"sv,
};

constexpr std::array query_fields{
    "record_type"sv,          "sequence_number"sv,
    "starting_range_id"sv,    "ending_range_id"sv,
    "universal_test_id"sv,    "request_time_limits_nature"sv,
    "results_begin_at"sv,     "results_end_at"sv,
    "requesting_physician"sv, "requesting_physician_telephone"sv,
    "user_field_1"sv,         "user_field_2"sv,
    "request_status"sv,
};

constexpr std::array terminator_fields{
    "record_type"sv,
    "sequence_number"sv,
    "termination_code"sv,
};

constexpr std::array scientific_fields{
    "record_type"sv,
    "sequence_number"sv,
    "analytical_method"sv,
    "instrumentation"sv,
    "reagents"sv,
    "units"sv,
    "quality_control"sv,
    "specimen_descriptor"sv,
    "reserved"sv,
    "container"sv,
    "specimen_id"sv,
    "analyte"sv,
    "result"sv,
    "result_units"sv,
    "collected_at"sv,
    "result_at"sv,
    "preprocessing_steps"sv,
    "patient_diagnosis"sv,
    "patient_birthdate"sv,
    "patient_sex"sv,
    "patient_race"sv,
};

// Manufacturer records define everything after the sequence number.
constexpr std::array manufacturer_fields{
    "record_type"sv,
    "sequence_number"sv,
};

constexpr std::array records{
    RecordInfo{.type = RecordType::header,
               .name = "header",
               .fields = header_fields,
               .has_sequence = false},
    RecordInfo{.type = RecordType::patient,
               .name = "patient",
               .fields = patient_fields,
               .has_sequence = true},
    RecordInfo{
        .type = RecordType::order, .name = "order", .fields = order_fields, .has_sequence = true},
    RecordInfo{.type = RecordType::result,
               .name = "result",
               .fields = result_fields,
               .has_sequence = true},
    RecordInfo{.type = RecordType::comment,
               .name = "comment",
               .fields = comment_fields,
               .has_sequence = true},
    RecordInfo{
        .type = RecordType::query, .name = "query", .fields = query_fields, .has_sequence = true},
    RecordInfo{.type = RecordType::terminator,
               .name = "terminator",
               .fields = terminator_fields,
               .has_sequence = true},
    RecordInfo{.type = RecordType::scientific,
               .name = "scientific",
               .fields = scientific_fields,
               .has_sequence = true},
    RecordInfo{.type = RecordType::manufacturer,
               .name = "manufacturer",
               .fields = manufacturer_fields,
               .has_sequence = true},
};

}  // namespace

std::optional<RecordType> record_type(char c) noexcept {
    const auto it = std::ranges::find(
        records, c, [](const RecordInfo& r) { return static_cast<char>(r.type); });
    if (it == records.end()) {
        return std::nullopt;
    }
    return it->type;
}

const RecordInfo& info(RecordType t) noexcept {
    // Every enumerator has an entry, so the search cannot fail.
    return *std::ranges::find(records, t, &RecordInfo::type);
}

std::string_view to_string(RecordType t) noexcept {
    return info(t).name;
}

std::optional<std::size_t> field_number(RecordType t, std::string_view name) noexcept {
    const auto fields = info(t).fields;
    const auto it = std::ranges::find(fields, name);
    if (it == fields.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - fields.begin()) + 1;
}

std::string_view field_name(RecordType t, std::size_t number) noexcept {
    const auto fields = info(t).fields;
    if (number == 0 || number > fields.size()) {
        return {};
    }
    return fields[number - 1];
}

}  // namespace lisbench::lis02

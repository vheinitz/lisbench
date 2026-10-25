#include "lisbench/session/device_model.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <random>
#include <stdexcept>

namespace lisbench::session {

using lis02::RecordType;

namespace {

constexpr std::array specimen_state_names{"queued",    "queried",   "ordered",  "no-order",
                                          "timed-out", "measuring", "reported", "failed"};

void put(lis02::Record& r, RecordType t, std::string_view name, lis02::Field value) {
    r.set(lis02::field_number_or_throw(t, name), std::move(value));
}

std::string now_stamp() {
    const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d%H%M%S}", t);
}

lis02::Builder start(const std::string& sender) {
    lis02::Builder b;
    auto& h = b.add(RecordType::header).value().get();
    put(h, RecordType::header, "sender_name", lis02::make_field({sender}));
    put(h, RecordType::header, "receiver_id", lis02::make_field("LIS"));
    put(h, RecordType::header, "processing_id", lis02::make_field("P"));
    put(h, RecordType::header, "version", lis02::make_field("LIS2-A2"));
    put(h, RecordType::header, "timestamp", lis02::make_field(now_stamp()));
    return b;
}

// L-3 termination code: N normal, F query answered, I nothing for the query.
std::vector<std::string> wire(lis02::Builder& b, char termination = 'N') {
    auto& l = b.add(RecordType::terminator).value().get();
    put(l, RecordType::terminator, "termination_code",
        lis02::make_field(std::string(1, termination)));
    auto lines = lis02::serialize(b.finish());
    if (!lines) {
        throw std::logic_error(lines.error().text);
    }
    return std::move(*lines);
}

// FNV-1a: the same specimen and test give the same value on every platform.
std::uint32_t stable_hash(std::string_view a, std::string_view b) {
    std::uint32_t h = 2166136261U;
    for (const auto part : {a, std::string_view{"|"}, b}) {
        for (const char c : part) {
            h = (h ^ static_cast<unsigned char>(c)) * 16777619U;
        }
    }
    return h;
}

std::string simulate(const TestDef* t, std::string_view specimen) {
    if (t == nullptr) {
        return "0";
    }
    std::mt19937 rng{stable_hash(specimen, t->code)};
    if (t->kind == TestDef::Kind::qualitative) {
        std::uniform_int_distribution<std::size_t> pick{0, t->values.size() - 1};
        return t->values.at(pick(rng));
    }
    const double low = t->simulate_low.value_or(t->low.value_or(0));
    const double high = t->simulate_high.value_or(t->high.value_or(low + 100));
    std::uniform_real_distribution<double> value{low, high};
    return std::format("{:.{}f}", value(rng), t->decimals);
}

}  // namespace

std::string_view to_string(SpecimenState s) noexcept {
    return specimen_state_names.at(static_cast<std::size_t>(s));
}

DeviceModel::DeviceModel(DeviceConfig config, Catalog catalog, std::string name)
    : config_{std::move(config)},
      catalog_{std::move(catalog)},
      name_{std::move(name)},
      counter_{config_.start} {}

bool DeviceModel::next(DeviceSpecimen& out) {
    out = {};
    if (!config_.specimens.empty()) {
        if (list_index_ >= config_.specimens.size()) {
            return false;
        }
        out.id = config_.specimens[list_index_++];
    } else {
        out.id = std::format("{}{:05}", config_.prefix, counter_++);
    }
    if (config_.mode == DeviceMode::unsolicited) {
        out.tests = config_.tests;
    }
    return true;
}

std::vector<std::string> DeviceModel::query(const DeviceSpecimen& s) const {
    auto b = start(name_);
    auto& q = b.add(RecordType::query).value().get();
    put(q, RecordType::query, "starting_range_id", lis02::make_field({"", s.id}));
    put(q, RecordType::query, "universal_test_id", lis02::make_field("ALL"));
    put(q, RecordType::query, "request_status", lis02::make_field("O"));
    return wire(b);
}

std::vector<std::string> DeviceModel::ordered_tests(const lis02::Message& answer,
                                                    std::string_view specimen) {
    std::vector<std::string> tests;
    for (const auto& r : answer.records) {
        if (r.kind() != RecordType::order) {
            continue;
        }
        const auto& id =
            r.component(lis02::field_number_or_throw(RecordType::order, "specimen_id"));
        const auto& report =
            r.component(lis02::field_number_or_throw(RecordType::order, "report_type"));
        const auto& action =
            r.component(lis02::field_number_or_throw(RecordType::order, "action_code"));
        const auto& test =
            r.component(lis02::field_number_or_throw(RecordType::order, "universal_test_id"), 0, 3);
        // Y / Z: no order on record; C: a cancellation, not work to do.
        if (id != specimen || report == "Y" || report == "Z" || action == "C" || test.empty()) {
            continue;
        }
        if (std::ranges::find(tests, test) == tests.end()) {
            tests.push_back(test);
        }
    }
    return tests;
}

std::vector<std::string> DeviceModel::results(DeviceSpecimen& s) const {
    auto b = start(name_);
    (void)b.add(RecordType::patient).value();
    s.values.clear();
    for (const auto& test : s.tests) {
        // One O record per test, each followed by its result.
        auto& o = b.add(RecordType::order).value().get();
        put(o, RecordType::order, "specimen_id", lis02::make_field(s.id));
        put(o, RecordType::order, "universal_test_id", lis02::make_field({"", "", "", test}));
        put(o, RecordType::order, "report_type", lis02::make_field("F"));
        const auto* def = catalog_.find(test);
        const auto value = simulate(def, s.id);
        s.values.push_back(value);
        auto& r = b.add(RecordType::result).value().get();
        put(r, RecordType::result, "universal_test_id", lis02::make_field({"", "", "", test}));
        put(r, RecordType::result, "value", lis02::make_field(value));
        if (def != nullptr) {
            if (!def->unit.empty()) {
                put(r, RecordType::result, "units", lis02::make_field(def->unit));
            }
            if (def->low || def->high) {
                put(r, RecordType::result, "reference_ranges",
                    lis02::make_field(
                        std::format("{}-{}", def->low.value_or(0), def->high.value_or(0))));
            }
            if (const auto flag = abnormal_flag(*def, value); !flag.empty()) {
                put(r, RecordType::result, "abnormal_flags", lis02::make_field(flag));
            }
        }
        put(r, RecordType::result, "result_status", lis02::make_field("F"));
        put(r, RecordType::result, "completed_at", lis02::make_field(now_stamp()));
    }
    return wire(b);
}

}  // namespace lisbench::session

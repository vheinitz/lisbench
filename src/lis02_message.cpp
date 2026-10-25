#include "lisbench/lis02_message.hpp"

#include <array>
#include <cassert>
#include <charconv>
#include <format>
#include <ranges>
#include <stdexcept>

#include "lisbench/compat.hpp"

namespace lisbench::lis02 {

namespace {

const std::string empty_string;

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    return compat::to_vector(text | std::views::split(delimiter) |
                             std::views::transform([](auto&& part) {
                                 return std::string_view(part.begin(), part.end());
                             }));
}

std::optional<int> to_int(std::string_view text) {
    int value = 0;
    const auto* end = text.data() + text.size();
    // from_chars is bounded by end; no terminator is needed.
    const auto [ptr, ec] = std::from_chars(
        text.data(), end, value);  // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::expected<Record, Error> parse_record(std::string_view wire, std::size_t index,
                                          const Delimiters& d, const ParseOptions& options) {
    if (wire.empty()) {
        return std::unexpected(Error{.record = index, .text = "empty record"});
    }
    const auto parts = split(wire, d.field);
    // Field 1 is kept verbatim; its first character is the record type.
    Record record{.type = parts.front().empty() ? '?' : parts.front().front(), .fields = {}};
    if (options.strict && (parts.front().size() != 1 || !record.kind())) {
        return std::unexpected(
            Error{.record = index, .text = std::format("invalid record type '{}'", parts.front())});
    }
    for (std::size_t f = 0; f < parts.size(); ++f) {
        const auto part = parts[f];
        if (f == 0) {
            record.fields.push_back(make_field(std::string{part}));
            continue;
        }
        if (f == 1 && record.type == 'H') {
            // The delimiter definition is taken verbatim, never unescaped.
            record.fields.push_back(make_field(std::string{part}));
            continue;
        }
        Field field;
        if (!part.empty()) {
            for (const auto repeat_text : split(part, d.repeat)) {
                Repeat repeat;
                for (const auto comp_text : split(repeat_text, d.component)) {
                    auto value = decode(comp_text, d, options.text);
                    if (!value) {
                        return std::unexpected(
                            Error{.record = index,
                                  .text = std::format("field {}: {}", f + 1, value.error().text)});
                    }
                    repeat.push_back(std::move(*value));
                }
                field.push_back(std::move(repeat));
            }
        }
        record.fields.push_back(std::move(field));
    }
    return record;
}

}  // namespace

Field make_field(std::string value) {
    if (value.empty()) {
        return {};
    }
    return Field{Repeat{std::move(value)}};
}

Field make_field(std::initializer_list<std::string> components) {
    return Field{Repeat(components)};
}

const Field* Record::field(std::size_t number) const noexcept {
    if (number == 0 || number > fields.size()) {
        return nullptr;
    }
    return &fields[number - 1];
}

const std::string& Record::component(std::size_t number, std::size_t repeat,
                                     std::size_t comp) const noexcept {
    const auto* f = field(number);
    if (f == nullptr || repeat >= f->size() || comp >= (*f)[repeat].size()) {
        return empty_string;
    }
    return (*f)[repeat][comp];
}

void Record::set(std::size_t number, Field value) {
    // The record type decides the hierarchy; it is fixed at creation.
    if (number <= 1) {
        throw std::invalid_argument("field 1 (record type) cannot be assigned");
    }
    if (fields.size() < number) {
        fields.resize(number);
    }
    fields[number - 1] = std::move(value);
}

void Record::trim() {
    while (fields.size() > 1 && fields.back().empty()) {
        fields.pop_back();
    }
}

std::expected<Record, Error> parse_record(std::string_view wire, const Delimiters& d,
                                          const ParseOptions& options) {
    if (!wire.empty() && wire.front() == 'H') {
        auto own = delimiters_from_header(wire);
        if (!own) {
            return std::unexpected(Error{.record = 0, .text = own.error().text});
        }
        return parse_record(wire, 0, *own, options);
    }
    return parse_record(wire, 0, d, options);
}

std::expected<Message, Error> parse(std::span<const std::string> records,
                                    const ParseOptions& options) {
    Message message;
    for (std::size_t i = 0; i < records.size(); ++i) {
        const std::string_view wire = records[i];
        if (!wire.empty() && wire.front() == 'H') {
            auto d = delimiters_from_header(wire);
            if (!d) {
                return std::unexpected(Error{.record = i, .text = d.error().text});
            }
            // One message object has one delimiter set; a transfer that
            // switches delimiters has to be split into messages first.
            if (i > 0 && *d != message.delimiters) {
                return std::unexpected(
                    Error{.record = i, .text = "delimiters change within the transfer"});
            }
            message.delimiters = *d;
        } else if (i == 0 && options.strict) {
            return std::unexpected(Error{.record = 0, .text = "first record is not a header"});
        }
        auto record = parse_record(wire, i, message.delimiters, options);
        if (!record) {
            return std::unexpected(record.error());
        }
        message.records.push_back(std::move(*record));
    }
    return message;
}

std::expected<Message, Error> parse_text(std::string_view text, const ParseOptions& options) {
    std::vector<std::string> records;
    for (auto line : split(text, '\r')) {
        // Files written on other systems often carry LF or CR LF.
        while (!line.empty() && line.front() == '\n') {
            line.remove_prefix(1);
        }
        while (!line.empty() && line.back() == '\n') {
            line.remove_suffix(1);
        }
        for (const auto part : split(line, '\n')) {
            if (!part.empty()) {
                records.emplace_back(part);
            }
        }
    }
    return parse(records, options);
}

std::expected<std::string, Error> serialize(const Record& record, const Delimiters& d,
                                            const TextOptions& options) {
    Record trimmed = record;
    trimmed.trim();
    std::string out = trimmed.fields.empty() ? std::string{record.type} : trimmed.component(1);
    for (std::size_t f = 1; f < trimmed.fields.size(); ++f) {
        out += d.field;
        if (f == 1 && record.type == 'H') {
            // Normally exactly the three delimiters. A header whose field 2
            // carries more (seen with malformed senders) is kept verbatim so
            // that nothing is lost on the way through.
            const auto definition = d.definition().substr(1);
            const auto& stored = trimmed.component(2);
            out += stored.starts_with(definition) ? stored : definition;
            continue;
        }
        const auto& field = trimmed.fields[f];
        for (std::size_t r = 0; r < field.size(); ++r) {
            if (r > 0) {
                out += d.repeat;
            }
            for (std::size_t c = 0; c < field[r].size(); ++c) {
                if (c > 0) {
                    out += d.component;
                }
                auto encoded = encode(field[r][c], d, options);
                if (!encoded) {
                    return std::unexpected(
                        Error{.record = 0,
                              .text = std::format("field {}: {}", f + 1, encoded.error().text)});
                }
                out += encoded->wire;
            }
        }
    }
    if (record.type == 'H' && trimmed.fields.size() < 2) {
        out += d.definition();
    }
    if (out.empty()) {
        // A record without type (kept by lenient parsing) must not vanish.
        out += d.field;
    }
    return out;
}

std::expected<std::vector<std::string>, Error> serialize(const Message& message,
                                                         const TextOptions& options) {
    std::vector<std::string> out;
    out.reserve(message.records.size());
    for (std::size_t i = 0; i < message.records.size(); ++i) {
        auto text = serialize(message.records[i], message.delimiters, options);
        if (!text) {
            return std::unexpected(Error{.record = i, .text = text.error().text});
        }
        out.push_back(std::move(*text));
    }
    return out;
}

std::vector<Issue> validate(const Message& message, const ValidateOptions& options) {
    std::vector<Issue> issues;
    const auto report = [&issues](std::size_t i, Severity s, std::string text) {
        issues.push_back(Issue{.record = i, .severity = s, .text = std::move(text)});
    };

    bool in_message = false;
    bool has_patient = false;
    bool has_order = false;
    bool has_parent = false;
    int patients = 0;
    int orders = 0;
    int results = 0;
    int queries = 0;
    std::array<int, 3> children{};  // C, M, S under the current parent

    const auto check_sequence = [&](std::size_t i, const Record& r, int expected) {
        const auto& text = r.component(2);
        const auto seq = to_int(text);
        if (!seq) {
            report(i, Severity::error, std::format("sequence number '{}' is not a number", text));
        } else if (*seq != expected) {
            report(i, Severity::error,
                   std::format("sequence number {} where {} was expected", *seq, expected));
        }
    };

    for (std::size_t i = 0; i < message.records.size(); ++i) {
        const auto& r = message.records[i];
        const auto kind = r.kind();
        if (!kind) {
            report(i, Severity::warning, std::format("unknown record type '{}'", r.type));
            continue;
        }
        if (!in_message && *kind != RecordType::header) {
            report(i, Severity::error, "record outside a message (no preceding H)");
        }
        const auto new_parent = [&] {
            children = {};
            has_parent = true;
        };
        switch (*kind) {
            case RecordType::header:
                if (r.component(2).size() != 3) {
                    report(i, Severity::warning,
                           "header field 2 is not exactly the three delimiters");
                }
                if (in_message) {
                    report(i, Severity::error, "H before the previous message was terminated by L");
                }
                in_message = true;
                has_patient = has_order = false;
                patients = orders = results = queries = 0;
                new_parent();
                break;
            case RecordType::patient:
                has_patient = true;
                has_order = false;
                check_sequence(i, r, ++patients);
                orders = 0;
                new_parent();
                break;
            case RecordType::order:
                if (!has_patient && !options.allow_order_without_patient) {
                    report(i, Severity::error, "O without a preceding P");
                }
                has_order = true;
                check_sequence(i, r, ++orders);
                results = 0;
                new_parent();
                break;
            case RecordType::result:
                if (!has_order) {
                    report(i, Severity::error, "R without a preceding O");
                }
                check_sequence(i, r, ++results);
                new_parent();
                break;
            case RecordType::query:
                check_sequence(i, r, ++queries);
                new_parent();
                break;
            case RecordType::comment:
            case RecordType::manufacturer:
            case RecordType::scientific: {
                if (!has_parent) {
                    report(i, Severity::error, "C/M/S without a parent record");
                }
                std::size_t slot = 2;  // scientific
                if (*kind == RecordType::comment) {
                    slot = 0;
                } else if (*kind == RecordType::manufacturer) {
                    slot = 1;
                }
                check_sequence(i, r, ++children.at(slot));
                break;
            }
            case RecordType::terminator:
                check_sequence(i, r, 1);
                in_message = false;
                has_parent = false;
                break;
        }
    }
    if (in_message) {
        report(message.records.size(), Severity::error, "message not terminated by L");
    }
    return issues;
}

Builder::Builder(Delimiters d, ValidateOptions options) : options_{options} {
    message_.delimiters = d;
}

std::expected<std::reference_wrapper<Record>, std::string> Builder::add(RecordType type) {
    if (terminated_) {
        return std::unexpected(std::format("{} after L", to_string(type)));
    }
    if (!has_header_ && type != RecordType::header) {
        return std::unexpected(std::format("{} before H", to_string(type)));
    }
    int sequence = 0;
    switch (type) {
        case RecordType::header:
            if (has_header_) {
                return std::unexpected("second H in one message");
            }
            has_header_ = true;
            break;
        case RecordType::patient:
            sequence = ++patients_;
            orders_ = 0;
            has_patient_ = true;
            has_order_ = false;
            break;
        case RecordType::order:
            if (!has_patient_ && !options_.allow_order_without_patient) {
                return std::unexpected("O without a preceding P");
            }
            sequence = ++orders_;
            results_ = 0;
            has_order_ = true;
            break;
        case RecordType::result:
            if (!has_order_) {
                return std::unexpected("R without a preceding O");
            }
            sequence = ++results_;
            break;
        case RecordType::query: sequence = ++queries_; break;
        case RecordType::comment:
        case RecordType::manufacturer:
        case RecordType::scientific:
            // Counted per parent and per type: the parent is the last record
            // that is not itself C, M or S.
            sequence = 1;
            for (const auto& prev : message_.records | std::views::reverse) {
                const auto k = prev.kind();
                if (k == type) {
                    ++sequence;
                } else if (k != RecordType::comment && k != RecordType::manufacturer &&
                           k != RecordType::scientific) {
                    break;
                }
            }
            break;
        case RecordType::terminator:
            sequence = 1;
            terminated_ = true;
            break;
    }

    Record record{.type = static_cast<char>(type), .fields = {}};
    record.fields.push_back(make_field(std::string{record.type}));
    if (type == RecordType::header) {
        record.fields.push_back(make_field(message_.delimiters.definition().substr(1)));
    } else {
        record.set(2, make_field(std::to_string(sequence)));
    }
    message_.records.push_back(std::move(record));
    return std::ref(message_.records.back());
}

Message Builder::finish() {
    if (has_header_ && !terminated_) {
        // Cannot fail: a header exists and nothing is terminated yet.
        [[maybe_unused]] const auto added = add(RecordType::terminator);
        assert(added);
    }
    return std::move(message_);
}

}  // namespace lisbench::lis02

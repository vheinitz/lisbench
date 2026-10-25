#include "lisbench/mapper/mapping.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>

namespace lisbench::mapper {

namespace {

std::size_t line_of(const YAML::Node& n) {
    return static_cast<std::size_t>(n.Mark().line) + 1;
}

std::string at(const YAML::Node& n, std::string_view text) {
    return std::format("line {}: {}", line_of(n), text);
}

std::expected<Template, std::string> template_of(const YAML::Node& n) {
    if (!n.IsScalar() && !n.IsNull()) {
        return std::unexpected(at(n, "expected a text value"));
    }
    return parse_template(n.IsNull() ? std::string{} : n.as<std::string>(), line_of(n));
}

std::expected<std::map<std::string, Template>, std::string> params_of(const YAML::Node& n) {
    std::map<std::string, Template> out;
    if (!n) {
        return out;
    }
    if (!n.IsMap()) {
        return std::unexpected(at(n, "params must be a mapping name: value"));
    }
    for (const auto& kv : n) {
        auto t = template_of(kv.second);
        if (!t) {
            return std::unexpected(t.error());
        }
        out.emplace(kv.first.as<std::string>(), std::move(*t));
    }
    return out;
}

std::optional<std::size_t> to_size(std::string_view s) {
    const auto* const end = std::to_address(s.end());
    std::size_t v = 0;
    const auto [ptr, ec] = std::from_chars(std::to_address(s.begin()), end, v);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return v;
}

std::expected<std::vector<Node>, std::string> nodes_of(const YAML::Node& seq);

std::expected<RecordNode, std::string> record_of(const YAML::Node& item) {
    RecordNode r{.line = line_of(item)};
    std::optional<lis02::RecordType> type;
    YAML::Node fields;
    for (const auto& kv : item) {
        const auto key = kv.first.as<std::string>();
        const auto kind = key.size() == 1 ? lis02::record_type(key.front()) : std::nullopt;
        if (kind) {
            if (type) {
                return std::unexpected(at(item, "one record per list item"));
            }
            type = kind;
            fields = kv.second;
        } else {
            return std::unexpected(
                at(kv.first,
                   std::format("unknown key '{}' (a record letter H P O R C Q M S L, or query)",
                               key)));
        }
    }
    if (!type) {
        return std::unexpected(
            at(item, "expected a record (H, P, O, R, C, Q, M, S, L) or a query"));
    }
    r.type = *type;
    if (!fields || fields.IsNull()) {
        return r;
    }
    if (!fields.IsMap()) {
        return std::unexpected(at(fields, "record fields must be a mapping field: value"));
    }
    for (const auto& kv : fields) {
        const auto name = kv.first.as<std::string>();
        if (name == "skip_if_empty") {
            r.skip_if_empty = kv.second.as<bool>();
            continue;
        }
        auto number = to_size(name);
        if (!number) {
            number = lis02::field_number(r.type, name);
        }
        if (!number) {
            return std::unexpected(
                at(kv.first, std::format("{} has no field '{}'", lis02::to_string(r.type), name)));
        }
        if (*number <= 2) {
            return std::unexpected(
                at(kv.first, "record type and sequence number are set automatically"));
        }
        FieldValue fv{.number = *number};
        if (kv.second.IsSequence()) {
            for (const auto& c : kv.second) {
                auto t = template_of(c);
                if (!t) {
                    return std::unexpected(t.error());
                }
                fv.components.push_back(std::move(*t));
            }
        } else {
            auto t = template_of(kv.second);
            if (!t) {
                return std::unexpected(t.error());
            }
            fv.components.push_back(std::move(*t));
        }
        r.fields.push_back(std::move(fv));
    }
    return r;
}

std::expected<QueryNode, std::string> query_of(const YAML::Node& item) {
    QueryNode q{.sql = item["query"].as<std::string>(), .line = line_of(item)};
    for (const auto& kv : item) {
        const auto key = kv.first.as<std::string>();
        if (key != "query" && key != "as" && key != "params" && key != "each") {
            return std::unexpected(
                at(kv.first,
                   std::format("unknown key '{}' in a query (query, as, params, each)", key)));
        }
    }
    if (item["as"]) {
        q.as = item["as"].as<std::string>();
        if (q.as == "cfg" || q.as == "arg") {
            return std::unexpected(at(item["as"], "'cfg' and 'arg' are reserved names"));
        }
    }
    auto params = params_of(item["params"]);
    if (!params) {
        return std::unexpected(params.error());
    }
    q.params = std::move(*params);
    if (item["each"]) {
        auto each = nodes_of(item["each"]);
        if (!each) {
            return std::unexpected(each.error());
        }
        q.each = std::move(*each);
    }
    return q;
}

std::expected<std::vector<Node>, std::string> nodes_of(const YAML::Node& seq) {
    if (!seq.IsSequence()) {
        return std::unexpected(at(seq, "expected a list of records and queries"));
    }
    std::vector<Node> out;
    for (const auto& item : seq) {
        if (!item.IsMap()) {
            return std::unexpected(at(item, "expected a record or a query"));
        }
        if (item["query"]) {
            auto q = query_of(item);
            if (!q) {
                return std::unexpected(q.error());
            }
            out.emplace_back(std::move(*q));
        } else {
            auto r = record_of(item);
            if (!r) {
                return std::unexpected(r.error());
            }
            out.emplace_back(std::move(*r));
        }
    }
    return out;
}

std::expected<WriteBack, std::string> write_back_of(const YAML::Node& n) {
    if (!n.IsMap() || !n["sql"]) {
        return std::unexpected(at(n, "expected {sql: ..., params: {...}}"));
    }
    auto params = params_of(n["params"]);
    if (!params) {
        return std::unexpected(params.error());
    }
    return WriteBack{
        .sql = n["sql"].as<std::string>(), .params = std::move(*params), .line = line_of(n)};
}

std::expected<Params, std::string> render_params(const std::map<std::string, Template>& params,
                                                 const Context& ctx) {
    Params out;
    for (const auto& [name, t] : params) {
        auto v = render(t, ctx);
        if (!v) {
            return std::unexpected(v.error());
        }
        out.emplace(name, std::move(*v));
    }
    return out;
}

std::expected<void, std::string> walk(const std::vector<Node>& nodes, lis02::Builder& builder,
                                      Database& db, Context& ctx) {
    for (const auto& node : nodes) {
        if (const auto* r = std::get_if<RecordNode>(&node)) {
            std::vector<std::pair<std::size_t, std::vector<std::string>>> values;
            // skip_if_empty looks at the fields that carry data; constants
            // such as comment_type: G do not count.
            bool has_data_fields = false;
            bool any_data = false;
            for (const auto& f : r->fields) {
                std::vector<std::string> comps;
                for (const auto& t : f.components) {
                    auto v = render(t, ctx);
                    if (!v) {
                        return std::unexpected(v.error());
                    }
                    if (!t.is_literal()) {
                        has_data_fields = true;
                        any_data = any_data || !v->empty();
                    }
                    comps.push_back(std::move(*v));
                }
                values.emplace_back(f.number, std::move(comps));
            }
            if (r->skip_if_empty && has_data_fields && !any_data) {
                continue;
            }
            auto added = builder.add(r->type);
            if (!added) {
                return std::unexpected(std::format("line {}: {}", r->line, added.error()));
            }
            auto& record = added->get();
            for (auto& [number, comps] : values) {
                const bool all_empty = std::ranges::all_of(comps, &std::string::empty);
                record.set(number, all_empty ? lis02::Field{}
                                             : lis02::Field{lis02::Repeat{std::move(comps)}});
            }
            continue;
        }
        const auto& q = std::get<QueryNode>(node);
        auto params = render_params(q.params, ctx);
        if (!params) {
            return std::unexpected(params.error());
        }
        auto rows = db.query(q.sql, *params);
        if (!rows) {
            return std::unexpected(std::format("line {}: {}", q.line, rows.error()));
        }
        for (auto& row : *rows) {
            ctx.rows.push_back(Row{.scope = q.as, .columns = std::move(row)});
            auto inner = walk(q.each, builder, db, ctx);
            ctx.rows.pop_back();
            if (!inner) {
                return inner;
            }
        }
    }
    return {};
}

}  // namespace

std::expected<Mapping, std::string> parse_mapping(std::string_view yaml, std::string source) {
    try {
        const auto root = YAML::Load(std::string{yaml});
        if (!root.IsMap()) {
            return std::unexpected("a mapping file is a YAML mapping");
        }
        if (!root["mapping"] || root["mapping"].as<int>() != 1) {
            return std::unexpected("the file must start with 'mapping: 1'");
        }
        Mapping m{.source = std::move(source)};
        for (const auto& kv : root["config"]) {
            m.cfg[kv.first.as<std::string>()] = kv.second.as<std::string>();
        }
        if (const auto t = root["text"]) {
            if (t["charset"]) {
                const auto c = t["charset"].as<std::string>();
                if (c != "latin1" && c != "utf8") {
                    return std::unexpected(at(t["charset"], "charset is latin1 or utf8"));
                }
                m.text.charset = c == "utf8" ? lis02::Charset::utf8 : lis02::Charset::latin1;
            }
            if (t["non_ascii"]) {
                const auto n = t["non_ascii"].as<std::string>();
                if (n == "ascii-hex") {
                    m.text.non_ascii = lis02::NonAscii::ascii_hex;
                } else if (n == "raw") {
                    m.text.non_ascii = lis02::NonAscii::raw;
                } else if (n == "transliterate") {
                    m.text.non_ascii = lis02::NonAscii::transliterate;
                } else {
                    return std::unexpected(
                        at(t["non_ascii"], "non_ascii is ascii-hex, raw or transliterate"));
                }
            }
            if (t["local_escape"]) {
                const auto l = t["local_escape"].as<std::string>();
                if (l != "keep" && l != "utf16-hex") {
                    return std::unexpected(
                        at(t["local_escape"], "local_escape is keep or utf16-hex"));
                }
                m.text.local_escape =
                    l == "utf16-hex" ? lis02::LocalEscape::utf16_hex : lis02::LocalEscape::keep;
            }
        }
        if (!root["message"]) {
            return std::unexpected("a mapping needs 'message'");
        }
        auto nodes = nodes_of(root["message"]);
        if (!nodes) {
            return std::unexpected(nodes.error());
        }
        m.message = std::move(*nodes);
        for (const auto* key : {"on_success", "on_failure"}) {
            if (root[key]) {
                auto wb = write_back_of(root[key]);
                if (!wb) {
                    return std::unexpected(wb.error());
                }
                (std::string_view{key} == "on_success" ? m.on_success : m.on_failure) =
                    std::move(*wb);
            }
        }
        return m;
    } catch (const YAML::Exception& e) {
        return std::unexpected(std::format("line {}: {}", e.mark.line + 1, e.msg));
    }
}

std::expected<Mapping, std::string> load_mapping(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open {}", file.string()));
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return parse_mapping(text, file.filename().string());
}

std::expected<lis02::Message, std::string> build_message(const Mapping& mapping, Database& db,
                                                         Context ctx) {
    lis02::Builder builder;
    if (auto ok = walk(mapping.message, builder, db, ctx); !ok) {
        return std::unexpected(ok.error());
    }
    auto message = builder.finish();
    if (message.records.empty()) {
        return std::unexpected("the mapping produced no records");
    }
    return message;
}

std::expected<int, std::string> write_back(const WriteBack& wb, Database& db, const Context& ctx) {
    auto params = render_params(wb.params, ctx);
    if (!params) {
        return std::unexpected(params.error());
    }
    auto changed = db.execute(wb.sql, *params);
    if (!changed) {
        return std::unexpected(std::format("line {}: {}", wb.line, changed.error()));
    }
    return changed;
}

std::string stable_message_id(std::string_view source,
                              const std::map<std::string, std::string>& args) {
    // FNV-1a over the source name and the sorted arguments (std::map is sorted).
    std::uint64_t h = 0xcbf29ce484222325ULL;
    const auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ULL;
        }
        h ^= 0xFFU;  // separator
        h *= 0x100000001b3ULL;
    };
    mix(source);
    for (const auto& [k, v] : args) {
        mix(k);
        mix(v);
    }
    return std::format("{:016X}", h);
}

std::string local_timestamp(std::chrono::system_clock::time_point t) {
    const std::chrono::zoned_time local{std::chrono::current_zone(),
                                        std::chrono::floor<std::chrono::seconds>(t)};
    return std::format("{:%Y%m%d%H%M%S}", local);
}

}  // namespace lisbench::mapper

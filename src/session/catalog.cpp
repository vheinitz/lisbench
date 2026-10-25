#include "lisbench/session/catalog.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <set>

#include "yaml_util.hpp"

namespace lisbench::session {

using namespace detail;

const TestDef* Catalog::find(std::string_view code) const noexcept {
    const auto it = std::ranges::find(tests, code, &TestDef::code);
    return it == tests.end() ? nullptr : &*it;
}

std::expected<Catalog, std::string> parse_catalog(std::string_view yaml) {
    try {
        const auto root = YAML::Load(std::string{yaml});
        only_keys(root, "the catalogue", {"lisbench", "tests"});
        if (get<std::string>(root, "lisbench", "") != "catalog/1") {
            fail(root, "a catalogue starts with 'lisbench: catalog/1'");
        }
        Catalog c;
        std::set<std::string> codes;
        for (const auto& t : root["tests"]) {
            only_keys(t, "a test",
                      {"code", "name", "kind", "unit", "reference", "decimals", "simulate",
                       "values", "abnormal"});
            TestDef d;
            d.code = required<std::string>(t, "code");
            if (d.code.empty() || d.code.find_first_of("|^\\&\r\n") != std::string::npos) {
                fail(t["code"], "a test code must not be empty or contain delimiters");
            }
            if (!codes.insert(d.code).second) {
                fail(t["code"], std::format("test code '{}' appears twice", d.code));
            }
            d.name = get<std::string>(t, "name", d.code);
            d.kind = choice(
                t, "kind", TestDef::Kind::numeric,
                {{"numeric", TestDef::Kind::numeric}, {"qualitative", TestDef::Kind::qualitative}});
            d.unit = get<std::string>(t, "unit", "");
            d.decimals = get<int>(t, "decimals", 0);
            if (d.decimals < 0 || d.decimals > 6) {
                fail(t["decimals"], "decimals is 0..6");
            }
            if (const auto r = t["reference"]) {
                only_keys(r, "reference", {"low", "high"});
                if (r["low"]) {
                    d.low = get<double>(r, "low", 0);
                }
                if (r["high"]) {
                    d.high = get<double>(r, "high", 0);
                }
                if (d.low && d.high && *d.low > *d.high) {
                    fail(r, "reference: low is above high");
                }
            }
            if (const auto s = t["simulate"]) {
                only_keys(s, "simulate", {"low", "high"});
                d.simulate_low = required<double>(s, "low");
                d.simulate_high = required<double>(s, "high");
                if (*d.simulate_low > *d.simulate_high) {
                    fail(s, "simulate: low is above high");
                }
            }
            d.values = get<std::vector<std::string>>(t, "values", {});
            d.abnormal = get<std::vector<std::string>>(t, "abnormal", {});
            if (d.kind == TestDef::Kind::qualitative && d.values.empty()) {
                fail(t, std::format("qualitative test '{}' needs values", d.code));
            }
            for (const auto& a : d.abnormal) {
                if (std::ranges::find(d.values, a) == d.values.end()) {
                    fail(t["abnormal"], std::format("'{}' is not one of the values", a));
                }
            }
            c.tests.push_back(std::move(d));
        }
        return c;
    } catch (const FileError& e) {
        return std::unexpected(e.what());
    } catch (const YAML::Exception& e) {
        return std::unexpected(std::format("line {}: {}", e.mark.line + 1, e.msg));
    }
}

std::expected<Catalog, std::string> load_catalog(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open {}", file.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto c = parse_catalog(text);
    if (!c) {
        return std::unexpected(std::format("{}: {}", file.string(), c.error()));
    }
    return c;
}

std::string to_yaml(const Catalog& catalog) {
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "lisbench" << YAML::Value << "catalog/1";
    out << YAML::Key << "tests" << YAML::Value << YAML::BeginSeq;
    for (const auto& t : catalog.tests) {
        out << YAML::BeginMap;
        out << YAML::Key << "code" << YAML::Value << t.code;
        out << YAML::Key << "name" << YAML::Value << t.name;
        out << YAML::Key << "kind" << YAML::Value
            << (t.kind == TestDef::Kind::numeric ? "numeric" : "qualitative");
        if (!t.unit.empty()) {
            out << YAML::Key << "unit" << YAML::Value << t.unit;
        }
        if (t.low || t.high) {
            out << YAML::Key << "reference" << YAML::Value << YAML::Flow << YAML::BeginMap;
            if (t.low) {
                out << YAML::Key << "low" << YAML::Value << *t.low;
            }
            if (t.high) {
                out << YAML::Key << "high" << YAML::Value << *t.high;
            }
            out << YAML::EndMap;
        }
        if (t.decimals != 0) {
            out << YAML::Key << "decimals" << YAML::Value << t.decimals;
        }
        if (t.simulate_low && t.simulate_high) {
            out << YAML::Key << "simulate" << YAML::Value << YAML::Flow << YAML::BeginMap
                << YAML::Key << "low" << YAML::Value << *t.simulate_low << YAML::Key << "high"
                << YAML::Value << *t.simulate_high << YAML::EndMap;
        }
        if (!t.values.empty()) {
            out << YAML::Key << "values" << YAML::Value << YAML::Flow << t.values;
        }
        if (!t.abnormal.empty()) {
            out << YAML::Key << "abnormal" << YAML::Value << YAML::Flow << t.abnormal;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq << YAML::EndMap;
    return std::string{out.c_str()} + "\n";
}

std::string abnormal_flag(const TestDef& test, std::string_view value) {
    if (test.kind == TestDef::Kind::qualitative) {
        if (std::ranges::find(test.values, value) == test.values.end()) {
            return "";
        }
        return std::ranges::find(test.abnormal, value) != test.abnormal.end() ? "A" : "N";
    }
    double v = 0;
    const auto [p, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (ec != std::errc{} || p != value.data() + value.size()) {
        return "";
    }
    if (test.low && v < *test.low) {
        return "L";
    }
    if (test.high && v > *test.high) {
        return "H";
    }
    return (test.low || test.high) ? "N" : "";
}

}  // namespace lisbench::session

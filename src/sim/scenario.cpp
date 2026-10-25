#include "lisbench/sim/scenario.hpp"

#include <yaml-cpp/yaml.h>

#include <charconv>
#include <format>
#include <fstream>
#include <iterator>
#include <utility>

namespace lisbench::sim {

namespace {

// Error text with the YAML position, 1-based like an editor shows it.
std::string at(const YAML::Node& node, std::string_view text) {
    return std::format("line {}: {}", node.Mark().line + 1, text);
}

std::expected<lis01::Outcome, std::string> outcome_of(const YAML::Node& node) {
    const auto name = node.as<std::string>();
    for (const auto o :
         {lis01::Outcome::delivered, lis01::Outcome::rejected, lis01::Outcome::timeout,
          lis01::Outcome::disconnected, lis01::Outcome::cancelled}) {
        if (lis01::to_string(o) == name) {
            return o;
        }
    }
    return std::unexpected(at(node, std::format("unknown outcome '{}'", name)));
}

std::expected<milliseconds, std::string> duration_of(const YAML::Node& node) {
    auto d = parse_duration(node.as<std::string>());
    if (!d) {
        return std::unexpected(at(node, d.error()));
    }
    return d;
}

std::expected<std::vector<std::string>, std::string> records_of(const YAML::Node& node,
                                                                const std::filesystem::path& base) {
    if (node.IsScalar()) {
        auto records = read_records(base / node.as<std::string>());
        if (!records) {
            return std::unexpected(at(node, records.error()));
        }
        return records;
    }
    if (node.IsMap() && node["records"]) {
        return node["records"].as<std::vector<std::string>>();
    }
    if (node.IsMap() && node["file"]) {
        return records_of(node["file"], base);
    }
    return std::unexpected(at(node, "expected a file name, {file: ...} or {records: [...]}"));
}

std::expected<io::Fault, std::string> fault_of(const YAML::Node& node) {
    if (!node.IsMap() || !node["on"] || !node["do"]) {
        return std::unexpected(at(node, "a fault needs 'on' and 'do'"));
    }
    const auto target = io::fault_target(node["on"].as<std::string>());
    if (!target) {
        return std::unexpected(at(node["on"], "unknown fault target (ack, nak, enq, eot, frame)"));
    }
    const auto action = io::fault_action(node["do"].as<std::string>());
    if (!action) {
        return std::unexpected(at(node["do"], "unknown fault action (drop, nak, corrupt, delay)"));
    }
    io::Fault f{.target = *target, .action = *action};
    if (node["nth"]) {
        f.nth = node["nth"].as<std::size_t>();
    }
    if (node["every"]) {
        f.every = node["every"].as<std::size_t>();
    }
    if (f.action == io::FaultAction::delay) {
        if (!node["ms"]) {
            return std::unexpected(at(node, "delay needs 'ms'"));
        }
        f.delay = milliseconds{node["ms"].as<long>()};
    }
    if (f.action == io::FaultAction::nak && f.target != io::FaultTarget::ack) {
        return std::unexpected(at(node, "'do: nak' only applies to 'on: ack'"));
    }
    return f;
}

std::expected<Step, std::string> step_of(const YAML::Node& node,
                                         const std::filesystem::path& base) {
    if (node["send"]) {
        auto records = records_of(node["send"], base);
        if (!records) {
            return std::unexpected(records.error());
        }
        SendStep s{.records = std::move(*records),
                   .source = node["send"].IsScalar() ? node["send"].as<std::string>() : "inline"};
        if (node["expect"]) {
            auto o = outcome_of(node["expect"]);
            if (!o) {
                return std::unexpected(o.error());
            }
            s.expect = *o;
        }
        return s;
    }
    if (node["expect"]) {
        const auto e = node["expect"];
        ExpectStep x;
        if (e.IsMap()) {
            if (e["within"]) {
                auto d = duration_of(e["within"]);
                if (!d) {
                    return std::unexpected(d.error());
                }
                x.within = *d;
            }
            if (e["records"]) {
                x.records = e["records"].as<std::size_t>();
            }
            if (e["contains"]) {
                x.contains = e["contains"].as<std::vector<std::string>>();
            }
            if (e["valid"]) {
                x.valid = e["valid"].as<bool>();
            }
            if (e["complete"]) {
                x.complete = e["complete"].as<bool>();
            }
        }
        return x;
    }
    if (node["wait"]) {
        auto d = duration_of(node["wait"]);
        if (!d) {
            return std::unexpected(d.error());
        }
        return WaitStep{.duration = *d};
    }
    return std::unexpected(at(node, "a step is send, expect or wait"));
}

}  // namespace

std::expected<milliseconds, std::string> parse_duration(std::string_view text) {
    long value = 0;
    const auto* end = text.data() + text.size();
    // from_chars is bounded by end; no terminator is needed.
    const auto [ptr, ec] = std::from_chars(
        text.data(), end, value);  // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (ec != std::errc{} || value < 0) {
        return std::unexpected(std::format("'{}' is not a duration (e.g. 500ms, 15s, 2m)", text));
    }
    const std::string_view unit(ptr, end);
    if (unit == "ms") {
        return milliseconds{value};
    }
    if (unit == "s" || unit.empty()) {
        return milliseconds{value * 1000};
    }
    if (unit == "m") {
        return milliseconds{value * 60'000};
    }
    return std::unexpected(std::format("unknown time unit '{}' (ms, s, m)", unit));
}

std::expected<std::vector<std::string>, std::string> read_records(
    const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open {}", file.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::string> records;
    std::string current;
    for (const char c : text) {
        if (c == '\r' || c == '\n') {
            if (!current.empty()) {
                records.push_back(std::exchange(current, {}));
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        records.push_back(std::move(current));
    }
    return records;
}

std::expected<Scenario, std::string> parse_scenario(std::string_view yaml,
                                                    const std::filesystem::path& base_dir) {
    try {
        const auto root = YAML::Load(std::string{yaml});
        if (!root.IsMap()) {
            return std::unexpected("scenario must be a YAML mapping");
        }
        Scenario s;
        s.name = root["name"] ? root["name"].as<std::string>() : "unnamed";
        if (root["role"]) {
            const auto role = root["role"].as<std::string>();
            if (role == "instrument") {
                s.role = lis01::Role::instrument;
            } else if (role == "computer" || role == "lis") {
                s.role = lis01::Role::computer;
            } else {
                return std::unexpected(at(root["role"], "role is instrument or computer"));
            }
        }
        if (!root["endpoint"]) {
            return std::unexpected("scenario needs an endpoint");
        }
        auto endpoint = io::parse_endpoint(root["endpoint"].as<std::string>());
        if (!endpoint) {
            return std::unexpected(at(root["endpoint"], endpoint.error()));
        }
        s.endpoint = *endpoint;

        if (const auto t = root["timing"]) {
            const auto set = [&t](const char* key,
                                  milliseconds& target) -> std::expected<void, std::string> {
                if (t[key]) {
                    auto d = duration_of(t[key]);
                    if (!d) {
                        return std::unexpected(d.error());
                    }
                    target = *d;
                }
                return {};
            };
            for (const auto& r : {set("reply_timeout", s.timing.reply_timeout),
                                  set("receive_timeout", s.timing.receive_timeout),
                                  set("busy_retry", s.timing.busy_retry),
                                  set("contention_instrument", s.timing.contention_instrument),
                                  set("contention_computer", s.timing.contention_computer)}) {
                if (!r) {
                    return std::unexpected(r.error());
                }
            }
            if (const auto w = t["contention_winner"]) {
                const auto v = w.as<std::string>();
                if (v != "instrument" && v != "computer") {
                    return std::unexpected(at(w, "contention_winner is instrument or computer"));
                }
                s.timing.contention_winner =
                    v == "computer" ? lis01::Role::computer : lis01::Role::instrument;
            }
            if (t["max_frame_attempts"]) {
                s.timing.max_frame_attempts = t["max_frame_attempts"].as<int>();
            }
            if (t["max_text"]) {
                s.timing.max_text = t["max_text"].as<std::size_t>();
            }
        }
        for (const auto& f : root["faults"]) {
            auto fault = fault_of(f);
            if (!fault) {
                return std::unexpected(fault.error());
            }
            s.faults.push_back(*fault);
        }
        if (root["answer_queries"]) {
            auto records = records_of(root["answer_queries"], base_dir);
            if (!records) {
                return std::unexpected(records.error());
            }
            s.answer_queries = std::move(*records);
        }
        if (root["serve"]) {
            s.serve = root["serve"].as<bool>();
        }
        for (const auto& step : root["steps"]) {
            auto parsed = step_of(step, base_dir);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            s.steps.push_back(std::move(*parsed));
        }
        return s;
    } catch (const YAML::Exception& e) {
        return std::unexpected(std::format("line {}: {}", e.mark.line + 1, e.msg));
    }
}

std::expected<Scenario, std::string> load_scenario(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open {}", file.string()));
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // The file is UTF-8 whatever the locale says; a BOM is tolerated.
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return parse_scenario(text, file.parent_path());
}

}  // namespace lisbench::sim

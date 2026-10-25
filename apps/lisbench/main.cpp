// lisbench command line: offline tools (frame, deframe, decode, validate),
// the simulator and the monitor.

#include <CLI/CLI.hpp>
#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <expected>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <utility>

#include "cmd_map.hpp"
#include "cmd_monitor.hpp"
#include "common.hpp"
#include "lisbench/lis01_decoder.hpp"
#include "lisbench/lis01_frame.hpp"
#include "lisbench/lis01_link.hpp"
#include "lisbench/lis02_message.hpp"
#include "lisbench/printable.hpp"
#include "lisbench/sim/runner.hpp"
#include "lisbench/sim/scenario.hpp"

namespace {

using namespace lisbench;
using cli::clock_now;
using cli::println;
using cli::read_input;
using cli::TextFlags;

bool looks_framed(std::string_view data) {
    return data.find('\x02') != std::string_view::npos;
}

// Records from a file that is either plain record text or a raw capture.
std::vector<std::string> load_records(const std::string& data, int& problems) {
    if (!looks_framed(data)) {
        // Plain text: one record per line, CR, LF or CR LF.
        std::vector<std::string> out;
        std::string current;
        for (const char c : data) {
            if (c == '\r' || c == '\n') {
                if (!current.empty()) {
                    out.push_back(std::exchange(current, {}));
                }
            } else {
                current += c;
            }
        }
        if (!current.empty()) {
            out.push_back(current);
        }
        return out;
    }
    const auto capture = lis01::deframe(to_bytes(data));
    for (const auto& p : capture.problems) {
        println(stderr, "deframe: {}", p);
        ++problems;
    }
    std::vector<std::string> out;
    out.reserve(capture.records.size());
    for (const auto& r : capture.records) {
        out.push_back(to_string(r));
    }
    return out;
}

int cmd_frame(const std::string& input, bool raw) {
    const auto data = read_input(input);
    int problems = 0;
    lis01::Message message;
    for (const auto& r : load_records(data, problems)) {
        message.records.push_back(to_bytes(r));
    }
    bytes session{lis01::ctl::enq};
    for (const auto& frame : lis01::frames_for(message)) {
        const auto encoded = lis01::encode(frame);
        session.insert(session.end(), encoded.begin(), encoded.end());
    }
    session.push_back(lis01::ctl::eot);
    if (raw) {
        const auto text = to_string(session);
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }
    println("<ENQ>");
    for (const auto& frame : lis01::frames_for(message)) {
        println("{}", printable(lis01::encode(frame)));
    }
    println("<EOT>");
    return 0;
}

int cmd_deframe(const std::string& input) {
    const auto data = read_input(input);
    const auto capture = lis01::deframe(to_bytes(data));
    for (const auto& r : capture.records) {
        println("{}", printable(r));
    }
    for (const auto& p : capture.problems) {
        println(stderr, "problem: {}", p);
    }
    return capture.problems.empty() ? 0 : 1;
}

nlohmann::json to_json(const lis02::Record& r) {
    nlohmann::json fields = nlohmann::json::object();
    const auto kind = r.kind();
    for (std::size_t n = 2; n <= r.fields.size(); ++n) {
        const auto& f = r.fields[n - 1];
        if (f.empty()) {
            continue;
        }
        auto name = kind ? std::string{lis02::field_name(*kind, n)} : std::string{};
        if (name.empty()) {
            name = "field_" + std::to_string(n);
        }
        if (f.size() == 1 && f[0].size() == 1) {
            fields[name] = f[0][0];
        } else {
            fields[name] = f;
        }
    }
    return {{"type", std::string{r.type}}, {"fields", fields}};
}

int cmd_decode(const std::string& input, const TextFlags& text, bool json) {
    const auto data = read_input(input);
    int problems = 0;
    const auto records = load_records(data, problems);
    auto message = lis02::parse(records, {.text = text.options(), .strict = text.strict});
    if (!message) {
        println(stderr, "record {}: {}", message.error().record + 1, message.error().text);
        return 2;
    }
    if (json) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& r : message->records) {
            out.push_back(to_json(r));
        }
        println("{}", out.dump(2));
        return problems == 0 ? 0 : 1;
    }
    for (const auto& r : message->records) {
        const auto kind = r.kind();
        println("{} {}", r.type, kind ? lis02::to_string(*kind) : "unknown");
        for (std::size_t n = 2; n <= r.fields.size(); ++n) {
            const auto& f = r.fields[n - 1];
            if (f.empty()) {
                continue;
            }
            const auto name = kind ? lis02::field_name(*kind, n) : std::string_view{};
            std::string value;
            for (std::size_t rep = 0; rep < f.size(); ++rep) {
                if (rep > 0) {
                    value += " \\ ";
                }
                for (std::size_t c = 0; c < f[rep].size(); ++c) {
                    if (c > 0) {
                        value += " ^ ";
                    }
                    value += f[rep][c];
                }
            }
            println("  {:>3} {:<28} {}", n, name.empty() ? "-" : name, value);
        }
    }
    return problems == 0 ? 0 : 1;
}

int cmd_validate(const std::string& input, const TextFlags& text, bool allow_order) {
    const auto data = read_input(input);
    int problems = 0;
    const auto records = load_records(data, problems);
    auto message = lis02::parse(records, {.text = text.options(), .strict = text.strict});
    if (!message) {
        println(stderr, "record {}: {}", message.error().record + 1, message.error().text);
        return 2;
    }
    const auto issues = lis02::validate(*message, {.allow_order_without_patient = allow_order});
    int errors = 0;
    for (const auto& issue : issues) {
        const bool is_error = issue.severity == lis02::Severity::error;
        errors += is_error ? 1 : 0;
        println("record {}: {}: {}", issue.record + 1, is_error ? "error" : "warning", issue.text);
    }
    if (issues.empty()) {
        println("{} records, no issues", message->records.size());
    }
    return errors + problems == 0 ? 0 : 1;
}

struct SimFlags {
    std::string scenario;
    std::string role{"instrument"};
    std::string endpoint;
    std::vector<std::string> send;
    bool receive{false};
    std::string answer_queries;
    bool serve{false};
    std::vector<std::string> faults;
    bool trace{false};
};

// "ack:2:nak", "frame:every3:corrupt", "ack:all:delay:20000"
std::expected<io::Fault, std::string> parse_fault(const std::string& spec) {
    std::vector<std::string> parts;
    for (auto p : spec | std::views::split(':')) {
        parts.emplace_back(p.begin(), p.end());
    }
    if (parts.size() < 3) {
        return std::unexpected(std::format("fault '{}': expected on:which:do[:ms]", spec));
    }
    const auto target = io::fault_target(parts[0]);
    const auto action = io::fault_action(parts[2]);
    if (!target || !action) {
        return std::unexpected(std::format("fault '{}': unknown target or action", spec));
    }
    io::Fault f{.target = *target, .action = *action};
    try {
        if (parts[1].starts_with("every")) {
            f.every = std::stoul(parts[1].substr(5));
        } else if (parts[1] != "all") {
            f.nth = std::stoul(parts[1]);
        }
        if (f.action == io::FaultAction::delay) {
            f.delay = std::chrono::milliseconds{parts.size() > 3 ? std::stol(parts[3]) : 20'000};
        }
    } catch (const std::exception&) {
        return std::unexpected(std::format("fault '{}': bad number", spec));
    }
    return f;
}

int cmd_sim(const SimFlags& flags) {
    sim::Scenario scenario;
    if (!flags.scenario.empty()) {
        auto loaded = sim::load_scenario(flags.scenario);
        if (!loaded) {
            println(stderr, "{}: {}", flags.scenario, loaded.error());
            return 2;
        }
        scenario = std::move(*loaded);
    } else {
        if (flags.endpoint.empty()) {
            println(stderr, "sim: give a scenario file or --endpoint");
            return 2;
        }
        auto endpoint = io::parse_endpoint(flags.endpoint);
        if (!endpoint) {
            println(stderr, "sim: {}", endpoint.error());
            return 2;
        }
        scenario.name = "command line";
        scenario.endpoint = *endpoint;
        scenario.role =
            flags.role == "instrument" ? lis01::Role::instrument : lis01::Role::computer;
        scenario.serve = flags.serve;
        for (const auto& file : flags.send) {
            auto records = sim::read_records(file);
            if (!records) {
                println(stderr, "sim: {}", records.error());
                return 2;
            }
            scenario.steps.emplace_back(
                sim::SendStep{.records = std::move(*records), .source = file});
        }
        if (flags.receive) {
            scenario.steps.emplace_back(sim::ExpectStep{});
        }
        if (!flags.answer_queries.empty()) {
            auto records = sim::read_records(flags.answer_queries);
            if (!records) {
                println(stderr, "sim: {}", records.error());
                return 2;
            }
            scenario.answer_queries = std::move(*records);
        }
        for (const auto& spec : flags.faults) {
            auto f = parse_fault(spec);
            if (!f) {
                println(stderr, "sim: {}", f.error());
                return 2;
            }
            scenario.faults.push_back(*f);
        }
    }

    sim::Observers obs;
    obs.on_info = [](const std::string& text) { println("{} [sim] {}", clock_now(), text); };
    obs.on_step = [](const sim::StepReport& r) {
        println("{} [step {}] {}: {}", clock_now(), r.index + 1, r.ok ? "ok" : "FAILED", r.text);
    };
    obs.session.on_warning = [](const std::string& text) {
        println("{} [warn] {}", clock_now(), text);
    };
    obs.session.on_transfer = [](const io::Transfer& t) {
        println("{} [recv] transfer {} ({} records{})", clock_now(),
                t.complete ? "complete" : "INCOMPLETE", t.records.size(),
                t.complete ? "" : ", " + t.reason);
        for (const auto& r : t.records) {
            println("         {}", printable(r));
        }
    };
    if (flags.trace) {
        obs.session.on_wire = [](const io::WireEvent& w) {
            println("{} {} {}{}", clock_now(), w.direction == io::Direction::in ? "<-" : "->",
                    printable(w.data),
                    w.fault ? std::format("   [fault: {} {}]", io::to_string(w.fault->target),
                                          io::to_string(w.fault->action))
                            : "");
        };
    }

    asio::io_context io;
    asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&io](const std::error_code&, int) { io.stop(); });
    sim::Report report;
    asio::co_spawn(io, sim::run(scenario, obs), [&](const std::exception_ptr& e, sim::Report r) {
        if (e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                println(stderr, "sim: {}", ex.what());
                report.ok = false;
            }
        } else {
            report = std::move(r);
        }
        signals.cancel();
    });
    io.run();
    if (!report.steps.empty()) {
        const auto failed =
            std::ranges::count_if(report.steps, [](const auto& s) { return !s.ok; });
        println("{} [sim] {} steps, {} failed", clock_now(), report.steps.size(), failed);
    }
    return report.ok ? 0 : 1;
}

int run(int argc, char** argv) {
    CLI::App app{"lisbench - tools for LIS01-A2 / LIS02-A2 (ASTM E1381 / E1394) interfaces"};
    app.require_subcommand(1);
    app.set_version_flag("--version", "lisbench " LISBENCH_VERSION);

    std::string input = "-";
    bool raw = false;
    bool json = false;
    bool allow_order = false;
    TextFlags text;

    auto* frame = app.add_subcommand("frame", "Wrap records into LIS01-A2 frames (ENQ ... EOT)");
    frame->add_option("input", input, "Record text, one record per line (- = stdin)");
    frame->add_flag("--raw", raw, "Write the raw bytes instead of a readable listing");

    auto* deframe =
        app.add_subcommand("deframe", "Check frames of a raw capture and print the records");
    deframe->add_option("input", input, "Raw capture (- = stdin)");

    auto* decode = app.add_subcommand("decode", "Show records field by field");
    decode->add_option("input", input, "Record text or raw capture (- = stdin)");
    decode->add_flag("--json", json, "JSON output");
    text.add_to(*decode);

    auto* validate =
        app.add_subcommand("validate", "Check record order, hierarchy and sequence numbers");
    validate->add_option("input", input, "Record text or raw capture (- = stdin)");
    validate->add_flag("--allow-order-without-patient", allow_order, "Accept O directly after H");
    text.add_to(*validate);

    SimFlags sim_flags;
    auto* simulate =
        app.add_subcommand("sim", "Simulate an instrument or a LIS on TCP or a serial port");
    simulate->add_option("scenario", sim_flags.scenario,
                         "Scenario file (YAML); or use the options below");
    simulate->add_option("--role", sim_flags.role, "instrument or computer (the LIS)")
        ->check(CLI::IsMember({"instrument", "computer"}))
        ->capture_default_str();
    simulate->add_option("--endpoint", sim_flags.endpoint,
                         "tcp://host:port, tcp-listen://:port, serial:/dev/ttyUSB0?baud=9600");
    simulate->add_option("--send", sim_flags.send, "Message file to send (repeatable)");
    simulate->add_flag("--receive", sim_flags.receive, "Wait for one transfer after sending");
    simulate->add_option("--answer-queries", sim_flags.answer_queries,
                         "Reply to queries with this message file");
    simulate->add_flag("--serve", sim_flags.serve,
                       "With tcp-listen: accept the next connection afterwards");
    simulate->add_option("--fault", sim_flags.faults,
                         "on:which:do[:ms], e.g. ack:2:nak, frame:every3:corrupt");
    simulate->add_flag("--trace", sim_flags.trace, "Show every byte on the wire");

    cli::MonitorFlags mon;
    auto* monitor =
        app.add_subcommand("monitor", "Pass traffic between two endpoints and show it decoded");
    monitor->add_option("--a", mon.a,
                        "Side A endpoint, e.g. tcp-listen://:15000 (the instrument connects here)");
    monitor->add_option("--b", mon.b, "Side B endpoint, e.g. tcp://lis-host:5000");
    monitor->add_option("--replay", mon.replay, "Replay a capture file instead of connecting");
    monitor->add_option("--labels", mon.labels, "Names for A and B")->capture_default_str();
    monitor->add_option("--watch", mon.watches,
                        "Watch expression, e.g. \"R.value > 100\" (repeatable)");
    monitor->add_option("--save", mon.save,
                        "Append the raw traffic to a capture file (JSON lines)");
    monitor->add_flag("--tap", mon.tap, "Only listen, do not pass bytes on (two receive lines)");
    monitor->add_flag("--decode", mon.decode, "Show every record field by field");
    monitor->add_flag("--verbose", mon.verbose, "Also show ENQ/ACK/EOT and every frame");
    monitor->add_option("--slow-reply", mon.slow_reply_ms, "Report replies slower than this (ms)")
        ->capture_default_str();
    mon.text.add_to(*monitor);

    cli::BridgeFlags bridge_flags;
    auto* bridge =
        app.add_subcommand("bridge", "Join two endpoints: serial<->TCP, client<->server");
    bridge->add_option("--a", bridge_flags.a, "Endpoint A")->required();
    bridge->add_option("--b", bridge_flags.b, "Endpoint B")->required();
    bridge->add_flag("--serve", bridge_flags.serve,
                     "Keep going: reconnect and accept again after a close");
    bridge->add_option("--save", bridge_flags.save, "Append the traffic to a capture file");

    cli::RecordFlags record_flags;
    auto* record = app.add_subcommand("record", "Write what an endpoint sends to a file");
    record->add_option("--from", record_flags.from, "Endpoint to read")->required();
    record->add_option("--to", record_flags.to, "Capture file (JSON lines, replayable by monitor)");
    record->add_option("--raw", record_flags.raw, "Raw byte file");
    record->add_flag("--serve", record_flags.serve, "Keep going after a close");

    cli::MapFlags map_flags;
    auto* map = app.add_subcommand("map", "Build messages from a database through a mapping file");
    map->add_option("mapping", map_flags.mapping, "Mapping file (YAML)")->required();
    map->add_option("--db", map_flags.db, "SQLite database file")->required();
    map->add_option("--arg", map_flags.args, "Argument name=value, as ${arg.name} (repeatable)");
    map->add_option("--cfg", map_flags.cfg, "Override a config value name=value (repeatable)");
    map->add_option("--each", map_flags.each,
                    "SQL: one message per result row, columns become ${arg.*}");
    map->add_option("--to", map_flags.to,
                    "Endpoint (tcp://..., serial:...) or file:dir/name_{unique}.astm; "
                    "without --to the messages are only printed");
    map->add_option("--role", map_flags.role, "instrument or computer when sending")
        ->check(CLI::IsMember({"instrument", "computer"}))
        ->capture_default_str();
    map->add_flag("--keep-going", map_flags.keep_going, "Continue after a failed message");

    CLI11_PARSE(app, argc, argv);
    try {
        if (*frame) {
            return cmd_frame(input, raw);
        }
        if (*deframe) {
            return cmd_deframe(input);
        }
        if (*decode) {
            return cmd_decode(input, text, json);
        }
        if (*validate) {
            return cmd_validate(input, text, allow_order);
        }
        if (*simulate) {
            return cmd_sim(sim_flags);
        }
        if (*map) {
            return cli::cmd_map(map_flags);
        }
        if (*bridge) {
            return cli::cmd_bridge(bridge_flags);
        }
        if (*record) {
            return cli::cmd_record(record_flags);
        }
        if (*monitor) {
            if (mon.replay.empty() && (mon.a.empty() || mon.b.empty())) {
                cli::println(stderr, "monitor: give --a and --b, or --replay");
                return 2;
            }
            return cli::cmd_monitor(mon);
        }
    } catch (const CLI::Error& e) {
        return app.exit(e);
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fputs("lisbench: ", stderr);
        std::fputs(e.what(), stderr);
        std::fputs("\n", stderr);
        return 2;
    }
}

// lisbench command line: frame, deframe, decode and validate LIS01-A2 /
// LIS02-A2 data offline.

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

#include "lisbench/lis01_frame.hpp"
#include "lisbench/lis01_link.hpp"
#include "lisbench/lis02_message.hpp"
#include "lisbench/printable.hpp"

namespace {

using namespace lisbench;

// std::println needs GCC 14; this is the one stand-in until GCC 13 is dropped.
template <typename... Args>
void println(std::FILE* to, std::format_string<Args...> fmt, Args&&... args) {
    const auto text = std::format(fmt, std::forward<Args>(args)...) + '\n';
    std::fwrite(text.data(), 1, text.size(), to);
}
template <typename... Args>
void println(std::format_string<Args...> fmt, Args&&... args) {
    println(stdout, fmt, std::forward<Args>(args)...);
}

std::string read_input(const std::string& path) {
    if (path == "-") {
        return {std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw CLI::ValidationError("input", "cannot open " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool looks_framed(std::string_view data) {
    return data.find('\x02') != std::string_view::npos;
}

struct TextFlags {
    std::string charset{"latin1"};
    std::string non_ascii{"ascii-hex"};
    bool strict{false};

    void add_to(CLI::App& cmd) {
        cmd.add_option("--charset", charset, "Wire character set")
            ->check(CLI::IsMember({"latin1", "utf8"}))
            ->capture_default_str();
        cmd.add_option("--non-ascii", non_ascii, "Non-ASCII handling when encoding")
            ->check(CLI::IsMember({"ascii-hex", "raw", "transliterate"}))
            ->capture_default_str();
        cmd.add_flag("--strict", strict, "Reject unknown escapes, record types, invalid bytes");
    }

    [[nodiscard]] lis02::TextOptions options() const {
        static const std::map<std::string, lis02::NonAscii> modes{
            {"ascii-hex", lis02::NonAscii::ascii_hex},
            {"raw", lis02::NonAscii::raw},
            {"transliterate", lis02::NonAscii::transliterate},
        };
        return {.charset = charset == "utf8" ? lis02::Charset::utf8 : lis02::Charset::latin1,
                .non_ascii = modes.at(non_ascii),
                .strict = strict};
    }
};

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

int run(int argc, char** argv) {
    CLI::App app{"lisbench - tools for LIS01-A2 / LIS02-A2 (ASTM E1381 / E1394) interfaces"};
    app.require_subcommand(1);
    app.set_version_flag("--version", "lisbench 0.1.0");

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

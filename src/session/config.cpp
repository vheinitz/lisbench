#include "lisbench/session/config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

#include "lisbench/sim/scenario.hpp"
#include "yaml_util.hpp"

namespace lisbench::session {

using namespace detail;

namespace {

milliseconds duration(const YAML::Node& map, const char* key, milliseconds fallback) {
    const auto n = map[key];
    if (!n || n.IsNull()) {
        return fallback;
    }
    const auto d = sim::parse_duration(get<std::string>(map, key, ""));
    if (!d) {
        fail(n, std::format("'{}': {}", key, d.error()));
    }
    return *d;
}

std::string duration_text(milliseconds d) {
    if (d.count() % 1000 == 0) {
        return std::format("{}s", d.count() / 1000);
    }
    return std::format("{}ms", d.count());
}

LinkConfig link_of(const YAML::Node& n) {
    // label: written for the UI (config_json), derived, ignored when read back.
    only_keys(n, "a link", {"role", "transport", "low_level", "serial", "tcp", "label"});
    LinkConfig l;
    l.role = get<std::string>(n, "role", "main");
    l.low_level = choice(
        n, "low_level", LowLevel::lis01_a2,
        {{"lis01-a2", LowLevel::lis01_a2}, {"none", LowLevel::none}, {"mllp", LowLevel::mllp}});
    l.transport = choice(n, "transport", Transport::tcp_server,
                         {{"serial", Transport::serial},
                          {"tcp-server", Transport::tcp_server},
                          {"tcp-client", Transport::tcp_client}});
    if (l.transport == Transport::serial) {
        const auto s = n["serial"];
        if (!s) {
            fail(n, "a serial link needs 'serial: {port: ...}'");
        }
        only_keys(s, "serial", {"port", "baud", "data_bits", "parity", "stop_bits", "flow"});
        l.serial.device = required<std::string>(s, "port");
        l.serial.baud = get<unsigned>(s, "baud", 9600);
        l.serial.data_bits = get<unsigned>(s, "data_bits", 8);
        if (l.serial.data_bits < 5 || l.serial.data_bits > 8) {
            fail(s["data_bits"], "data_bits is 5..8");
        }
        l.serial.parity = choice(
            s, "parity", io::Parity::none,
            {{"none", io::Parity::none}, {"odd", io::Parity::odd}, {"even", io::Parity::even}});
        const auto stop = get<double>(s, "stop_bits", 1);
        if (stop == 1) {
            l.serial.stop_bits = io::StopBits::one;
        } else if (stop == 1.5) {
            l.serial.stop_bits = io::StopBits::one_half;
        } else if (stop == 2) {
            l.serial.stop_bits = io::StopBits::two;
        } else {
            fail(s["stop_bits"], "stop_bits is 1, 1.5 or 2");
        }
        l.serial.flow = choice(s, "flow", io::FlowControl::none,
                               {{"none", io::FlowControl::none},
                                {"software", io::FlowControl::software},
                                {"hardware", io::FlowControl::hardware}});
    } else {
        const auto t = n["tcp"];
        if (!t) {
            fail(n, "a TCP link needs 'tcp: {port: ...}'");
        }
        only_keys(t, "tcp", {"address", "host", "port"});
        if (l.transport == Transport::tcp_server) {
            l.host = get<std::string>(t, "address", "0.0.0.0");
        } else {
            l.host = required<std::string>(t, "host");
        }
        const auto port = required<int>(t, "port");
        if (port < 1 || port > 65535) {
            fail(t["port"], "port is 1..65535");
        }
        l.port = static_cast<std::uint16_t>(port);
    }
    return l;
}

char parity_letter(io::Parity p) {
    switch (p) {
        case io::Parity::none: return 'N';
        case io::Parity::odd: return 'O';
        case io::Parity::even: return 'E';
    }
    return '?';
}

std::string_view parity_name(io::Parity p) {
    switch (p) {
        case io::Parity::none: return "none";
        case io::Parity::odd: return "odd";
        case io::Parity::even: return "even";
    }
    return "?";
}

std::string_view stop_text(io::StopBits s) {
    switch (s) {
        case io::StopBits::one: return "1";
        case io::StopBits::one_half: return "1.5";
        case io::StopBits::two: return "2";
    }
    return "?";
}

std::string_view flow_name(io::FlowControl f) {
    switch (f) {
        case io::FlowControl::none: return "none";
        case io::FlowControl::software: return "software";
        case io::FlowControl::hardware: return "hardware";
    }
    return "?";
}

std::string_view unsolicited_name(Unsolicited u) {
    switch (u) {
        case Unsolicited::accept: return "accept";
        case Unsolicited::flag: return "flag";
        case Unsolicited::ignore: return "ignore";
    }
    return "?";
}

}  // namespace

std::string_view to_string(SessionType t) noexcept {
    return t == SessionType::lis ? "lis" : "device";
}

std::string_view to_string(Transport t) noexcept {
    switch (t) {
        case Transport::serial: return "serial";
        case Transport::tcp_server: return "tcp-server";
        case Transport::tcp_client: return "tcp-client";
    }
    return "?";
}

io::Endpoint LinkConfig::endpoint() const {
    if (transport == Transport::serial) {
        auto s = serial;
        s.low_level = low_level;
        return s;
    }
    return io::Tcp{.host = host,
                   .port = port,
                   .listen = transport == Transport::tcp_server,
                   .low_level = low_level};
}

std::string LinkConfig::label() const {
    const auto carried = [this]() -> std::string_view {
        switch (low_level) {
            case LowLevel::lis01_a2: return "";
            case LowLevel::none: return ", without LIS01-A2";
            case LowLevel::mllp: return ", MLLP";
        }
        return "";
    }();
    return std::format("{}{}", transport_label(), carried);
}

std::string LinkConfig::transport_label() const {
    switch (transport) {
        case Transport::serial: {
            return std::format("serial {} {} {}{}{}", serial.device, serial.baud, serial.data_bits,
                               parity_letter(serial.parity), stop_text(serial.stop_bits));
        }
        case Transport::tcp_server: return std::format("waits on port {} ({})", port, host);
        case Transport::tcp_client: return std::format("connects to {}:{}", host, port);
    }
    return "?";
}

bool valid_session_name(std::string_view name) noexcept {
    return !name.empty() && name.size() <= 64 && name.front() != '.' &&
           std::ranges::all_of(name, [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' ||
                      c == '.';
           });
}

std::expected<SessionConfig, std::string> parse_session(std::string_view yaml) {
    try {
        const auto root = YAML::Load(std::string{yaml});
        only_keys(root, "the session",
                  {"lisbench", "name", "type", "profile", "timing", "links", "lis", "device"});
        if (get<std::string>(root, "lisbench", "") != "session/1") {
            fail(root, "a session file starts with 'lisbench: session/1'");
        }
        SessionConfig c;
        c.name = required<std::string>(root, "name");
        if (!valid_session_name(c.name)) {
            fail(root["name"], "a name has letters, digits, '.', '_' or '-' (at most 64)");
        }
        c.type = choice(root, "type", SessionType::lis,
                        {{"lis", SessionType::lis}, {"device", SessionType::device}});
        c.profile = get<std::string>(root, "profile", "");
        c.fast_timing = choice(root, "timing", false, {{"lis01-a2", false}, {"fast-test", true}});
        if (!root["links"] || root["links"].size() == 0) {
            fail(root, "a session needs at least one link");
        }
        std::set<std::string> roles;
        for (const auto& n : root["links"]) {
            auto l = link_of(n);
            const bool known = l.role == "main" || (c.type == SessionType::lis &&
                                                    (l.role == "orders" || l.role == "results"));
            if (!known) {
                fail(n, std::format("role '{}' is not valid here (main{})", l.role,
                                    c.type == SessionType::lis ? ", orders, results" : ""));
            }
            if (!roles.insert(l.role).second) {
                fail(n, std::format("role '{}' appears twice", l.role));
            }
            c.links.push_back(std::move(l));
        }
        if (roles.contains("main") && roles.size() > 1) {
            fail(root["links"], "use either one 'main' link or 'orders' and 'results'");
        }
        if (roles.contains("orders") != roles.contains("results")) {
            fail(root["links"], "'orders' and 'results' come together");
        }
        if (const auto l = root["lis"]) {
            if (c.type != SessionType::lis) {
                fail(l, "'lis' belongs to a session of type lis");
            }
            only_keys(l, "lis", {"unsolicited", "answer_queries", "no_order_reply"});
            c.lis.unsolicited = choice(l, "unsolicited", Unsolicited::flag,
                                       {{"accept", Unsolicited::accept},
                                        {"flag", Unsolicited::flag},
                                        {"ignore", Unsolicited::ignore}});
            c.lis.answer_queries = get<bool>(l, "answer_queries", true);
            c.lis.no_order_reply =
                choice(l, "no_order_reply", NoOrderReply::y_record,
                       {{"y-record", NoOrderReply::y_record}, {"empty", NoOrderReply::empty}});
        }
        if (const auto d = root["device"]) {
            if (c.type != SessionType::device) {
                fail(d, "'device' belongs to a session of type device");
            }
            only_keys(d, "device",
                      {"mode", "specimens", "tests", "interval", "query_timeout", "measure_time"});
            c.device.mode = choice(
                d, "mode", DeviceMode::unsolicited,
                {{"unsolicited", DeviceMode::unsolicited}, {"host-query", DeviceMode::host_query}});
            if (const auto s = d["specimens"]) {
                if (s.IsSequence()) {
                    c.device.specimens = get<std::vector<std::string>>(d, "specimens", {});
                } else {
                    only_keys(s, "specimens", {"prefix", "start"});
                    c.device.prefix = get<std::string>(s, "prefix", "S-");
                    c.device.start = get<unsigned>(s, "start", 1);
                }
            }
            c.device.tests = get<std::vector<std::string>>(d, "tests", {});
            c.device.interval = duration(d, "interval", c.device.interval);
            c.device.query_timeout = duration(d, "query_timeout", c.device.query_timeout);
            c.device.measure_time = duration(d, "measure_time", c.device.measure_time);
            if (c.device.mode == DeviceMode::unsolicited && c.device.tests.empty()) {
                fail(d, "mode unsolicited needs 'tests' to report");
            }
        } else if (c.type == SessionType::device) {
            fail(root, "a device session needs 'device: {mode: ...}'");
        }
        return c;
    } catch (const FileError& e) {
        return std::unexpected(e.what());
    } catch (const YAML::Exception& e) {
        return std::unexpected(std::format("line {}: {}", e.mark.line + 1, e.msg));
    }
}

std::expected<SessionConfig, std::string> load_session(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("cannot open {}", file.string()));
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto c = parse_session(text);
    if (!c) {
        return std::unexpected(std::format("{}: {}", file.filename().string(), c.error()));
    }
    return c;
}

std::string to_yaml(const SessionConfig& c) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "lisbench" << YAML::Value << "session/1";
    out << YAML::Key << "name" << YAML::Value << c.name;
    out << YAML::Key << "type" << YAML::Value << std::string{to_string(c.type)};
    if (!c.profile.empty()) {
        out << YAML::Key << "profile" << YAML::Value << c.profile;
    }
    out << YAML::Key << "timing" << YAML::Value << (c.fast_timing ? "fast-test" : "lis01-a2");
    out << YAML::Key << "links" << YAML::Value << YAML::BeginSeq;
    for (const auto& l : c.links) {
        out << YAML::BeginMap;
        out << YAML::Key << "role" << YAML::Value << l.role;
        out << YAML::Key << "transport" << YAML::Value << std::string{to_string(l.transport)};
        out << YAML::Key << "low_level" << YAML::Value << std::string{to_string(l.low_level)};
        if (l.transport == Transport::serial) {
            const auto& s = l.serial;
            out << YAML::Key << "serial" << YAML::Value << YAML::Flow << YAML::BeginMap;
            out << YAML::Key << "port" << YAML::Value << s.device;
            out << YAML::Key << "baud" << YAML::Value << s.baud;
            out << YAML::Key << "data_bits" << YAML::Value << s.data_bits;
            out << YAML::Key << "parity" << YAML::Value << std::string{parity_name(s.parity)};
            // A number in the file (1, 1.5, 2), as a person writes it.
            out << YAML::Key << "stop_bits" << YAML::Value
                << YAML::Load(std::string{stop_text(s.stop_bits)});
            out << YAML::Key << "flow" << YAML::Value << std::string{flow_name(s.flow)};
            out << YAML::EndMap;
        } else {
            out << YAML::Key << "tcp" << YAML::Value << YAML::Flow << YAML::BeginMap;
            out << YAML::Key << (l.transport == Transport::tcp_server ? "address" : "host")
                << YAML::Value << l.host;
            out << YAML::Key << "port" << YAML::Value << l.port << YAML::EndMap;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    if (c.type == SessionType::lis) {
        out << YAML::Key << "lis" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "unsolicited" << YAML::Value
            << std::string{unsolicited_name(c.lis.unsolicited)};
        out << YAML::Key << "answer_queries" << YAML::Value << c.lis.answer_queries;
        out << YAML::Key << "no_order_reply" << YAML::Value
            << (c.lis.no_order_reply == NoOrderReply::y_record ? "y-record" : "empty");
        out << YAML::EndMap;
    } else {
        const auto& d = c.device;
        out << YAML::Key << "device" << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "mode" << YAML::Value
            << (d.mode == DeviceMode::unsolicited ? "unsolicited" : "host-query");
        if (d.specimens.empty()) {
            out << YAML::Key << "specimens" << YAML::Value << YAML::Flow << YAML::BeginMap
                << YAML::Key << "prefix" << YAML::Value << d.prefix << YAML::Key << "start"
                << YAML::Value << d.start << YAML::EndMap;
        } else {
            out << YAML::Key << "specimens" << YAML::Value << YAML::Flow << d.specimens;
        }
        if (!d.tests.empty()) {
            out << YAML::Key << "tests" << YAML::Value << YAML::Flow << d.tests;
        }
        out << YAML::Key << "interval" << YAML::Value << duration_text(d.interval);
        out << YAML::Key << "query_timeout" << YAML::Value << duration_text(d.query_timeout);
        out << YAML::Key << "measure_time" << YAML::Value << duration_text(d.measure_time);
        out << YAML::EndMap;
    }
    out << YAML::EndMap;
    return std::string{out.c_str()} + "\n";
}

nlohmann::json config_json(const SessionConfig& c) {
    using nlohmann::json;
    json links = json::array();
    for (const auto& l : c.links) {
        json link = {{"role", l.role},
                     {"transport", to_string(l.transport)},
                     {"low_level", to_string(l.low_level)}};
        if (l.transport == Transport::serial) {
            const auto stop = stop_text(l.serial.stop_bits);
            link["serial"] = {
                {"port", l.serial.device},
                {"baud", l.serial.baud},
                {"data_bits", l.serial.data_bits},
                {"parity", parity_name(l.serial.parity)},
                {"stop_bits", stop == "1.5" ? json(1.5) : json(std::stoi(std::string{stop}))},
                {"flow", flow_name(l.serial.flow)}};
        } else {
            link["tcp"] = {{l.transport == Transport::tcp_server ? "address" : "host", l.host},
                           {"port", l.port}};
        }
        link["label"] = l.label();
        links.push_back(std::move(link));
    }
    json j = {{"lisbench", "session/1"},
              {"name", c.name},
              {"type", to_string(c.type)},
              {"profile", c.profile},
              {"timing", c.fast_timing ? "fast-test" : "lis01-a2"},
              {"links", std::move(links)}};
    if (c.type == SessionType::lis) {
        j["lis"] = {{"unsolicited", unsolicited_name(c.lis.unsolicited)},
                    {"answer_queries", c.lis.answer_queries},
                    {"no_order_reply",
                     c.lis.no_order_reply == NoOrderReply::y_record ? "y-record" : "empty"}};
    } else {
        const auto& d = c.device;
        json specimens = d.specimens.empty() ? json{{"prefix", d.prefix}, {"start", d.start}}
                                             : json(d.specimens);
        j["device"] = {{"mode", d.mode == DeviceMode::unsolicited ? "unsolicited" : "host-query"},
                       {"specimens", std::move(specimens)},
                       {"tests", d.tests},
                       {"interval", duration_text(d.interval)},
                       {"query_timeout", duration_text(d.query_timeout)},
                       {"measure_time", duration_text(d.measure_time)}};
    }
    return j;
}

}  // namespace lisbench::session

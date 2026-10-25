#include "lisbench/io/endpoint.hpp"

#include <array>
#include <charconv>
#include <format>
#include <ranges>

namespace lisbench::io {

namespace {

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

std::expected<unsigned, std::string> to_unsigned(std::string_view text, std::string_view what) {
    unsigned value = 0;
    const auto* end = text.data() + text.size();
    // from_chars is bounded by end; no terminator is needed.
    const auto [ptr, ec] = std::from_chars(
        text.data(), end, value);  // NOLINT(bugprone-suspicious-stringview-data-usage)
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected(std::format("{} '{}' is not a number", what, text));
    }
    return value;
}

std::expected<Endpoint, std::string> parse_tcp(std::string_view rest, bool listen) {
    const auto colon = rest.rfind(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(std::format("missing port in '{}'", rest));
    }
    auto host = rest.substr(0, colon);
    if (host.empty()) {
        host = listen ? "0.0.0.0" : "";
    }
    if (host.empty()) {
        return std::unexpected("missing host");
    }
    const auto port = to_unsigned(rest.substr(colon + 1), "port");
    if (!port) {
        return std::unexpected(port.error());
    }
    if (*port == 0 && !listen) {
        return std::unexpected("port 0 is only valid for tcp-listen");
    }
    if (*port > 65535) {
        return std::unexpected("port out of range");
    }
    return Tcp{
        .host = std::string{host}, .port = static_cast<std::uint16_t>(*port), .listen = listen};
}

std::expected<Endpoint, std::string> parse_serial(std::string_view rest) {
    const auto query_at = rest.find('?');
    Serial s{.device = std::string{rest.substr(0, query_at)}};
    if (s.device.empty()) {
        return std::unexpected("missing serial device");
    }
    if (query_at == std::string_view::npos) {
        return s;
    }
    for (auto part : rest.substr(query_at + 1) | std::views::split('&')) {
        const std::string_view kv(part.begin(), part.end());
        const auto eq = kv.find('=');
        if (eq == std::string_view::npos) {
            return std::unexpected(std::format("expected key=value, got '{}'", kv));
        }
        const auto key = kv.substr(0, eq);
        const auto value = kv.substr(eq + 1);
        if (key == "baud" || key == "data") {
            const auto n = to_unsigned(value, key);
            if (!n) {
                return std::unexpected(n.error());
            }
            if (key == "baud") {
                s.baud = *n;
            } else if (*n >= 5 && *n <= 8) {
                s.data_bits = *n;
            } else {
                return std::unexpected("data bits must be 5..8");
            }
        } else if (key == "parity") {
            if (value == "none") {
                s.parity = Parity::none;
            } else if (value == "odd") {
                s.parity = Parity::odd;
            } else if (value == "even") {
                s.parity = Parity::even;
            } else {
                return std::unexpected(std::format("unknown parity '{}'", value));
            }
        } else if (key == "stop") {
            if (value == "1") {
                s.stop_bits = StopBits::one;
            } else if (value == "1.5") {
                s.stop_bits = StopBits::one_half;
            } else if (value == "2") {
                s.stop_bits = StopBits::two;
            } else {
                return std::unexpected(std::format("unknown stop bits '{}'", value));
            }
        } else if (key == "flow") {
            if (value == "none") {
                s.flow = FlowControl::none;
            } else if (value == "xonxoff") {
                s.flow = FlowControl::software;
            } else if (value == "rtscts") {
                s.flow = FlowControl::hardware;
            } else {
                return std::unexpected(std::format("unknown flow control '{}'", value));
            }
        } else {
            return std::unexpected(std::format("unknown serial option '{}'", key));
        }
    }
    return s;
}

}  // namespace

std::expected<Endpoint, std::string> parse_endpoint(std::string_view uri) {
    if (uri.starts_with("tcp://")) {
        return parse_tcp(uri.substr(6), false);
    }
    if (uri.starts_with("tcp-listen://")) {
        return parse_tcp(uri.substr(13), true);
    }
    if (uri.starts_with("serial:")) {
        auto rest = uri.substr(7);
        if (rest.starts_with("//")) {
            rest.remove_prefix(2);
        }
        return parse_serial(rest);
    }
    return std::unexpected(
        std::format("unknown endpoint '{}' (tcp://, tcp-listen://, serial:)", uri));
}

std::string to_string(const Endpoint& endpoint) {
    return std::visit(
        overloaded{
            [](const Tcp& t) {
                return std::format("{}://{}:{}", t.listen ? "tcp-listen" : "tcp", t.host, t.port);
            },
            [](const Serial& s) {
                constexpr std::array parity{"none", "odd", "even"};
                constexpr std::array stop{"1", "1.5", "2"};
                constexpr std::array flow{"none", "xonxoff", "rtscts"};
                return std::format("serial:{}?baud={}&data={}&parity={}&stop={}&flow={}", s.device,
                                   s.baud, s.data_bits,
                                   parity.at(static_cast<std::size_t>(s.parity)),
                                   stop.at(static_cast<std::size_t>(s.stop_bits)),
                                   flow.at(static_cast<std::size_t>(s.flow)));
            },
        },
        endpoint);
}

}  // namespace lisbench::io

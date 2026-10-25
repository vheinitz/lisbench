#include "lisbench/web/http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>
#include <ranges>

namespace lisbench::web {

namespace {

std::string lower(std::string_view s) {
    std::string out{s};
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

int hex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string_view Request::header(std::string_view name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? std::string_view{} : std::string_view{it->second};
}

RequestParser::Status RequestParser::fail(int status, std::string why) {
    error_status_ = status;
    error_ = std::move(why);
    status_ = Status::error;
    return status_;
}

RequestParser::Status RequestParser::feed(std::string_view data) {
    if (status_ != Status::need_more) {
        return status_;
    }
    buffer_ += data;
    if (!body_start_) {
        const auto end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            if (buffer_.size() > limits_.head) {
                return fail(431, "request head too large");
            }
            return status_;
        }
        if (end > limits_.head) {
            return fail(431, "request head too large");
        }
        if (parse_head(std::string_view{buffer_}.substr(0, end)) == Status::error) {
            return status_;
        }
        body_start_ = end + 4;
    }
    if (buffer_.size() - *body_start_ >= content_length_) {
        request_.body = buffer_.substr(*body_start_, content_length_);
        status_ = Status::done;
    }
    return status_;
}

RequestParser::Status RequestParser::parse_head(std::string_view head) {
    auto lines = head | std::views::split(std::string_view{"\r\n"});
    auto it = lines.begin();
    const std::string_view request_line{(*it).begin(), (*it).end()};
    const auto sp1 = request_line.find(' ');
    const auto sp2 = request_line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1) {
        return fail(400, "malformed request line");
    }
    request_.method = std::string{request_line.substr(0, sp1)};
    request_.target = std::string{request_line.substr(sp1 + 1, sp2 - sp1 - 1)};
    const auto version = request_line.substr(sp2 + 1);
    if (!version.starts_with("HTTP/1.")) {
        return fail(400, "only HTTP/1.x");
    }
    if (request_.target.empty() || request_.target.front() != '/') {
        return fail(400, "target must be an absolute path");
    }
    const auto q = request_.target.find('?');
    auto path = url_decode(std::string_view{request_.target}.substr(0, q), false);
    if (!path || path->contains('\0')) {
        return fail(400, "malformed path");
    }
    request_.path = std::move(*path);
    if (q != std::string::npos) {
        for (auto part : std::string_view{request_.target}.substr(q + 1) | std::views::split('&')) {
            const std::string_view kv{part.begin(), part.end()};
            if (kv.empty()) {
                continue;
            }
            const auto eq = kv.find('=');
            auto key = url_decode(kv.substr(0, eq), true);
            auto value = url_decode(eq == std::string_view::npos ? "" : kv.substr(eq + 1), true);
            if (!key || !value) {
                return fail(400, "malformed query");
            }
            request_.query[std::move(*key)] = std::move(*value);
        }
    }
    for (++it; it != lines.end(); ++it) {
        const std::string_view line{(*it).begin(), (*it).end()};
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return fail(400, "malformed header");
        }
        request_.headers[lower(line.substr(0, colon))] = std::string{trim(line.substr(colon + 1))};
    }
    if (!request_.header("transfer-encoding").empty()) {
        return fail(501, "chunked request bodies are not supported");
    }
    if (const auto cl = request_.header("content-length"); !cl.empty()) {
        std::size_t n = 0;
        const auto [p, ec] = std::from_chars(cl.data(), cl.data() + cl.size(), n);
        if (ec != std::errc{} || p != cl.data() + cl.size()) {
            return fail(400, "bad Content-Length");
        }
        if (n > limits_.body) {
            return fail(413, "request body too large");
        }
        content_length_ = n;
    }
    return status_;
}

std::string_view reason(int status) noexcept {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string serialize(const Response& r) {
    std::string out = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n",
                                  r.status, reason(r.status), r.content_type, r.body.size());
    for (const auto& [name, value] : r.headers) {
        out += std::format("{}: {}\r\n", name, value);
    }
    out +=
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n";
    out += r.body;
    return out;
}

Response json_response(std::string body, int status) {
    return {.status = status,
            .content_type = "application/json",
            .body = std::move(body),
            .headers = {}};
}

Response error(int status, std::string_view message) {
    std::string escaped;
    for (const char c : message) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            escaped += c;
        }
    }
    return json_response(std::format(R"({{"error":"{}"}})", escaped), status);
}

std::optional<std::string> url_decode(std::string_view s, bool plus_is_space) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) {
                return std::nullopt;  // needs two hex digits
            }
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi < 0 || lo < 0) {
                return std::nullopt;
            }
            out += static_cast<char>((hi << 4) | lo);
            i += 2;
        } else if (s[i] == '+' && plus_is_space) {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string sse_event(std::string_view event, std::string_view data) {
    std::string out = std::format("event: {}\n", event);
    for (auto line : data | std::views::split('\n')) {
        out += "data: ";
        out.append(line.begin(), line.end());
        out += '\n';
    }
    out += '\n';
    return out;
}

}  // namespace lisbench::web

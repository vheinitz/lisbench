#pragma once

// The small HTTP/1.1 subset the web UI needs: requests with Content-Length
// bodies (no chunked uploads, no pipelining), responses with a body, and
// Server-Sent Events. Pure: no I/O here, see web/server.hpp.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lisbench::web {

struct Limits {
    std::size_t head{std::size_t{8} * 1024};  // request line and headers
    std::size_t body{std::size_t{1024} * 1024};
};

struct Request {
    std::string method{};
    std::string target{};  // as sent: path and query
    std::string path{};    // decoded, without query
    std::map<std::string, std::string> query{};
    std::map<std::string, std::string> headers{};  // names in lower case
    std::string body{};

    [[nodiscard]] std::string_view header(std::string_view name) const;
};

class RequestParser {
public:
    enum class Status : std::uint8_t { need_more, done, error };

    explicit RequestParser(Limits limits = {}) : limits_{limits} {}

    // Appends data; done once a whole request is there. Bytes after it are
    // ignored (one request per connection).
    Status feed(std::string_view data);

    [[nodiscard]] const Request& request() const noexcept { return request_; }
    // On error: the status to answer with (400, 413, 431, 501) and why.
    [[nodiscard]] int error_status() const noexcept { return error_status_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    Status fail(int status, std::string why);
    Status parse_head(std::string_view head);

    Limits limits_;
    std::string buffer_{};
    std::optional<std::size_t> body_start_{};
    std::size_t content_length_{0};
    Request request_{};
    int error_status_{0};
    std::string error_{};
    Status status_{Status::need_more};
};

struct Response {
    int status{200};
    std::string content_type{"text/plain; charset=utf-8"};
    std::string body{};
    std::vector<std::pair<std::string, std::string>> headers{};
};

[[nodiscard]] std::string_view reason(int status) noexcept;
// Status line, headers (with Content-Length, Connection: close, and
// Cache-Control: no-store unless r.headers has a Cache-Control) and body.
[[nodiscard]] std::string serialize(const Response& r);

[[nodiscard]] Response json_response(std::string body, int status = 200);
[[nodiscard]] Response error(int status, std::string_view message);

// %XX and '+' (in queries) decoded; nullopt for a malformed escape.
[[nodiscard]] std::optional<std::string> url_decode(std::string_view s, bool plus_is_space);

// One Server-Sent Events message; `data` may contain newlines.
[[nodiscard]] std::string sse_event(std::string_view event, std::string_view data);

}  // namespace lisbench::web

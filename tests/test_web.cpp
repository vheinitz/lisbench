#include <catch2/catch_test_macros.hpp>

#include <asio/experimental/awaitable_operators.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

#include "lisbench/lis02_records.hpp"
#include "lisbench/web/http.hpp"
#include "lisbench/web/server.hpp"

using namespace lisbench;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;
using Status = web::RequestParser::Status;

TEST_CASE("http: a request with query and body, fed in pieces") {
    web::RequestParser p;
    const std::string text =
        "POST /api/monitor%2Fstart?a=tcp%3A%2F%2Fx%3A1&watch=R.value+%3E+1 HTTP/1.1\r\n"
        "Host: localhost\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"x\":1}";
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        REQUIRE(p.feed(text.substr(i, 1)) == Status::need_more);
    }
    REQUIRE(p.feed(text.substr(text.size() - 1)) == Status::done);
    const auto& r = p.request();
    CHECK(r.method == "POST");
    CHECK(r.path == "/api/monitor/start");
    CHECK(r.query.at("a") == "tcp://x:1");
    CHECK(r.query.at("watch") == "R.value > 1");
    CHECK(r.header("CONTENT-TYPE") == "application/json");
    CHECK(r.body == "{\"x\":1}");
}

TEST_CASE("http: malformed and oversized requests get the right status") {
    const auto status_of = [](std::string_view text, web::Limits limits = {}) {
        web::RequestParser p{limits};
        return p.feed(text) == Status::error ? p.error_status() : 0;
    };
    CHECK(status_of("GARBAGE\r\n\r\n") == 400);
    CHECK(status_of("GET x HTTP/1.1\r\n\r\n") == 400);  // not an absolute path
    CHECK(status_of("GET / SPDY/3\r\n\r\n") == 400);
    CHECK(status_of("GET /%zz HTTP/1.1\r\n\r\n") == 400);    // bad escape
    CHECK(status_of("GET /a%00b HTTP/1.1\r\n\r\n") == 400);  // NUL in the path
    CHECK(status_of("GET / HTTP/1.1\r\nNoColon\r\n\r\n") == 400);
    CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 12x\r\n\r\n") == 400);
    CHECK(status_of("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == 501);
    CHECK(status_of("POST / HTTP/1.1\r\nContent-Length: 100\r\n\r\n", {.head = 8192, .body = 10}) ==
          413);
    CHECK(status_of("GET / HTTP/1.1\r\nX: " + std::string(9000, 'a'), {}) == 431);  // no end yet
    CHECK(web::url_decode("%4", false) == std::nullopt);
    CHECK(web::url_decode("a+b", false) == "a+b");
    CHECK(web::url_decode("a+b", true) == "a b");
}

TEST_CASE("http: responses and events") {
    const auto out = web::serialize(web::json_response(R"({"ok":true})"));
    CHECK(out.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(out.contains("Content-Type: application/json\r\n"));
    CHECK(out.contains("Content-Length: 11\r\n"));
    CHECK(out.ends_with("\r\n\r\n{\"ok\":true}"));
    // Outside the macro: MSVC mangles raw string literals in stringized macro arguments.
    const std::string escaped = R"({"error":"say \"no\""})";
    CHECK(web::error(404, "say \"no\"\n").body == escaped);
    CHECK(web::sse_event("record", "a\nb") == "event: record\ndata: a\ndata: b\n\n");
}

namespace {

// One request over a real socket; returns everything the server sent.
asio::awaitable<std::string> fetch(std::uint16_t port, std::string request) {
    asio::ip::tcp::socket s{co_await asio::this_coro::executor};
    co_await s.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
    co_await asio::async_write(s, asio::buffer(request), asio::use_awaitable);
    std::string out;
    std::array<char, 1024> buf{};
    while (true) {
        const auto [ec, n] =
            co_await s.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;
        }
        out.append(buf.data(), n);
    }
    co_return out;
}

}  // namespace

TEST_CASE("server: routes, files, 404/405, and events to every stream") {
    asio::io_context io;
    web::Server server{io.get_executor(), {asio::ip::make_address("127.0.0.1"), 0}};
    server.route("GET", "/api/hello", [](web::Request r) -> asio::awaitable<web::Response> {
        co_return web::json_response(
            std::format(R"({{"name":"{}"}})", r.query.contains("n") ? r.query.at("n") : ""));
    });
    server.route("POST", "/api/fail", [](web::Request) -> asio::awaitable<web::Response> {
        throw std::runtime_error("broken handler");
        co_return web::json_response("{}");
    });
    static constexpr std::string_view page = "<!doctype html><title>t</title>";
    server.file("/index.html", "text/html; charset=utf-8", page);
    const auto port = server.port();
    asio::co_spawn(io, server.run(), asio::detached);

    std::map<std::string, std::string> got;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            got["hello"] = co_await fetch(port, "GET /api/hello?n=lis HTTP/1.1\r\n\r\n");
            got["index"] = co_await fetch(port, "GET / HTTP/1.1\r\n\r\n");
            // The browser asks again with the ETag it got: 304, no body.
            const auto etag_at = got["index"].find("ETag: ");
            const auto etag =
                got["index"].substr(etag_at + 6, got["index"].find("\r\n", etag_at) - etag_at - 6);
            got["304"] = co_await fetch(
                port, std::format("GET /index.html HTTP/1.1\r\nIf-None-Match: {}\r\n\r\n", etag));
            got["404"] = co_await fetch(port, "GET /nope HTTP/1.1\r\n\r\n");
            got["405"] = co_await fetch(port, "DELETE /api/hello HTTP/1.1\r\n\r\n");
            got["500"] =
                co_await fetch(port, "POST /api/fail HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
            got["400"] = co_await fetch(port, "nonsense\r\n\r\n");

            // Two event streams, then one event: both get it.
            const auto executor = co_await asio::this_coro::executor;
            std::array<asio::ip::tcp::socket, 2> sse{asio::ip::tcp::socket{executor},
                                                     asio::ip::tcp::socket{executor}};
            for (auto& s : sse) {
                co_await s.async_connect({asio::ip::make_address("127.0.0.1"), port},
                                         asio::use_awaitable);
                co_await asio::async_write(
                    s, asio::buffer(std::string_view{"GET /api/events HTTP/1.1\r\n\r\n"}),
                    asio::use_awaitable);
            }
            asio::steady_timer t{executor, 100ms};
            co_await t.async_wait(asio::use_awaitable);
            got["streams"] = std::to_string(server.streams());
            server.publish("status", R"({"running":false})");
            for (std::size_t i = 0; i < sse.size(); ++i) {
                std::string text;
                std::array<char, 1024> buf{};
                while (!text.contains("\n\nevent: status")) {
                    const auto n =
                        co_await sse.at(i).async_read_some(asio::buffer(buf), asio::use_awaitable);
                    text.append(buf.data(), n);
                }
                asio::steady_timer more{executor, 50ms};
                co_await more.async_wait(asio::use_awaitable);
                const auto n =
                    sse.at(i).available() > 0 ? sse.at(i).read_some(asio::buffer(buf)) : 0;
                text.append(buf.data(), n);
                got["sse" + std::to_string(i)] = text;
            }
            server.stop();
        },
        [](const std::exception_ptr& e) { REQUIRE_FALSE(e); });
    io.run_for(10s);

    CHECK(got["hello"].starts_with("HTTP/1.1 200 OK"));
    CHECK(got["hello"].ends_with(R"({"name":"lis"})"));
    CHECK(got["index"].contains("Content-Type: text/html; charset=utf-8"));
    CHECK(got["index"].ends_with(page));
    CHECK(got["index"].contains("Cache-Control: no-cache\r\n"));
    CHECK_FALSE(got["index"].contains("no-store"));
    CHECK(got["304"].starts_with("HTTP/1.1 304 Not Modified"));
    CHECK(got["304"].ends_with("\r\n\r\n"));
    CHECK(got["hello"].contains("Cache-Control: no-store\r\n"));  // API: never cached
    CHECK(got["404"].starts_with("HTTP/1.1 404"));
    CHECK(got["405"].starts_with("HTTP/1.1 405"));
    CHECK(got["500"].starts_with("HTTP/1.1 500"));
    CHECK(got["500"].contains("broken handler"));
    CHECK(got["400"].starts_with("HTTP/1.1 400"));
    CHECK(got["streams"] == "2");
    for (const auto* key : {"sse0", "sse1"}) {
        INFO(key);
        CHECK(got[key].contains("Content-Type: text/event-stream"));
        CHECK(got[key].contains("event: status\ndata: {\"running\":false}\n\n"));
    }
}

TEST_CASE("knowledge page: every note names a field of the record table") {
    std::ifstream in{std::filesystem::path{LISBENCH_WEB_DIR} / "knowledge.js"};
    REQUIRE(in);
    const std::string js{std::istreambuf_iterator<char>{in}, {}};
    const std::regex key{R"re("([HPORCQLSM])\.([a-z_0-9]+)": \{)re"};
    std::size_t keys = 0;
    for (auto it = std::sregex_iterator{js.begin(), js.end(), key}; it != std::sregex_iterator{};
         ++it) {
        const auto type = lisbench::lis02::record_type((*it)[1].str().front());
        INFO((*it)[0].str());
        REQUIRE(type);
        CHECK(lisbench::lis02::field_number(*type, (*it)[2].str()));
        ++keys;
    }
    CHECK(keys > 40);
}

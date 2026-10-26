#include "lisbench/web/server.hpp"

#include <asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <cstdint>
#include <format>

namespace lisbench::web {

namespace {

using namespace asio::experimental::awaitable_operators;
constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

}  // namespace

Server::Server(const asio::any_io_executor& executor, const asio::ip::tcp::endpoint& at,
               ServerOptions options)
    : acceptor_{executor}, options_{options} {
    acceptor_.open(at.protocol());
    acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address{true});
    acceptor_.bind(at);
    acceptor_.listen();
}

void Server::route(std::string method, std::string path, Handler handler) {
    routes_.push_back({std::move(method), std::move(path), std::move(handler)});
}

void Server::file(std::string path, std::string content_type, std::string_view data) {
    std::uint64_t hash = 14695981039346656037ULL;  // FNV-1a
    for (const char c : data) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    files_[std::move(path)] = File{.content_type = std::move(content_type),
                                   .data = data,
                                   .etag = std::format("\"{:016x}\"", hash)};
}

void Server::publish(std::string_view event, std::string_view data) {
    const auto message = sse_event(event, data);
    for (const auto& s : streams_) {
        if (s->dropped) {
            continue;
        }
        if (s->queue.size() >= options_.max_queued) {
            // A reader that cannot keep up is let go; its page reconnects.
            s->dropped = true;
            std::error_code ec;
            s->socket.close(ec);
        } else {
            s->queue.push_back(message);
        }
        s->wake.cancel();
    }
}

asio::awaitable<void> Server::run() {
    while (!stopping_) {
        auto [ec, socket] = co_await acceptor_.async_accept(use_tuple);
        if (ec) {
            if (stopping_) {
                break;
            }
            continue;
        }
        asio::co_spawn(acceptor_.get_executor(), serve(std::move(socket)), asio::detached);
    }
}

void Server::stop() {
    stopping_ = true;
    std::error_code ec;
    acceptor_.close(ec);
    for (const auto& s : streams_) {
        s->socket.close(ec);
        s->wake.cancel();
    }
}

asio::awaitable<void> Server::serve(asio::ip::tcp::socket socket) {
    RequestParser parser{options_.limits};
    std::array<char, 4096> buffer{};
    asio::steady_timer deadline{socket.get_executor(), options_.head_timeout};
    auto status = RequestParser::Status::need_more;
    while (status == RequestParser::Status::need_more) {
        auto r = co_await (socket.async_read_some(asio::buffer(buffer), use_tuple) ||
                           deadline.async_wait(use_tuple));
        if (r.index() == 1) {
            const auto out = serialize(error(408, "request timeout"));
            (void)co_await asio::async_write(socket, asio::buffer(out), use_tuple);
            co_return;
        }
        const auto [ec, n] = std::get<0>(r);
        if (ec) {
            co_return;
        }
        status = parser.feed({buffer.data(), n});
    }
    if (status == RequestParser::Status::error) {
        const auto out = serialize(error(parser.error_status(), parser.error()));
        (void)co_await asio::async_write(socket, asio::buffer(out), use_tuple);
        co_return;
    }
    const auto& request = parser.request();
    if (request.method == "GET" && request.path == "/api/events") {
        if (streams_.size() >= options_.max_streams) {
            const auto out = serialize(error(503, "too many event streams"));
            (void)co_await asio::async_write(socket, asio::buffer(out), use_tuple);
            co_return;
        }
        auto s = std::make_shared<Stream>(Stream{
            .socket = std::move(socket), .wake = asio::steady_timer{acceptor_.get_executor()}});
        streams_.push_back(s);
        co_await stream(s);
        std::erase(streams_, s);
        co_return;
    }
    const auto response = co_await dispatch(request);
    const auto out = serialize(response);
    (void)co_await asio::async_write(socket, asio::buffer(out), use_tuple);
    std::error_code ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_send, ec);
}

asio::awaitable<Response> Server::dispatch(Request request) {
    bool path_known = false;
    for (const auto& r : routes_) {
        const bool match =
            r.path.ends_with('/') ? request.path.starts_with(r.path) : request.path == r.path;
        if (!match) {
            continue;
        }
        path_known = true;
        if (r.method == request.method) {
            try {
                co_return co_await r.handler(request);
            } catch (const std::exception& e) {
                co_return error(500, e.what());
            }
        }
    }
    if (request.method == "GET") {
        const auto path = request.path == "/" ? std::string{"/index.html"} : request.path;
        if (const auto it = files_.find(path); it != files_.end()) {
            // The files are built in, so they change only with the program:
            // the browser keeps them and asks with the ETag whether they did.
            std::vector<std::pair<std::string, std::string>> headers{{"ETag", it->second.etag},
                                                                     {"Cache-Control", "no-cache"}};
            if (request.header("if-none-match") == it->second.etag) {
                co_return Response{.status = 304,
                                   .content_type = it->second.content_type,
                                   .body = {},
                                   .headers = std::move(headers)};
            }
            co_return Response{.status = 200,
                               .content_type = it->second.content_type,
                               .body = std::string{it->second.data},
                               .headers = std::move(headers)};
        }
    }
    co_return path_known ? error(405, "method not allowed") : error(404, "not found");
}

asio::awaitable<void> Server::stream(std::shared_ptr<Stream> s) const {
    const std::string head =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\n"
        "Connection: keep-alive\r\n\r\n: lisbench\n\n";
    if (auto [ec, n] = co_await asio::async_write(s->socket, asio::buffer(head), use_tuple); ec) {
        co_return;
    }
    while (!stopping_ && !s->dropped) {
        while (!s->queue.empty()) {
            const auto message = std::move(s->queue.front());
            s->queue.pop_front();
            if (auto [ec, n] =
                    co_await asio::async_write(s->socket, asio::buffer(message), use_tuple);
                ec) {
                co_return;
            }
        }
        s->wake.expires_after(options_.keepalive);
        const auto [ec] = co_await s->wake.async_wait(use_tuple);
        if (!ec && s->queue.empty()) {
            // Keep-alive comment; also notices a reader that went away.
            static constexpr std::string_view ping = ": ping\n\n";
            if (auto [wec, n] =
                    co_await asio::async_write(s->socket, asio::buffer(ping), use_tuple);
                wec) {
                co_return;
            }
        }
    }
}

}  // namespace lisbench::web

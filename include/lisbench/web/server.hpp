#pragma once

// HTTP server for the web UI on asio coroutines: one request per connection,
// routes by method and path, files compiled into the binary, and a
// Server-Sent Events stream at /api/events that every view listens to.

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lisbench/web/http.hpp"

namespace lisbench::web {

struct ServerOptions {
    Limits limits{};
    std::size_t max_streams{16};
    std::size_t max_queued{2000};  // events per stream before a slow reader is dropped
    std::chrono::milliseconds head_timeout{10'000};
    std::chrono::milliseconds keepalive{15'000};  // SSE comment so proxies keep the stream open
};

class Server {
public:
    // Takes the request by value: a handler is a coroutine and may outlive a reference.
    using Handler = std::function<asio::awaitable<Response>(Request)>;

    Server(const asio::any_io_executor& executor, const asio::ip::tcp::endpoint& at,
           ServerOptions options = {});

    // Exact path, or a prefix when `path` ends with '/'. Later routes do not
    // override earlier ones.
    void route(std::string method, std::string path, Handler handler);
    // A file compiled into the binary; `data` must outlive the server.
    void file(std::string path, std::string content_type, std::string_view data);

    // Sends an event to every open /api/events stream.
    void publish(std::string_view event, std::string_view data);
    [[nodiscard]] std::size_t streams() const noexcept { return streams_.size(); }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
    [[nodiscard]] asio::any_io_executor executor() { return acceptor_.get_executor(); }
    asio::awaitable<void> run();
    void stop();

private:
    struct Stream {
        asio::ip::tcp::socket socket;
        asio::steady_timer wake;
        std::deque<std::string> queue{};
        bool dropped{false};
    };
    struct Route {
        std::string method;
        std::string path;
        Handler handler;
    };
    struct File {
        std::string content_type;
        std::string_view data;
    };

    asio::awaitable<void> serve(asio::ip::tcp::socket socket);
    asio::awaitable<Response> dispatch(Request request);
    asio::awaitable<void> stream(std::shared_ptr<Stream> s) const;

    asio::ip::tcp::acceptor acceptor_;
    ServerOptions options_;
    std::vector<Route> routes_{};
    std::map<std::string, File> files_{};
    std::vector<std::shared_ptr<Stream>> streams_{};
    bool stopping_{false};
};

}  // namespace lisbench::web

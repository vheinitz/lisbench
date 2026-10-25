#include "lisbench/io/relay.hpp"

#include <asio/experimental/awaitable_operators.hpp>

#include <array>

namespace lisbench::io {

namespace {

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

void close(Stream& s) {
    std::error_code ec;
    std::visit([&ec](auto& stream) { stream.close(ec); }, s);
}

// Both streams live in relay()'s frame for as long as the pumps run.
// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
asio::awaitable<void> pump(Side from, Stream& in, Stream& out, const RelayHandlers& handlers,
                           bool forward) {
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    std::array<std::byte, 2048> buffer{};
    while (true) {
        const auto [ec, n] = co_await std::visit(
            [&buffer](auto& s) { return s.async_read_some(asio::buffer(buffer), use_tuple); }, in);
        if (ec || n == 0) {
            if (handlers.on_closed) {
                handlers.on_closed(from, ec ? ec.message() : "closed");
            }
            break;
        }
        const bytes_view data{buffer.data(), n};
        // Report first, so that the log order matches the order on the wire.
        if (handlers.on_data) {
            handlers.on_data(from, std::chrono::system_clock::now(), data);
        }
        if (forward) {
            const auto [wec, written] = co_await std::visit(
                [&data](auto& s) {
                    return asio::async_write(s, asio::buffer(data.data(), data.size()), use_tuple);
                },
                out);
            if (wec) {
                if (handlers.on_closed) {
                    handlers.on_closed(from == Side::a ? Side::b : Side::a, wec.message());
                }
                break;
            }
        }
    }
    close(in);
    close(out);
}

}  // namespace

asio::awaitable<void> drain(Stream s, RelayHandlers handlers) {
    // The pump writes only when forwarding, so the second stream is unused.
    co_await pump(Side::a, s, s, handlers, false);
}

asio::awaitable<void> relay(Stream a, Stream b, RelayHandlers handlers, bool forward) {
    using namespace asio::experimental::awaitable_operators;
    co_await (pump(Side::a, a, b, handlers, forward) && pump(Side::b, b, a, handlers, forward));
}

}  // namespace lisbench::io

// The relay must be byte-transparent in both directions, whatever the bytes.
#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <random>

#include "lisbench/io/relay.hpp"

using namespace lisbench;
using namespace std::chrono_literals;

namespace {

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

bytes random_bytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::uniform_int_distribution<int> any{0, 255};
    bytes out(n);
    for (auto& b : out) {
        b = static_cast<std::byte>(any(rng));
    }
    return out;
}

// Writes `out` in random-sized pieces, reads until `expect` bytes arrived.
asio::awaitable<bytes> exchange(
    asio::ip::tcp::socket& s, bytes out,
    std::size_t expect,  // NOLINT(cppcoreguidelines-avoid-reference-coroutine-parameters)
    std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::uniform_int_distribution<std::size_t> piece{1, 3000};
    bytes in;
    std::array<std::byte, 4096> buffer{};
    std::size_t sent = 0;
    while (sent < out.size() || in.size() < expect) {
        if (sent < out.size()) {
            const auto n = std::min(piece(rng), out.size() - sent);
            co_await asio::async_write(
                s, asio::buffer(out.data() + sent, n),
                use_tuple);  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            sent += n;
        }
        while (in.size() < expect && s.available() > 0) {
            const auto [ec, n] = co_await s.async_read_some(asio::buffer(buffer), use_tuple);
            if (ec) {
                co_return in;
            }
            in.insert(in.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
        }
        if (sent == out.size() && in.size() < expect) {
            const auto [ec, n] = co_await s.async_read_some(asio::buffer(buffer), use_tuple);
            if (ec) {
                co_return in;
            }
            in.insert(in.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n));
        }
    }
    co_return in;
}

}  // namespace

TEST_CASE("relay is transparent for 200 kB of random bytes each way") {
    asio::io_context io;
    const io::Tcp local{.host = "127.0.0.1", .port = 0, .listen = true};
    auto relay_in = io::make_acceptor(io.get_executor(), local);  // side A connects here
    auto far_end = io::make_acceptor(io.get_executor(), local);   // relay connects to B here
    const auto a_data = random_bytes(200'000, 1);
    const auto b_data = random_bytes(200'000, 2);
    std::size_t reported_a = 0;
    std::size_t reported_b = 0;
    bytes got_at_a;
    bytes got_at_b;

    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<
                  void> {  // NOLINT(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            auto a = co_await io::accept(relay_in);
            auto b = co_await io::connect(
                io::Tcp{.host = "127.0.0.1", .port = far_end.local_endpoint().port()});
            co_await io::relay(std::move(a), std::move(b),
                               {.on_data =
                                    [&](io::Side s, auto, bytes_view d) {
                                        (s == io::Side::a ? reported_a : reported_b) += d.size();
                                    },
                                .on_closed = {}});
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<
                  void> {  // NOLINT(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            auto s = co_await far_end.async_accept(asio::use_awaitable);
            got_at_b = co_await exchange(s, b_data, a_data.size(), 3);
        },
        asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<
                  void> {  // NOLINT(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            asio::ip::tcp::socket s{io};
            co_await s.async_connect(
                {asio::ip::make_address("127.0.0.1"), relay_in.local_endpoint().port()},
                asio::use_awaitable);
            got_at_a = co_await exchange(s, a_data, b_data.size(), 4);
            s.close();
        },
        asio::detached);
    io.run_for(20s);

    CHECK(got_at_b == a_data);
    CHECK(got_at_a == b_data);
    CHECK(reported_a == a_data.size());
    CHECK(reported_b == b_data.size());
}

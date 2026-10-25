// The conformance suite against lisbench's own link (which must pass) and
// against a deliberately broken receiver (which must fail where it is broken).

#include <catch2/catch_test_macros.hpp>

#include <asio/experimental/awaitable_operators.hpp>

#include <map>

#include "../devsim/LisDevice/src/LisDevice.h"
#include "lisbench/conform/tester.hpp"
#include "lisbench/sim/runner.hpp"

using namespace lisbench;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;

namespace {

// Short but proportionate timing for both sides, so the suite runs in seconds.
lis01::Timing quick() {
    lis01::Timing t;
    t.reply_timeout = 400ms;
    t.receive_timeout = 800ms;
    t.busy_retry = 300ms;
    t.contention_instrument = 150ms;
    t.contention_computer = 600ms;
    return t;
}

conform::Options options(conform::Iut iut) {
    return {.iut = iut,
            .timing = quick(),
            .only = {},
            .wait_for_iut = 3s,
            .honours_receiver_interrupt = std::nullopt};
}

std::map<std::string, conform::Result> by_id(const std::vector<conform::Result>& results) {
    std::map<std::string, conform::Result> out;
    for (const auto& r : results) {
        out[r.id] = r;
    }
    return out;
}

// Accepts the tester's connection and hands the stream to `iut`; the tester
// connects and runs the suite.
template <typename Iut>
std::vector<conform::Result> run_against(conform::Iut role, Iut iut,
                                         std::vector<std::string> only = {}) {
    asio::io_context io;
    auto acceptor = io::make_acceptor(io.get_executor(),
                                      io::Tcp{.host = "127.0.0.1", .port = 0, .listen = true});
    const auto port = acceptor.local_endpoint().port();
    std::vector<conform::Result> results;
    asio::co_spawn(io, iut(acceptor), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto stream = co_await io::connect(io::Tcp{.host = "127.0.0.1", .port = port});
            auto o = options(role);
            o.only = only;
            results = co_await conform::run(std::move(stream), o, {});
            io.stop();
        },
        [](const std::exception_ptr& e) { REQUIRE_FALSE(e); });
    io.run_for(120s);
    return results;
}

}  // namespace

TEST_CASE("conform: lisbench's own LIS passes") {
    const auto results = run_against(conform::Iut::computer, [](asio::ip::tcp::acceptor& a) {
        sim::Scenario lis{.name = "lis",
                          .role = lis01::Role::computer,
                          .endpoint = io::Tcp{.host = "127.0.0.1", .port = 0, .listen = true},
                          .timing = quick()};
        lis.answer_queries = {"H|\\^&|||lis", "P|1", "O|1|CONF-1||^^^TEST", "L|1|F"};
        return [](sim::Scenario s, asio::ip::tcp::acceptor& acc) -> asio::awaitable<void> {
            (void)co_await sim::run(std::move(s), {}, &acc);
        }(std::move(lis), a);
    });
    REQUIRE(by_id(results).size() == conform::suite.size());
    for (const auto& r : results) {
        INFO(r.id << ": " << r.detail);
        CHECK(r.verdict == conform::Verdict::pass);
        CHECK_FALSE(r.detail.contains("note"));  // also every "should" kept
    }
}

TEST_CASE("conform: lisbench's own instrument passes") {
    const auto results = run_against(conform::Iut::instrument, [](asio::ip::tcp::acceptor& a) {
        return [](asio::ip::tcp::acceptor& acc) -> asio::awaitable<void> {
            auto stream = co_await io::accept(acc);
            io::Session session{std::move(stream),
                                {.role = lis01::Role::instrument, .timing = quick(), .faults = {}},
                                {}};
            lis01::Message m;
            for (const auto r :
                 {"H|\\^&|||instrument", "P|1", "O|1|S1||^^^GLU", "R|1|^^^GLU|5", "L|1|N"}) {
                m.records.push_back(to_bytes(r));
            }
            const auto produce = [&]() -> asio::awaitable<void> {
                while (true) {
                    (void)co_await session.send(m);
                    asio::steady_timer pause{co_await asio::this_coro::executor, 100ms};
                    co_await pause.async_wait(asio::use_awaitable);
                }
            };
            co_await (session.run() || produce());
        }(a);
    });
    REQUIRE(by_id(results).size() == conform::suite.size());
    for (const auto& r : results) {
        INFO(r.id << ": " << r.detail);
        CHECK(r.verdict == conform::Verdict::pass);
        CHECK_FALSE(r.detail.contains("note"));  // also every "should" kept
    }
}

TEST_CASE("conform: the microcontroller instrument (devsim/LisDevice) passes") {
    const auto results = run_against(conform::Iut::instrument, [](asio::ip::tcp::acceptor& a) {
        return [](asio::ip::tcp::acceptor& acc) -> asio::awaitable<void> {
            auto stream = co_await io::accept(acc);
            auto& socket = std::get<asio::ip::tcp::socket>(stream);
            struct Ctx {
                asio::ip::tcp::socket* socket;
                std::vector<std::string> records;
                bool finished{true};
                std::chrono::steady_clock::time_point last_end{};
            } ctx{&socket, {"H|\\^&|||uno", "P|1", "O|1|S1||^^^GLU", "R|1|^^^GLU|5", "L|1|N"}};
            const lisdev::Callbacks cb{
                &ctx,
                [](void* c, const std::uint8_t* d, std::size_t n) {
                    std::error_code ec;
                    asio::write(*static_cast<Ctx*>(c)->socket, asio::buffer(d, n), ec);
                },
                [](void* c) { return static_cast<Ctx*>(c)->records.size(); },
                [](void* c, std::size_t i, char* buf, std::size_t cap) {
                    const auto& r = static_cast<Ctx*>(c)->records.at(i);
                    r.copy(buf, std::min(r.size(), cap));
                    return r.size();
                },
                nullptr,
                [](void* c, lisdev::Outcome) {
                    static_cast<Ctx*>(c)->last_end = std::chrono::steady_clock::now();
                }};
            lisdev::Timing t;
            const auto q = quick();
            t.reply_timeout_ms = static_cast<std::uint32_t>(q.reply_timeout.count());
            t.receive_timeout_ms = static_cast<std::uint32_t>(q.receive_timeout.count());
            t.busy_retry_ms = static_cast<std::uint32_t>(q.busy_retry.count());
            t.contention_ms = static_cast<std::uint32_t>(q.contention_instrument.count());
            lisdev::Device device{cb, t};
            const auto start = std::chrono::steady_clock::now();
            const auto millis = [&] {
                return static_cast<std::uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count());
            };
            std::array<std::uint8_t, 256> buffer{};
            while (true) {
                // The sketch's loop(): bytes in, poll, start a message 100 ms after the last.
                asio::steady_timer tick{co_await asio::this_coro::executor, 5ms};
                const auto r =
                    co_await (socket.async_read_some(asio::buffer(buffer),
                                                     asio::as_tuple(asio::use_awaitable)) ||
                              tick.async_wait(asio::as_tuple(asio::use_awaitable)));
                if (r.index() == 0) {
                    const auto [ec, n] = std::get<0>(r);
                    if (ec) {
                        co_return;
                    }
                    for (std::size_t i = 0; i < n; ++i) {
                        device.on_byte(buffer.at(i), millis());
                    }
                }
                device.poll(millis());
                if (device.state() == lisdev::idle &&
                    std::chrono::steady_clock::now() - ctx.last_end > 100ms) {
                    (void)device.start(millis());
                }
            }
        }(a);
    });
    REQUIRE(by_id(results).size() == conform::suite.size());
    for (const auto& r : results) {
        INFO(r.id << ": " << r.detail);
        CHECK(r.verdict == conform::Verdict::pass);
        CHECK_FALSE(r.detail.contains("note"));  // also every "should" kept
    }
}

TEST_CASE("conform: an IUT that is still starting up is waited for, not judged") {
    // Like an Arduino that resets when the port is opened: nothing for a while.
    const auto results =
        run_against(conform::Iut::computer,
                    [](asio::ip::tcp::acceptor& a) {
                        return [](asio::ip::tcp::acceptor& acc) -> asio::awaitable<void> {
                            auto stream = co_await io::accept(acc);
                            asio::steady_timer boot{co_await asio::this_coro::executor, 1500ms};
                            co_await boot.async_wait(asio::use_awaitable);
                            // Bytes sent during the boot are lost, as with a real reset.
                            std::get<asio::ip::tcp::socket>(stream).non_blocking(true);
                            std::array<std::byte, 256> junk{};
                            std::error_code ec;
                            while (std::get<asio::ip::tcp::socket>(stream).read_some(
                                       asio::buffer(junk), ec) > 0) {
                            }
                            std::get<asio::ip::tcp::socket>(stream).non_blocking(false);
                            io::Session session{
                                std::move(stream),
                                {.role = lis01::Role::computer, .timing = quick(), .faults = {}},
                                {}};
                            co_await session.run();
                        }(a);
                    },
                    {"TR01", "TR06"});
    const auto r = by_id(results);
    REQUIRE(r.size() == 2);
    INFO(r.at("TR01").detail << " / " << r.at("TR06").detail);
    CHECK(r.at("TR01").verdict == conform::Verdict::pass);
    CHECK(r.at("TR06").verdict == conform::Verdict::pass);
}

TEST_CASE("conform: a receiver that acknowledges everything fails TR02 and TR03") {
    const auto results = run_against(conform::Iut::computer, [](asio::ip::tcp::acceptor& a) {
        return [](asio::ip::tcp::acceptor& acc) -> asio::awaitable<void> {
            auto stream = co_await io::accept(acc);
            auto& socket = std::get<asio::ip::tcp::socket>(stream);
            std::array<std::byte, 512> buffer{};
            while (true) {
                const auto [ec, n] = co_await socket.async_read_some(
                    asio::buffer(buffer), asio::as_tuple(asio::use_awaitable));
                if (ec) {
                    co_return;
                }
                for (std::size_t i = 0; i < n; ++i) {
                    // ACK for ENQ and for the LF that ends every frame; never checks anything.
                    if (buffer.at(i) == lis01::ctl::enq || buffer.at(i) == lis01::ctl::lf) {
                        const std::array ack{lis01::ctl::ack};
                        co_await asio::async_write(socket, asio::buffer(ack),
                                                   asio::as_tuple(asio::use_awaitable));
                    }
                }
            }
        }(a);
    });
    const auto r = by_id(results);
    REQUIRE(r.size() == conform::suite.size());
    CHECK(r.at("TR01").verdict == conform::Verdict::pass);
    CHECK(r.at("TR02").verdict == conform::Verdict::fail);
    CHECK(r.at("TR03").verdict == conform::Verdict::fail);
    CHECK(r.at("TR07").verdict == conform::Verdict::pass);  // a "should": only a note
    CHECK(r.at("TR07").detail.contains("note"));
    CHECK(r.at("TS02").verdict == conform::Verdict::inconclusive);  // it never sends
    CHECK(r.at("TS01").verdict == conform::Verdict::inconclusive);
}

TEST_CASE("conform: verdict helpers") {
    CHECK(conform::within(950ms, 1000ms));
    CHECK_FALSE(conform::within(940ms, 1000ms));
    CHECK(conform::within(1900ms, 1000ms, 2.0));
    CHECK_FALSE(conform::within(2100ms, 1000ms, 2.0));
    CHECK(conform::find("TS03") != nullptr);
    CHECK(conform::find("TX99") == nullptr);
    const auto lines = conform::report({{.id = "TR02",
                                         .verdict = conform::Verdict::fail,
                                         .detail = "answered with ACK",
                                         .measured = {}}});
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with("TR02  FAIL"));
    CHECK(lines[2] == "0 pass, 1 fail, 0 inconclusive");
}

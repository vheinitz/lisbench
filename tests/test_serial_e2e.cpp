// Instrument and LIS simulators on two serial ports, connected by a null
// modem made of two pseudo terminals (what socat would do). Linux only.
#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include <pty.h>
#include <unistd.h>
#include <asio.hpp>

#include <array>

#include "lisbench/sim/runner.hpp"

using namespace lisbench;
using namespace std::chrono_literals;

namespace {

struct Pty {
    int master{-1};
    int slave_fd{-1};  // kept open: with no slave open, reads on the master fail (EIO)
    std::string slave;
};

Pty open_pty() {
    int master = -1;
    int slave = -1;
    std::array<char, 128> name{};
    REQUIRE(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0);
    return {.master = master, .slave_fd = slave, .slave = name.data()};
}

asio::awaitable<void> pump(asio::posix::stream_descriptor& from,
                           asio::posix::stream_descriptor& to) {
    std::array<char, 512> buffer{};
    while (true) {
        const auto [ec, n] = co_await from.async_read_some(asio::buffer(buffer),
                                                           asio::as_tuple(asio::use_awaitable));
        if (ec) {
            co_return;
        }
        co_await asio::async_write(to, asio::buffer(buffer.data(), n),
                                   asio::as_tuple(asio::use_awaitable));
    }
}

}  // namespace

TEST_CASE("e2e serial: results and query over a null modem") {
    asio::io_context io;
    auto a = open_pty();
    auto b = open_pty();
    asio::posix::stream_descriptor ma{io, a.master};
    asio::posix::stream_descriptor mb{io, b.master};
    asio::co_spawn(io, pump(ma, mb), asio::detached);
    asio::co_spawn(io, pump(mb, ma), asio::detached);

    lis01::Timing t;
    t.reply_timeout = 1s;
    t.receive_timeout = 2s;
    const auto records = [](const char* name) {
        return sim::read_records(std::filesystem::path{LISBENCH_CORPUS_DIR} / name).value();
    };
    sim::Scenario instrument{.name = "instrument",
                             .role = lis01::Role::instrument,
                             .endpoint = io::Serial{.device = a.slave, .baud = 115200},
                             .timing = t};
    instrument.steps = {
        sim::SendStep{.records = records("valid_host_query.astm"), .source = "query"},
        sim::ExpectStep{.within = 3s, .records = 7, .valid = true},
    };
    sim::Scenario lis{.name = "lis",
                      .role = lis01::Role::computer,
                      .endpoint = io::Serial{.device = b.slave, .baud = 115200},
                      .timing = t,
                      .answer_queries = records("valid_order_download.astm")};
    lis.steps = {sim::ExpectStep{.within = 3s, .records = 3, .contains = {"Q|1|"}},
                 sim::WaitStep{.duration = 1500ms}};

    sim::Report ri;
    sim::Report rl;
    asio::co_spawn(io, sim::run(lis, {}), [&](const std::exception_ptr& e, sim::Report r) {
        REQUIRE_FALSE(e);
        rl = std::move(r);
    });
    asio::co_spawn(io, sim::run(instrument, {}), [&](const std::exception_ptr& e, sim::Report r) {
        REQUIRE_FALSE(e);
        ri = std::move(r);
        ma.cancel();
        mb.cancel();
    });
    io.run_for(15s);
    ::close(a.slave_fd);
    ::close(b.slave_fd);
    for (const auto& s : ri.steps) {
        INFO("instrument: " << s.text);
        CHECK(s.ok);
    }
    for (const auto& s : rl.steps) {
        INFO("lis: " << s.text);
        CHECK(s.ok);
    }
    CHECK(ri.steps.size() == 2);
    CHECK(rl.steps.size() == 2);
}

#endif

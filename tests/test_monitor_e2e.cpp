// instrument simulator -> monitor relay -> LIS simulator, all over TCP.
#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>

#include "lisbench/io/relay.hpp"
#include "lisbench/monitor/monitor.hpp"
#include "lisbench/sim/runner.hpp"

using namespace lisbench;
using namespace std::chrono_literals;

TEST_CASE("e2e monitor: everything passes through and is decoded on the way") {
    asio::io_context io;
    const auto local = [](std::uint16_t port, bool listen) {
        return io::Tcp{.host = "127.0.0.1", .port = port, .listen = listen};
    };
    auto lis_acceptor = io::make_acceptor(io.get_executor(), local(0, true));
    auto mon_acceptor = io::make_acceptor(io.get_executor(), local(0, true));
    const auto records = [](const char* name) {
        return sim::read_records(std::filesystem::path{LISBENCH_CORPUS_DIR} / name).value();
    };

    lis01::Timing t;
    t.reply_timeout = 1s;
    t.receive_timeout = 1s;
    sim::Scenario lis{.name = "lis",
                      .role = lis01::Role::computer,
                      .endpoint = local(0, true),
                      .timing = t,
                      .answer_queries = records("valid_order_download.astm")};
    sim::Scenario instrument{.name = "instrument",
                             .role = lis01::Role::instrument,
                             .endpoint = local(mon_acceptor.local_endpoint().port(), false),
                             .timing = t};
    instrument.faults = {
        io::Fault{.target = io::FaultTarget::frame, .action = io::FaultAction::corrupt, .nth = 2}};
    instrument.steps = {
        sim::SendStep{.records = records("valid_result_upload.astm"), .source = "results"},
        sim::SendStep{.records = records("valid_host_query.astm"), .source = "query"},
        sim::ExpectStep{.within = 2s, .records = 7},
    };

    monitor::Options options;
    options.watches.push_back(watch::parse("O.priority == S").value());
    monitor::Monitor mon{options};
    std::vector<monitor::Event> events;
    sim::Report instrument_report;

    asio::co_spawn(io, sim::run(lis, {}, &lis_acceptor), asio::detached);
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            auto from_instrument = co_await io::accept(mon_acceptor);
            auto to_lis = co_await io::connect(local(lis_acceptor.local_endpoint().port(), false));
            co_await io::relay(std::move(from_instrument), std::move(to_lis),
                               {.on_data =
                                    [&](io::Side s, monitor::Clock::time_point at, bytes_view d) {
                                        auto more = mon.feed(s, at, d);
                                        events.insert(events.end(), more.begin(), more.end());
                                    },
                                .on_closed = {}});
            io.stop();
        },
        asio::detached);
    asio::co_spawn(io, sim::run(instrument, {}), [&](const std::exception_ptr& e, sim::Report r) {
        REQUIRE_FALSE(e);
        instrument_report = std::move(r);
    });
    io.run_for(15s);

    for (const auto& s : instrument_report.steps) {
        INFO(s.text);
        CHECK(s.ok);
    }
    CHECK(instrument_report.steps.size() == 3);
    CHECK(mon.stats(io::Side::a).records == 10);  // results + query
    CHECK(mon.stats(io::Side::a).bad_frames == 1);
    CHECK(mon.stats(io::Side::b).naks_sent == 1);
    CHECK(mon.stats(io::Side::b).records == 7);  // orders
    CHECK(mon.stats(io::Side::b).watch_hits == 2);
    CHECK(mon.stats(io::Side::a).transfers == 2);
    CHECK(mon.stats(io::Side::b).transfers == 1);
}

// The simulator against itself over real sockets (and serial ports on Linux).
#include <catch2/catch_test_macros.hpp>

#include <asio.hpp>

#include "lisbench/sim/runner.hpp"

using namespace lisbench;
using namespace std::chrono_literals;

namespace {

lis01::Timing fast_timing() {
    lis01::Timing t;
    t.reply_timeout = 300ms;
    t.receive_timeout = 500ms;
    t.busy_retry = 100ms;
    t.contention_instrument = 50ms;
    t.contention_computer = 200ms;
    return t;
}

std::vector<std::string> corpus(const char* name) {
    return sim::read_records(std::filesystem::path{LISBENCH_CORPUS_DIR} / name).value();
}

struct Pair {
    sim::Report instrument;
    sim::Report lis;
    std::vector<io::Transfer> lis_received;
};

// LIS listens on an ephemeral port, the instrument connects to it.
Pair run_pair(sim::Scenario instrument, sim::Scenario lis) {
    asio::io_context io;
    auto acceptor = io::make_acceptor(io.get_executor(),
                                      io::Tcp{.host = "127.0.0.1", .port = 0, .listen = true});
    instrument.endpoint =
        io::Tcp{.host = "127.0.0.1", .port = acceptor.local_endpoint().port(), .listen = false};
    lis.endpoint = io::Tcp{.host = "127.0.0.1", .port = 0, .listen = true};
    Pair result;
    sim::Observers lis_obs;
    lis_obs.session.on_transfer = [&result](const io::Transfer& t) {
        result.lis_received.push_back(t);
    };
    asio::co_spawn(io, sim::run(lis, lis_obs, &acceptor),
                   [&](const std::exception_ptr& e, sim::Report r) {
                       REQUIRE_FALSE(e);
                       result.lis = std::move(r);
                   });
    asio::co_spawn(io, sim::run(instrument, {}), [&](const std::exception_ptr& e, sim::Report r) {
        REQUIRE_FALSE(e);
        result.instrument = std::move(r);
    });
    io.run_for(20s);
    return result;
}

}  // namespace

TEST_CASE("e2e: results up, query answered with orders") {
    sim::Scenario instrument{
        .name = "instrument", .role = lis01::Role::instrument, .timing = fast_timing()};
    instrument.steps = {
        sim::SendStep{.records = corpus("valid_result_upload.astm"), .source = "results"},
        sim::SendStep{.records = corpus("valid_host_query.astm"), .source = "query"},
        sim::ExpectStep{.within = 2s, .records = 7, .contains = {"O|1|S-1003"}, .valid = true},
    };
    sim::Scenario lis{.name = "lis",
                      .role = lis01::Role::computer,
                      .timing = fast_timing(),
                      .answer_queries = corpus("valid_order_download.astm")};

    const auto r = run_pair(instrument, lis);
    INFO((r.instrument.steps.empty() ? std::string{} : r.instrument.steps.back().text));
    CHECK(r.instrument.ok);
    CHECK(r.instrument.steps.size() == 3);
    CHECK(r.lis.connections == 1);
    REQUIRE(r.lis_received.size() == 2);
    CHECK(r.lis_received[0].records.size() == 7);
    CHECK(r.lis_received[0].complete);
}

TEST_CASE("e2e: refused and corrupted frames are repeated, the message still arrives") {
    sim::Scenario instrument{
        .name = "instrument", .role = lis01::Role::instrument, .timing = fast_timing()};
    instrument.faults = {io::Fault{
        .target = io::FaultTarget::frame, .action = io::FaultAction::corrupt, .every = 2}};
    instrument.steps = {
        sim::SendStep{.records = corpus("valid_result_upload.astm"), .source = "results"}};
    sim::Scenario lis{.name = "lis", .role = lis01::Role::computer, .timing = fast_timing()};
    lis.faults = {
        io::Fault{.target = io::FaultTarget::ack, .action = io::FaultAction::nak, .nth = 3}};

    const auto r = run_pair(instrument, lis);
    CHECK(r.instrument.ok);
    REQUIRE(r.lis_received.size() == 1);
    CHECK(r.lis_received[0].records.size() == 7);
    std::vector<std::string> got;
    for (const auto& rec : r.lis_received[0].records) {
        got.push_back(to_string(rec));
    }
    CHECK(got == corpus("valid_result_upload.astm"));
}

TEST_CASE("e2e: a receiver that never answers frames makes the sender time out") {
    sim::Scenario instrument{
        .name = "instrument", .role = lis01::Role::instrument, .timing = fast_timing()};
    instrument.steps = {sim::SendStep{.records = corpus("valid_host_query.astm"),
                                      .source = "query",
                                      .expect = lis01::Outcome::timeout}};
    sim::Scenario lis{.name = "lis", .role = lis01::Role::computer, .timing = fast_timing()};
    lis.faults = {
        io::Fault{.target = io::FaultTarget::ack, .action = io::FaultAction::drop, .nth = 2}};

    const auto r = run_pair(instrument, lis);
    INFO((r.instrument.steps.empty() ? std::string{} : r.instrument.steps.back().text));
    CHECK(r.instrument.ok);  // the timeout was expected
    // The sender ends the transfer with EOT after its timeout. On LIS01-A2
    // level that looks like a normal end; only the missing L record shows
    // that the message is incomplete.
    REQUIRE(r.lis_received.size() == 1);
    CHECK(r.lis_received[0].records.size() == 1);
    CHECK(to_string(r.lis_received[0].records.back()).front() != 'L');
}

TEST_CASE("e2e: frames refused six times end with rejected") {
    sim::Scenario instrument{
        .name = "instrument", .role = lis01::Role::instrument, .timing = fast_timing()};
    instrument.steps = {sim::SendStep{.records = corpus("valid_host_query.astm"),
                                      .source = "query",
                                      .expect = lis01::Outcome::rejected}};
    sim::Scenario lis{.name = "lis", .role = lis01::Role::computer, .timing = fast_timing()};
    // The first ACK answers ENQ; from then on every frame is refused.
    lis.faults = {
        io::Fault{.target = io::FaultTarget::ack, .action = io::FaultAction::nak, .every = 1}};
    lis.faults.insert(
        lis.faults.begin(),
        io::Fault{.target = io::FaultTarget::ack, .action = io::FaultAction::delay, .nth = 1});

    const auto r = run_pair(instrument, lis);
    INFO((r.instrument.steps.empty() ? std::string{} : r.instrument.steps.back().text));
    CHECK(r.instrument.ok);
}

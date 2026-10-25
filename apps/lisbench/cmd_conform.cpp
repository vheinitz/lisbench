// lisbench conform: run the LIS01-A2 conformance test suite against an IUT.

#include "cmd_conform.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <ranges>

#include "cmd_monitor.hpp"
#include "common.hpp"
#include "lisbench/conform/tester.hpp"

namespace lisbench::cli {

namespace {

asio::awaitable<std::vector<conform::Result>> run_suite(io::Endpoint endpoint,
                                                        conform::Options options) {
    auto stream = co_await open_endpoint(std::move(endpoint), "IUT");
    co_return co_await conform::run(
        std::move(stream), std::move(options),
        [](std::string_view line) { println("{} [conform] {}", clock_now(), line); });
}

}  // namespace

int cmd_conform(const ConformFlags& flags) {
    conform::Options options{
        .iut = flags.iut == "instrument" ? conform::Iut::instrument : conform::Iut::computer,
        .timing = flags.fast_timing ? lis01::fast_test_timing() : lis01::Timing{},
        .only = {},
        .wait_for_iut = std::chrono::seconds{flags.wait_s},
        .honours_receiver_interrupt = std::nullopt};
    for (const auto& item : flags.only) {
        for (auto part : item | std::views::split(',')) {
            std::string id{part.begin(), part.end()};
            if (conform::find(id) == nullptr) {
                println(stderr, "conform: unknown test case '{}'", id);
                return 2;
            }
            options.only.push_back(std::move(id));
        }
    }
    std::string pics = "none";
    if (!flags.profile.empty()) {
        const auto p = sim::load_profile(flags.profile, profile_search_path());
        if (!p) {
            println(stderr, "conform: --profile: {}", p.error());
            return 2;
        }
        sim::apply(*p, options.timing);
        options.honours_receiver_interrupt = p->honours_receiver_interrupt;
        pics = p->id;
    }
    auto endpoint = io::parse_endpoint(flags.endpoint);
    if (!endpoint) {
        println(stderr, "conform: {}", endpoint.error());
        return 2;
    }

    println("lisbench {} conformance test, LIS01-A2", LISBENCH_VERSION);
    println("IUT: {} at {}; profile (PICS): {}; timing: {}", flags.iut, io::to_string(*endpoint),
            pics, flags.fast_timing ? "fast test preset (not LIS01-A2)" : "LIS01-A2");

    asio::io_context io;
    std::vector<conform::Result> results;
    int rc = 0;
    asio::co_spawn(io, run_suite(*endpoint, options),
                   [&](const std::exception_ptr& e, std::vector<conform::Result> r) {
                       if (e) {
                           try {
                               std::rethrow_exception(e);
                           } catch (const std::exception& ex) {
                               println(stderr, "conform: {}", ex.what());
                               rc = 2;
                           }
                       }
                       results = std::move(r);
                   });
    io.run();
    if (rc != 0) {
        return rc;
    }
    println("");
    for (const auto& line : conform::report(results)) {
        println("{}", line);
    }
    if (!flags.json.empty()) {
        nlohmann::json j = nlohmann::json::array();
        for (const auto& r : results) {
            const auto* tc = conform::find(r.id);
            j.push_back({{"id", r.id},
                         {"purpose", tc != nullptr ? tc->purpose : ""},
                         {"level", tc != nullptr ? conform::to_string(tc->level) : ""},
                         {"verdict", conform::to_string(r.verdict)},
                         {"detail", r.detail},
                         {"measured_ms", r.measured ? nlohmann::json(r.measured->count())
                                                    : nlohmann::json(nullptr)}});
        }
        std::ofstream out(flags.json);
        out << j.dump(2) << '\n';
        if (!out) {
            println(stderr, "conform: cannot write {}", flags.json);
            return 2;
        }
    }
    const bool failed = std::ranges::any_of(
        results, [](const auto& r) { return r.verdict == conform::Verdict::fail; });
    return failed ? 1 : 0;
}

}  // namespace lisbench::cli

// The web API end to end, as the browser uses it: requests over HTTP and
// live data from /api/events, against simulators on the same event loop.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#include "lisbench/sim/runner.hpp"
#include "lisbench/web/app.hpp"
#include "lisbench/web/server.hpp"

using namespace lisbench;
using namespace std::chrono_literals;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::uint16_t free_port(asio::io_context& io) {
    asio::ip::tcp::acceptor a{io, {asio::ip::make_address("127.0.0.1"), 0}};
    return a.local_endpoint().port();
}

struct Reply {
    int status{0};
    json body;
};

asio::awaitable<Reply> http(std::uint16_t port, std::string method, std::string target,
                            json body = nullptr) {
    asio::ip::tcp::socket s{co_await asio::this_coro::executor};
    co_await s.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
    const auto text = body.is_null() ? std::string{} : body.dump();
    const auto request = std::format("{} {} HTTP/1.1\r\nContent-Length: {}\r\n\r\n{}", method,
                                     target, text.size(), text);
    co_await asio::async_write(s, asio::buffer(request), asio::use_awaitable);
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
        const auto [ec, n] =
            co_await s.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
        if (ec) {
            break;
        }
        out.append(buf.data(), n);
    }
    Reply r;
    r.status = std::stoi(out.substr(9, 3));
    const auto body_at = out.find("\r\n\r\n");
    r.body = json::parse(out.substr(body_at + 4), nullptr, false);
    co_return r;
}

// Collects the events of one /api/events stream.
struct Events {
    std::vector<std::pair<std::string, json>> got;

    asio::awaitable<void> listen(std::uint16_t port) {
        asio::ip::tcp::socket s{co_await asio::this_coro::executor};
        co_await s.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
        co_await asio::async_write(
            s, asio::buffer(std::string_view{"GET /api/events HTTP/1.1\r\n\r\n"}),
            asio::use_awaitable);
        std::string pending;
        std::array<char, 4096> buf{};
        while (true) {
            const auto [ec, n] =
                co_await s.async_read_some(asio::buffer(buf), asio::as_tuple(asio::use_awaitable));
            if (ec) {
                co_return;
            }
            pending.append(buf.data(), n);
            for (auto end = pending.find("\n\n"); end != std::string::npos;
                 end = pending.find("\n\n")) {
                const auto block = pending.substr(0, end);
                pending.erase(0, end + 2);
                if (!block.starts_with("event: ")) {
                    continue;
                }
                const auto nl = block.find('\n');
                std::string data;
                for (std::size_t at = nl + 1; at < block.size();) {
                    const auto next = block.find('\n', at);
                    data += block.substr(
                        at + 6, next == std::string::npos ? std::string::npos : next - at - 6);
                    at = next == std::string::npos ? block.size() : next + 1;
                }
                got.emplace_back(block.substr(7, nl - 7), json::parse(data, nullptr, false));
            }
        }
    }

    [[nodiscard]] std::size_t count(std::string_view event,
                                    const std::function<bool(const json&)>& pred = {}) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            got, [&](const auto& e) { return e.first == event && (!pred || pred(e.second)); }));
    }

    asio::awaitable<bool> wait(std::string_view event, const std::function<bool(const json&)>& pred,
                               std::chrono::milliseconds limit = 10s) {
        const auto until = std::chrono::steady_clock::now() + limit;
        while (count(event, pred) == 0) {
            if (std::chrono::steady_clock::now() > until) {
                co_return false;
            }
            asio::steady_timer t{co_await asio::this_coro::executor, 20ms};
            co_await t.async_wait(asio::use_awaitable);
        }
        co_return true;
    }
};

bool has_line(const json& event, std::string_view text) {
    return std::ranges::any_of(event["lines"],
                               [&](const json& l) { return l.get<std::string>().contains(text); });
}

}  // namespace

TEST_CASE("web API: monitor, captures, conformance test and simulator from the browser's side") {
    const auto dir = fs::temp_directory_path() / "lisbench_webapp_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream(dir / "results.astm", std::ios::binary)
            << "H|\\^&|||web\rP|1\rO|1|S1||^^^GLU\rR|1|^^^GLU|150|mg/dL\rL|1|N\r";
        std::ofstream(dir / "orders.astm", std::ios::binary)
            << "H|\\^&|||lis\rP|1\rO|1|S1||^^^GLU\rL|1|F\r";
    }

    asio::io_context io;
    web::Server server{io.get_executor(), {asio::ip::make_address("127.0.0.1"), 0}};
    web::App app{server, {.version = "test", .data_dir = dir, .profile_search = {}}};
    const auto port = server.port();
    asio::co_spawn(io, server.run(), asio::detached);

    // A LIS for the monitor to pass traffic to and for the conformance test.
    lis01::Timing quick;
    quick.reply_timeout = 400ms;
    quick.receive_timeout = 800ms;
    quick.busy_retry = 300ms;
    auto lis_acceptor = io::make_acceptor(io.get_executor(),
                                          io::Tcp{.host = "127.0.0.1", .port = 0, .listen = true});
    const auto lis_port = lis_acceptor.local_endpoint().port();
    sim::Scenario lis{.name = "lis",
                      .role = lis01::Role::computer,
                      .endpoint = io::Tcp{.host = "127.0.0.1", .port = 0, .listen = true},
                      .timing = quick,
                      .serve = true};
    lis.answer_queries = std::vector<std::string>{"H|\\^&|||lis", "P|1", "O|1|S1||^^^GLU", "L|1|F"};
    asio::co_spawn(io, sim::run(lis, {}, &lis_acceptor), asio::detached);
    const auto monitor_port = free_port(io);

    Events events;
    asio::co_spawn(io, events.listen(port), asio::detached);
    // Request bodies are built here: GCC 13 fails (ICE) on braced initializers
    // inside a co_await expression.
    const json bad_name = {{"role", "instrument"},
                           {"endpoint", "tcp://127.0.0.1:1"},
                           {"send", {"../etc/passwd.astm"}}};
    const json bad_json = "not an object";
    const json bad_watch = {
        {"a", "tcp://127.0.0.1:1"}, {"b", "tcp://127.0.0.1:1"}, {"watches", {"R.value >> 1"}}};
    const json start_monitor = {{"a", std::format("tcp-listen://127.0.0.1:{}", monitor_port)},
                                {"b", std::format("tcp://127.0.0.1:{}", lis_port)},
                                {"labels", {"instrument", "lis"}},
                                {"watches", {"R.value > 100"}},
                                {"save", "run1"}};
    const json monitor_again = {{"a", "tcp://127.0.0.1:1"}, {"b", "tcp://127.0.0.1:1"}};
    const json start_sim = {{"role", "instrument"},
                            {"endpoint", std::format("tcp://127.0.0.1:{}", monitor_port)},
                            {"send", {"results.astm"}},
                            {"fast", true}};
    const json start_conform = {{"iut", "computer"},
                                {"endpoint", std::format("tcp://127.0.0.1:{}", lis_port)},
                                {"only", {"TR01", "TR02"}},
                                {"fast", true}};
    const auto watch_hit = [](const json& e) {
        return e["kind"] == "watch" && has_line(e, "R.value > 100");
    };
    const auto step_ok = [](const json& e) {
        return e["text"].get<std::string>().contains("step 1 ok");
    };
    const auto monitor_summary = [](const json& e) { return e["source"] == "monitor"; };
    const auto stopped = [](const json& e) {
        return e["monitor"]["running"] == false && e["monitor"]["save"] == "run1.jsonl";
    };
    const auto any = [](const json&) { return true; };

    std::map<std::string, Reply> r;
    std::map<std::string, bool> seen;
    asio::co_spawn(
        io,
        [&]() -> asio::awaitable<void> {
            r["status"] = co_await http(port, "GET", "/api/status");
            r["bad name"] = co_await http(port, "POST", "/api/sim/start", bad_name);
            r["bad json"] = co_await http(port, "POST", "/api/monitor/start", bad_json);
            r["bad watch"] = co_await http(port, "POST", "/api/monitor/start", bad_watch);

            // Monitor: the instrument connects to A, the monitor connects B to the LIS.
            r["monitor"] = co_await http(port, "POST", "/api/monitor/start", start_monitor);
            r["monitor again"] = co_await http(port, "POST", "/api/monitor/start", monitor_again);
            asio::steady_timer t{co_await asio::this_coro::executor, 200ms};
            co_await t.async_wait(asio::use_awaitable);
            r["sim"] = co_await http(port, "POST", "/api/sim/start", start_sim);
            seen["watch"] = co_await events.wait("monitor", watch_hit);
            seen["sim step"] = co_await events.wait("sim", step_ok);
            r["stop"] = co_await http(port, "POST", "/api/monitor/stop");
            seen["summary"] = co_await events.wait("summary", monitor_summary);
            seen["stopped"] = co_await events.wait("status", stopped);

            // The capture of that run.
            r["files"] = co_await http(port, "GET", "/api/files");
            r["capture"] =
                co_await http(port, "GET", "/api/capture?name=run1.jsonl&watch=R.value+%3E+100");
            r["capture bad"] = co_await http(port, "GET", "/api/capture?name=..%2Frun1.jsonl");

            // Conformance test against the LIS, two cases.
            r["conform"] = co_await http(port, "POST", "/api/conform/start", start_conform);
            seen["result"] = co_await events.wait("result", any, 30s);
            r["result"] = co_await http(port, "GET", "/api/conform/result");
            r["suite"] = co_await http(port, "GET", "/api/suite");
            app.stop_all();
            server.stop();
            io.stop();
        },
        [](const std::exception_ptr& e) { REQUIRE_FALSE(e); });
    io.run_for(60s);

    CHECK(r["status"].status == 200);
    CHECK(r["status"].body["monitor"]["running"] == false);
    CHECK(r["bad name"].status == 400);
    CHECK(r["bad name"].body["error"].get<std::string>().contains("not a valid file name"));
    CHECK(r["bad json"].status == 400);
    CHECK(r["bad watch"].status == 400);
    CHECK(r["monitor"].status == 200);
    CHECK(r["monitor again"].status == 409);
    CHECK(r["sim"].status == 200);
    CHECK(seen["watch"]);
    CHECK(seen["sim step"]);
    CHECK(r["stop"].status == 200);
    CHECK(seen["summary"]);
    CHECK(seen["stopped"]);
    CHECK(events.count("monitor", [](const json& e) {
        return e["kind"] == "record" && e["from"] == "instrument" && has_line(e, "R|1|^^^GLU|150");
    }) == 1);

    REQUIRE(r["files"].status == 200);
    std::vector<std::string> names;
    for (const auto& f : r["files"].body) {
        names.push_back(f["name"]);
    }
    CHECK(names == std::vector<std::string>{"orders.astm", "results.astm", "run1.jsonl"});
    REQUIRE(r["capture"].status == 200);
    CHECK(std::ranges::any_of(r["capture"].body["events"],
                              [](const json& e) { return e["kind"] == "watch"; }));
    CHECK(r["capture bad"].status == 400);

    CHECK(r["conform"].status == 200);
    CHECK(seen["result"]);
    REQUIRE(r["result"].body["results"].size() == 2);
    for (const auto& res : r["result"].body["results"]) {
        INFO(res.dump());
        CHECK(res["verdict"] == "pass");
    }
    CHECK(r["suite"].body.size() == 14);
    fs::remove_all(dir);
}

TEST_CASE("web API: file names are plain names in the data directory") {
    CHECK(web::valid_file_name("run-1_a.jsonl", {".jsonl"}));
    CHECK_FALSE(web::valid_file_name("../x.jsonl", {".jsonl"}));
    CHECK_FALSE(web::valid_file_name("a/b.jsonl", {".jsonl"}));
    CHECK_FALSE(web::valid_file_name(".hidden.jsonl", {".jsonl"}));
    CHECK_FALSE(web::valid_file_name("x.jsonl", {".astm"}));
    CHECK_FALSE(web::valid_file_name(".jsonl", {".jsonl"}));
    CHECK_FALSE(web::valid_file_name("a b.jsonl", {".jsonl"}));
}

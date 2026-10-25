// lisbench bridge: join two endpoints (serial <-> TCP, client <-> server)
// without looking at the bytes. lisbench record: write what one endpoint
// sends to a file.

#include <asio.hpp>

#include <csignal>
#include <fstream>
#include <optional>

#include "cmd_monitor.hpp"
#include "common.hpp"
#include "lisbench/io/relay.hpp"
#include "lisbench/monitor/capture.hpp"

namespace lisbench::cli {

namespace {

using namespace std::chrono_literals;

struct Counters {
    std::size_t a_to_b{0};
    std::size_t b_to_a{0};
};

// Opens an endpoint; in serve mode a refused connection is retried.
asio::awaitable<io::Stream> open_retrying(io::Endpoint endpoint, std::string label, bool retry) {
    while (true) {
        try {
            co_return co_await open_endpoint(endpoint, label);
        } catch (const std::system_error& e) {
            if (!retry) {
                throw;
            }
            println("{} [{}] {}; retrying in 2 s", clock_now(), label, e.what());
        }
        asio::steady_timer wait{co_await asio::this_coro::executor, 2s};
        co_await wait.async_wait(asio::use_awaitable);
    }
}

asio::awaitable<void> bridge_loop(io::Endpoint a, io::Endpoint b, bool serve, std::ofstream* save) {
    while (true) {
        auto sa = co_await open_retrying(a, "A", serve);
        auto sb = co_await open_retrying(b, "B", serve);
        Counters counters;
        co_await io::relay(
            std::move(sa), std::move(sb),
            {.on_data =
                 [&counters, save](io::Side from, std::chrono::system_clock::time_point at,
                                   bytes_view data) {
                     (from == io::Side::a ? counters.a_to_b : counters.b_to_a) += data.size();
                     if (save != nullptr) {
                         *save << monitor::to_capture_line(
                                      {.at = at,
                                       .from = from,
                                       .data = bytes(data.begin(), data.end())})
                               << '\n'
                               << std::flush;
                     }
                 },
             .on_closed =
                 [](io::Side side, const std::string& reason) {
                     println("{} [bridge] {} closed: {}", clock_now(),
                             side == io::Side::a ? "A" : "B", reason);
                 }});
        println("{} [bridge] {} bytes A->B, {} bytes B->A", clock_now(), counters.a_to_b,
                counters.b_to_a);
        if (!serve) {
            co_return;
        }
    }
}

asio::awaitable<void> record_loop(io::Endpoint from, bool serve, std::ofstream* capture,
                                  std::ofstream* raw) {
    while (true) {
        auto s = co_await open_retrying(from, "record", serve);
        std::size_t total = 0;
        co_await io::drain(
            std::move(s),
            {.on_data =
                 [&total, capture, raw](io::Side, std::chrono::system_clock::time_point at,
                                        bytes_view data) {
                     total += data.size();
                     if (capture != nullptr) {
                         *capture << monitor::to_capture_line(
                                         {.at = at,
                                          .from = io::Side::a,
                                          .data = bytes(data.begin(), data.end())})
                                  << '\n'
                                  << std::flush;
                     }
                     if (raw != nullptr) {
                         // ostream writes char; the bytes are passed on unchanged.
                         // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                         const auto* chars = reinterpret_cast<const char*>(data.data());
                         raw->write(chars, static_cast<std::streamsize>(data.size()));
                         raw->flush();
                     }
                 },
             .on_closed = {}});
        println("{} [record] {} bytes recorded", clock_now(), total);
        if (!serve) {
            co_return;
        }
    }
}

int run_until_done(asio::io_context& io, asio::awaitable<void> task) {
    asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&io](const std::error_code&, int) { io.stop(); });
    int rc = 0;
    asio::co_spawn(io, std::move(task), [&](const std::exception_ptr& e) {
        if (e) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                println(stderr, "lisbench: {}", ex.what());
                rc = 1;
            }
        }
        signals.cancel();
    });
    io.run();
    return rc;
}

}  // namespace

int cmd_bridge(const BridgeFlags& flags) {
    auto a = io::parse_endpoint(flags.a);
    auto b = io::parse_endpoint(flags.b);
    if (!a || !b) {
        println(stderr, "bridge: {}", !a ? a.error() : b.error());
        return 2;
    }
    std::optional<std::ofstream> save;
    if (!flags.save.empty()) {
        save.emplace(flags.save, std::ios::app);
        if (!*save) {
            println(stderr, "bridge: cannot write {}", flags.save);
            return 2;
        }
    }
    asio::io_context io;
    return run_until_done(io, bridge_loop(*a, *b, flags.serve, save ? &*save : nullptr));
}

int cmd_record(const RecordFlags& flags) {
    auto from = io::parse_endpoint(flags.from);
    if (!from) {
        println(stderr, "record: {}", from.error());
        return 2;
    }
    if (flags.to.empty() && flags.raw.empty()) {
        println(stderr, "record: give --to (capture) and/or --raw (bytes)");
        return 2;
    }
    std::optional<std::ofstream> capture;
    std::optional<std::ofstream> raw;
    if (!flags.to.empty()) {
        capture.emplace(flags.to, std::ios::app);
    }
    if (!flags.raw.empty()) {
        raw.emplace(flags.raw, std::ios::app | std::ios::binary);
    }
    if ((capture && !*capture) || (raw && !*raw)) {
        println(stderr, "record: cannot open output file");
        return 2;
    }
    asio::io_context io;
    return run_until_done(
        io, record_loop(*from, flags.serve, capture ? &*capture : nullptr, raw ? &*raw : nullptr));
}

}  // namespace lisbench::cli

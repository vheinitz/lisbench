// lisbench monitor: sit between two endpoints (or listen on two receive
// lines), pass everything through and show it decoded.

#include "cmd_monitor.hpp"

#include <asio.hpp>

#include <array>
#include <csignal>
#include <fstream>
#include <optional>
#include <ranges>

#include "common.hpp"
#include "lisbench/io/relay.hpp"
#include "lisbench/monitor/capture.hpp"
#include "lisbench/monitor/monitor.hpp"

namespace lisbench::cli {

namespace {

asio::awaitable<io::Stream> open(io::Endpoint endpoint, std::string label) {
    if (const auto* tcp = std::get_if<io::Tcp>(&endpoint); tcp != nullptr && tcp->listen) {
        auto acceptor = io::make_acceptor(co_await asio::this_coro::executor, *tcp);
        println("{} [monitor] {}: waiting on {}", clock_now(), label, io::to_string(endpoint));
        co_return co_await io::accept(acceptor);
    }
    auto stream = co_await io::connect(endpoint);
    println("{} [monitor] {}: connected to {}", clock_now(), label, io::to_string(endpoint));
    co_return stream;
}

asio::awaitable<void> run_relay(io::Endpoint a, io::Endpoint b, std::array<std::string, 2> labels,
                                io::RelayHandlers handlers, bool forward) {
    auto sa = co_await open(std::move(a), labels[0]);
    auto sb = co_await open(std::move(b), labels[1]);
    co_await io::relay(std::move(sa), std::move(sb), std::move(handlers), forward);
}

void show(const std::vector<monitor::Event>& events, const monitor::Options& options,
          const monitor::FormatOptions& format) {
    for (const auto& e : events) {
        for (const auto& line : monitor::format(e, options, format)) {
            println("{}", line);
        }
    }
}

}  // namespace

int cmd_monitor(const MonitorFlags& flags) {
    monitor::Options options;
    options.text = flags.text.options();
    options.slow_reply = std::chrono::milliseconds{flags.slow_reply_ms};
    {
        std::vector<std::string> labels;
        for (auto part : flags.labels | std::views::split(',')) {
            labels.emplace_back(part.begin(), part.end());
        }
        if (labels.size() != 2) {
            println(stderr, "monitor: --labels needs two names, e.g. instrument,lis");
            return 2;
        }
        options.labels = {labels[0], labels[1]};
    }
    for (const auto& text : flags.watches) {
        auto w = watch::parse(text);
        if (!w) {
            println(stderr, "monitor: {}", w.error());
            return 2;
        }
        options.watches.push_back(std::move(*w));
    }
    const monitor::FormatOptions format{.verbose = flags.verbose, .decode = flags.decode};
    monitor::Monitor mon{options};

    if (!flags.replay.empty()) {
        std::ifstream in(flags.replay);
        if (!in) {
            println(stderr, "monitor: cannot open {}", flags.replay);
            return 2;
        }
        std::string line;
        std::size_t number = 0;
        auto last = monitor::Clock::time_point{};
        while (std::getline(in, line)) {
            ++number;
            if (line.empty()) {
                continue;
            }
            const auto c = monitor::parse_capture_line(line);
            if (!c) {
                println(stderr, "{}:{}: {}", flags.replay, number, c.error());
                return 2;
            }
            last = c->at;
            show(mon.feed(c->from, c->at, c->data), options, format);
        }
        show(mon.finish(last), options, format);
        for (const auto& s : monitor::summary(mon)) {
            println("{}", s);
        }
        return 0;
    }

    auto a = io::parse_endpoint(flags.a);
    auto b = io::parse_endpoint(flags.b);
    if (!a || !b) {
        println(stderr, "monitor: {}", !a ? a.error() : b.error());
        return 2;
    }
    std::optional<std::ofstream> save;
    if (!flags.save.empty()) {
        save.emplace(flags.save, std::ios::app);
        if (!*save) {
            println(stderr, "monitor: cannot write {}", flags.save);
            return 2;
        }
    }

    asio::io_context io;
    asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&io](const std::error_code&, int) { io.stop(); });
    io::RelayHandlers handlers{
        .on_data =
            [&](io::Side from, monitor::Clock::time_point at, bytes_view data) {
                if (save) {
                    *save << monitor::to_capture_line(
                                 {.at = at, .from = from, .data = bytes(data.begin(), data.end())})
                          << '\n'
                          << std::flush;
                }
                show(mon.feed(from, at, data), options, format);
            },
        .on_closed =
            [&](io::Side side, const std::string& reason) {
                println("{} [monitor] {} closed: {}", clock_now(),
                        options.labels.at(side == io::Side::a ? 0 : 1), reason);
            },
    };
    int rc = 0;
    asio::co_spawn(io, run_relay(*a, *b, options.labels, handlers, !flags.tap),
                   [&](const std::exception_ptr& e) {
                       if (e) {
                           try {
                               std::rethrow_exception(e);
                           } catch (const std::exception& ex) {
                               println(stderr, "monitor: {}", ex.what());
                               rc = 1;
                           }
                       }
                       signals.cancel();
                   });
    io.run();
    show(mon.finish(monitor::Clock::now()), options, format);
    for (const auto& s : monitor::summary(mon)) {
        println("{}", s);
    }
    return rc;
}

}  // namespace lisbench::cli

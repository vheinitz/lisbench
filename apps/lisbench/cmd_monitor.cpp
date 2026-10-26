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
#include "lisbench/io/command.hpp"
#include "lisbench/io/relay.hpp"
#include "lisbench/monitor/capture.hpp"
#include "lisbench/monitor/monitor.hpp"
#include "lisbench/monitor/trigger.hpp"
#include "lisbench/printable.hpp"

namespace lisbench::cli {

namespace {}  // namespace

asio::awaitable<io::Stream> open_endpoint(io::Endpoint endpoint, std::string label) {
    if (const auto* tcp = std::get_if<io::Tcp>(&endpoint); tcp != nullptr && tcp->listen) {
        auto acceptor = io::make_acceptor(co_await asio::this_coro::executor, *tcp);
        println("{} [{}] waiting on {}", clock_now(), label, io::to_string(endpoint));
        co_return co_await io::accept(acceptor);
    }
    auto stream = co_await io::connect(endpoint);
    println("{} [{}] connected to {}", clock_now(), label, io::to_string(endpoint));
    co_return stream;
}

namespace {

asio::awaitable<void> run_relay(io::Endpoint a, io::Endpoint b, std::array<std::string, 2> labels,
                                io::RelayHandlers handlers, bool forward) {
    auto sa = co_await open_endpoint(std::move(a), labels[0]);
    auto sb = co_await open_endpoint(std::move(b), labels[1]);
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

// Carries out the triggers for the watch events among `events`.
class Triggers {
public:
    // triggers[i] belongs to Options::watches[first_watch + i].
    Triggers(std::vector<monitor::Trigger> triggers, std::size_t first_watch,
             std::array<std::string, 2> labels)
        : triggers_{std::move(triggers)}, first_watch_{first_watch}, labels_{std::move(labels)} {}

    void set_save(std::ofstream* save) { save_ = save; }
    [[nodiscard]] bool stop_requested() const noexcept { return stop_; }

    void on_events(const std::vector<monitor::Event>& events) {
        for (const auto& e : events) {
            if (e.kind != monitor::Kind::watch || !e.watch || *e.watch < first_watch_ ||
                *e.watch - first_watch_ >= triggers_.size()) {
                continue;
            }
            const auto& t = triggers_[*e.watch - first_watch_];
            std::visit(overloaded{[&](const monitor::Exec& x) { exec(x, t, e); },
                                  [&](const monitor::Mark& m) { mark(m, e); },
                                  [&](const monitor::Stop&) {
                                      println("{} [trigger] stop: {}", clock_now(), t.watch.text);
                                      stop_ = true;
                                  }},
                       t.action);
        }
    }

    void drain() { commands_.drain(); }

private:
    template <typename... Ts>
    struct overloaded : Ts... {
        using Ts::operator()...;
    };

    void exec(const monitor::Exec& x, const monitor::Trigger& t, const monitor::Event& e) {
        const auto time =
            std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(e.at));
        io::Command c{.command = x.command,
                      .env = {{"LISBENCH_WATCH", t.watch.text},
                              {"LISBENCH_VALUE", e.value},
                              {"LISBENCH_RECORD", printable(e.raw)},
                              {"LISBENCH_FROM", labels_.at(e.from == io::Side::a ? 0 : 1)},
                              {"LISBENCH_TIME", time}}};
        if (!commands_.submit(std::move(c))) {
            println(stderr, "{} [trigger] queue full, not run: {}", clock_now(), x.command);
        }
    }

    void mark(const monitor::Mark& m, const monitor::Event& e) {
        if (save_ != nullptr) {
            *save_ << monitor::to_capture_line({.at = e.at, .from = e.from, .mark = m.text}) << '\n'
                   << std::flush;
        }
        println("{} [trigger] mark: {}", clock_now(), m.text);
    }

    std::vector<monitor::Trigger> triggers_;
    std::size_t first_watch_;
    std::array<std::string, 2> labels_;
    std::ofstream* save_{nullptr};
    bool stop_{false};
    io::CommandQueue commands_{64, [](const io::CommandResult& r) {
                                   if (!r.error.empty()) {
                                       println(stderr, "{} [trigger] cannot run '{}': {}",
                                               clock_now(), r.command.command, r.error);
                                   } else if (r.exit_code != 0) {
                                       println(stderr, "{} [trigger] '{}' exited with {}",
                                               clock_now(), r.command.command, r.exit_code);
                                   }
                               }};
};

}  // namespace

int cmd_monitor(const MonitorFlags& flags) {
    monitor::Options options;
    options.text = flags.text.options();
    if (flags.text.loaded) {
        options.max_text = flags.text.loaded->max_text.value_or(options.max_text);
    }
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
    std::vector<monitor::Trigger> triggers;
    bool needs_save = false;
    for (const auto& text : flags.triggers) {
        auto t = monitor::parse_trigger(text);
        if (!t) {
            println(stderr, "monitor: {}", t.error());
            return 2;
        }
        needs_save = needs_save || std::holds_alternative<monitor::Mark>(t->action);
        triggers.push_back(std::move(*t));
    }
    if (needs_save && (flags.save.empty() || !flags.replay.empty())) {
        println(stderr, "monitor: a mark trigger needs --save (and is ignored on --replay)");
        if (flags.replay.empty()) {
            return 2;
        }
    }
    const auto first_trigger_watch = options.watches.size();
    for (const auto& t : triggers) {
        options.watches.push_back(t.watch);
    }
    Triggers actions{std::move(triggers), first_trigger_watch, options.labels};
    if (flags.replay.empty()) {
        // Wrong URIs are reported below, where they are opened.
        const auto a = io::parse_endpoint(flags.a);
        const auto b = io::parse_endpoint(flags.b);
        if (a && b) {
            options.low_level = {io::low_level_of(*a), io::low_level_of(*b)};
            if (options.low_level[0] != options.low_level[1]) {
                // The monitor passes bytes on as they are; converting needs a gateway.
                println(stderr, "monitor: A and B use different low-level protocols ({} and {})",
                        to_string(options.low_level[0]), to_string(options.low_level[1]));
                return 2;
            }
        }
    } else {
        const auto l = low_level_from(flags.low_level);
        if (!l) {
            println(stderr, "monitor: --low-level is lis01-a2, none or mllp");
            return 2;
        }
        options.low_level = {*l, *l};
    }
    const monitor::FormatOptions format{.verbose = flags.verbose, .decode = flags.decode};
    monitor::Monitor mon{options};
    const auto handle = [&](const std::vector<monitor::Event>& events) {
        show(events, options, format);
        actions.on_events(events);
    };

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
            if (!c->mark.empty()) {
                println("{:%T} --- mark: {}", std::chrono::floor<std::chrono::milliseconds>(c->at),
                        c->mark);
                continue;
            }
            handle(mon.feed(c->from, c->at, c->data));
            if (actions.stop_requested()) {
                break;
            }
        }
        handle(mon.finish(last));
        actions.drain();
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
        actions.set_save(&*save);
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
                handle(mon.feed(from, at, data));
                if (actions.stop_requested()) {
                    io.stop();
                }
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
    handle(mon.finish(monitor::Clock::now()));
    actions.drain();
    for (const auto& s : monitor::summary(mon)) {
        println("{}", s);
    }
    return rc;
}

}  // namespace lisbench::cli

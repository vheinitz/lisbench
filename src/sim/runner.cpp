#include "lisbench/sim/runner.hpp"

#include <asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>

#include "lisbench/lis02_message.hpp"

namespace lisbench::sim {

namespace {

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

lis01::Message message_of(const std::vector<std::string>& records) {
    lis01::Message m;
    m.records.reserve(records.size());
    for (const auto& r : records) {
        m.records.push_back(to_bytes(r));
    }
    return m;
}

void add(Report& report, const Observers& obs, StepReport step) {
    report.ok = report.ok && step.ok;
    if (obs.on_step) {
        obs.on_step(step);
    }
    report.steps.push_back(std::move(step));
}

StepReport check(std::size_t index, const ExpectStep& x, const std::optional<io::Transfer>& t) {
    const auto fail = [index](std::string text) {
        return StepReport{.index = index, .ok = false, .text = std::move(text)};
    };
    if (!t) {
        return fail(std::format("no transfer within {} ms", x.within.count()));
    }
    if (t->complete != x.complete) {
        return fail(
            std::format("transfer {} ({})", t->complete ? "complete" : "incomplete", t->reason));
    }
    if (x.records && t->records.size() != *x.records) {
        return fail(
            std::format("{} records where {} were expected", t->records.size(), *x.records));
    }
    std::vector<std::string> wire;
    wire.reserve(t->records.size());
    for (const auto& r : t->records) {
        wire.push_back(to_string(r));
    }
    for (const auto& needle : x.contains) {
        if (std::ranges::none_of(wire,
                                 [&needle](const std::string& r) { return r.contains(needle); })) {
            return fail(std::format("no record contains '{}'", needle));
        }
    }
    if (x.valid) {
        const auto message = lis02::parse(wire);
        if (!message) {
            return fail(
                std::format("record {}: {}", message.error().record + 1, message.error().text));
        }
        for (const auto& issue : lis02::validate(*message)) {
            if (issue.severity == lis02::Severity::error) {
                return fail(std::format("record {}: {}", issue.record + 1, issue.text));
            }
        }
    }
    return StepReport{
        .index = index, .ok = true, .text = std::format("received {} records", t->records.size())};
}

// Called and awaited inside run(), which owns all four arguments.
// NOLINTBEGIN(cppcoreguidelines-avoid-reference-coroutine-parameters)
asio::awaitable<void> run_steps(io::Session& session, const Scenario& scenario, Report& report,
                                const Observers& obs) {
    // NOLINTEND(cppcoreguidelines-avoid-reference-coroutine-parameters)
    for (std::size_t i = 0; i < scenario.steps.size(); ++i) {
        const auto& step = scenario.steps[i];
        if (const auto* s = std::get_if<SendStep>(&step)) {
            const auto outcome = co_await session.send(message_of(s->records));
            add(report, obs,
                StepReport{.index = i,
                           .ok = outcome == s->expect,
                           .text = std::format(
                               "send {} ({} records): {}{}", s->source, s->records.size(),
                               lis01::to_string(outcome),
                               outcome == s->expect
                                   ? ""
                                   : std::format(", expected {}", lis01::to_string(s->expect)))});
        } else if (const auto* x = std::get_if<ExpectStep>(&step)) {
            const auto t = co_await session.receive(x->within);
            add(report, obs, check(i, *x, t));
        } else if (const auto* w = std::get_if<WaitStep>(&step)) {
            asio::steady_timer timer{co_await asio::this_coro::executor, w->duration};
            co_await timer.async_wait(use_tuple);
            add(report, obs,
                StepReport{.index = i,
                           .ok = true,
                           .text = std::format("waited {} ms", w->duration.count())});
        }
    }
    if (!scenario.steps.empty()) {
        session.stop();
    }
}

bool has_query(const io::Transfer& t) {
    return std::ranges::any_of(
        t.records, [](const bytes& r) { return !r.empty() && r.front() == std::byte{'Q'}; });
}

}  // namespace

asio::awaitable<Report> run(Scenario scenario, Observers observers,
                            asio::ip::tcp::acceptor* acceptor) {
    Report report;
    const io::SessionOptions options{.role = scenario.role,
                                     .timing = scenario.timing,
                                     .faults = scenario.faults,
                                     .low_level = io::low_level_of(scenario.endpoint)};
    if (const auto ok = io::check(options); !ok) {
        add(report, observers, StepReport{.index = 0, .ok = false, .text = ok.error()});
        co_return report;
    }
    const auto executor = co_await asio::this_coro::executor;
    const auto* tcp = std::get_if<io::Tcp>(&scenario.endpoint);
    const bool listening = tcp != nullptr && tcp->listen;
    std::optional<asio::ip::tcp::acceptor> own;
    if (listening && acceptor == nullptr) {
        own.emplace(io::make_acceptor(executor, *tcp));
        acceptor = &*own;
    }
    const auto info = [&observers](const std::string& text) {
        if (observers.on_info) {
            observers.on_info(text);
        }
    };

    while (true) {
        std::optional<io::Stream> stream;
        try {
            if (listening) {
                info(std::format("listening on {}:{}", tcp->host,
                                 acceptor->local_endpoint().port()));
                stream.emplace(co_await io::accept(*acceptor));
            } else {
                stream.emplace(co_await io::connect(scenario.endpoint));
            }
        } catch (const std::system_error& e) {
            add(report, observers,
                StepReport{.index = 0,
                           .ok = false,
                           .text = std::format("cannot open {}: {}",
                                               io::to_string(scenario.endpoint), e.what())});
            co_return report;
        }
        ++report.connections;
        info(std::format("connected ({})", io::to_string(scenario.endpoint)));

        io::Session* current = nullptr;
        auto handlers = observers.session;
        if (scenario.answer_queries) {
            handlers.on_transfer = [&current, &scenario,
                                    user = observers.session.on_transfer](const io::Transfer& t) {
                if (user) {
                    user(t);
                }
                if (current != nullptr && t.complete && has_query(t)) {
                    (void)current->submit(message_of(*scenario.answer_queries));
                }
            };
        }
        io::Session session{std::move(*stream), options, std::move(handlers)};
        current = &session;
        using namespace asio::experimental::awaitable_operators;
        co_await (session.run() && run_steps(session, scenario, report, observers));
        current = nullptr;
        info("disconnected");
        if (!listening || !scenario.serve) {
            break;
        }
    }
    co_return report;
}

}  // namespace lisbench::sim

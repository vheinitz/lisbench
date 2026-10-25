#include "lisbench/web/app.hpp"

#include <asio/experimental/awaitable_operators.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <optional>

#include "lisbench/conform/tester.hpp"
#include "lisbench/io/faults.hpp"
#include "lisbench/io/relay.hpp"
#include "lisbench/monitor/capture.hpp"
#include "lisbench/monitor/monitor.hpp"
#include "lisbench/printable.hpp"
#include "lisbench/sim/profile.hpp"
#include "lisbench/sim/runner.hpp"
#include "lisbench/sim/scenario.hpp"

namespace lisbench::web {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace asio::experimental::awaitable_operators;

namespace {

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t max_capture_events = 50'000;

std::string clock_text(std::chrono::system_clock::time_point t) {
    return std::format("{:%T}", std::chrono::floor<std::chrono::milliseconds>(t));
}

// A request body field, with a readable error when it has the wrong type.
template <typename T>
T field(const json& body, const char* name, T fallback) {
    if (!body.contains(name) || body[name].is_null()) {
        return fallback;
    }
    try {
        return body[name].get<T>();
    } catch (const json::exception&) {
        throw std::invalid_argument(std::format("'{}' has the wrong type", name));
    }
}

asio::awaitable<io::Stream> open(io::Endpoint e) {
    if (const auto* tcp = std::get_if<io::Tcp>(&e); tcp != nullptr && tcp->listen) {
        auto acceptor = io::make_acceptor(co_await asio::this_coro::executor, *tcp);
        co_return co_await io::accept(acceptor);
    }
    co_return co_await io::connect(e);
}

// A task that can be stopped from the browser: the work races a timer that
// stop() cancels.
struct Task {
    explicit Task(const asio::any_io_executor& ex) : stop_timer{ex} {}
    bool running{false};
    bool stop_requested{false};
    asio::steady_timer stop_timer;
    json info = json::object();

    asio::awaitable<void> until_stopped() {
        if (stop_requested) {
            co_return;
        }
        stop_timer.expires_at(asio::steady_timer::time_point::max());
        (void)co_await stop_timer.async_wait(use_tuple);
    }
    void begin(json what) {
        running = true;
        stop_requested = false;
        info = std::move(what);
    }
    void stop() {
        stop_requested = true;
        stop_timer.cancel();
    }
    [[nodiscard]] json state() const {
        auto j = info;
        j["running"] = running;
        return j;
    }
};

json event_json(const monitor::Event& e, const monitor::Options& o) {
    auto lines = monitor::format(e, o, {.verbose = true, .decode = true});
    return {{"t", clock_text(e.at)},
            {"from", o.labels.at(e.from == io::Side::a ? 0 : 1)},
            {"kind", monitor::to_string(e.kind)},
            {"lines", std::move(lines)}};
}

}  // namespace

namespace {

std::string_view kind_of(std::string_view name) {
    if (name.ends_with(".jsonl")) {
        return "capture";
    }
    return name.ends_with(".astm") ? "message" : "scenario";
}

}  // namespace

bool valid_file_name(std::string_view name, std::initializer_list<std::string_view> extensions) {
    if (name.empty() || name.size() > 128 || name.front() == '.') {
        return false;
    }
    const bool chars_ok = std::ranges::all_of(name, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' || c == '-';
    });
    return chars_ok && std::ranges::any_of(extensions, [&](std::string_view ext) {
               return name.size() > ext.size() && name.ends_with(ext);
           });
}

struct App::State {
    State(Server& s, AppOptions o)
        : server{s},
          options{std::move(o)},
          executor{s.executor()},
          monitor_task{executor},
          sim_task{executor},
          conform_task{executor} {}

    Server& server;
    AppOptions options;
    asio::any_io_executor executor;
    Task monitor_task;
    Task sim_task;
    Task conform_task;
    json last_result = nullptr;

    void publish(std::string_view event, const json& data) const {
        server.publish(event, data.dump());
    }
    void publish_status() const { publish("status", status()); }
    void problem(std::string_view source, std::string_view text) const {
        publish("problem", {{"source", source}, {"text", text}});
    }
    [[nodiscard]] json status() const {
        return {{"version", options.version},
                {"data_dir", options.data_dir.string()},
                {"monitor", monitor_task.state()},
                {"sim", sim_task.state()},
                {"conform", conform_task.state()}};
    }

    [[nodiscard]] fs::path data_file(const std::string& name,
                                     std::initializer_list<std::string_view> extensions) const {
        if (options.data_dir.empty()) {
            throw std::invalid_argument("no data directory (start with --captures DIR)");
        }
        if (!valid_file_name(name, extensions)) {
            throw std::invalid_argument(std::format("'{}' is not a valid file name", name));
        }
        return options.data_dir / name;
    }

    [[nodiscard]] monitor::Options monitor_options(const json& body) const {
        monitor::Options o;
        const auto labels = field<std::vector<std::string>>(body, "labels", {"A", "B"});
        if (labels.size() != 2) {
            throw std::invalid_argument("labels needs two names");
        }
        o.labels = {labels[0], labels[1]};
        for (const auto& w : field<std::vector<std::string>>(body, "watches", {})) {
            if (w.empty()) {
                continue;
            }
            auto parsed = watch::parse(w);
            if (!parsed) {
                throw std::invalid_argument(parsed.error());
            }
            o.watches.push_back(std::move(*parsed));
        }
        if (const auto profile = field<std::string>(body, "profile", ""); !profile.empty()) {
            const auto p = sim::load_profile(profile, options.profile_search);
            if (!p) {
                throw std::invalid_argument(p.error());
            }
            o.text = p->text;
            o.max_text = p->max_text.value_or(o.max_text);
        }
        return o;
    }

    // --- monitor ----------------------------------------------------------

    Response monitor_start(const json& body) {
        if (monitor_task.running) {
            return error(409, "the monitor is already running");
        }
        const auto a = io::parse_endpoint(field<std::string>(body, "a", ""));
        const auto tap = field<bool>(body, "tap", false);
        const auto b = io::parse_endpoint(field<std::string>(body, "b", ""));
        if (!a || !b) {
            return error(400, !a ? a.error() : b.error());
        }
        auto o = monitor_options(body);
        std::optional<fs::path> save;
        if (const auto name = field<std::string>(body, "save", ""); !name.empty()) {
            save = data_file(name.ends_with(".jsonl") ? name : name + ".jsonl", {".jsonl"});
        }
        monitor_task.begin({{"a", io::to_string(*a)},
                            {"b", io::to_string(*b)},
                            {"tap", tap},
                            {"save", save ? save->filename().string() : ""}});
        publish_status();
        asio::co_spawn(executor, run_monitor(*a, *b, std::move(o), tap, save), asio::detached);
        return json_response(R"({"started":true})");
    }

    asio::awaitable<void> run_monitor(io::Endpoint a, io::Endpoint b, monitor::Options o, bool tap,
                                      std::optional<fs::path> save_to) {
        monitor::Monitor mon{o};
        std::optional<std::ofstream> save;
        if (save_to) {
            save.emplace(*save_to, std::ios::app);
        }
        const auto handlers = io::RelayHandlers{
            .on_data =
                [&](io::Side from, std::chrono::system_clock::time_point at, bytes_view data) {
                    if (save) {
                        *save << monitor::to_capture_line({.at = at,
                                                           .from = from,
                                                           .data = bytes(data.begin(), data.end())})
                              << '\n'
                              << std::flush;
                    }
                    for (const auto& e : mon.feed(from, at, data)) {
                        publish("monitor", event_json(e, o));
                    }
                },
            .on_closed =
                [&](io::Side side, const std::string& reason) {
                    // The end of a connection is news, not a fault.
                    publish(
                        "monitor-info",
                        {{"text", std::format("{} closed: {}",
                                              o.labels.at(side == io::Side::a ? 0 : 1), reason)}});
                },
        };
        // Awaited below, inside this frame, so the references stay valid.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        const auto work = [&]() -> asio::awaitable<void> {
            auto sa = co_await open(a);
            publish("monitor-info", {{"text", std::format("{} connected", o.labels[0])}});
            auto sb = co_await open(b);
            publish("monitor-info", {{"text", std::format("{} connected", o.labels[1])}});
            co_await io::relay(std::move(sa), std::move(sb), handlers, !tap);
        };
        try {
            co_await (work() || monitor_task.until_stopped());
        } catch (const std::exception& e) {
            problem("monitor", e.what());
        }
        for (const auto& e : mon.finish(std::chrono::system_clock::now())) {
            publish("monitor", event_json(e, o));
        }
        publish("summary", {{"source", "monitor"}, {"lines", monitor::summary(mon)}});
        monitor_task.running = false;
        publish_status();
    }

    // --- captures and files ------------------------------------------------

    [[nodiscard]] json files() const {
        json list = json::array();
        if (options.data_dir.empty()) {
            return list;
        }
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator{options.data_dir, ec}) {
            const auto name = entry.path().filename().string();
            if (!entry.is_regular_file() || !valid_file_name(name, {".jsonl", ".astm", ".yaml"})) {
                continue;
            }
            list.push_back({{"name", name}, {"size", entry.file_size()}, {"kind", kind_of(name)}});
        }
        // json's iterators do not model the ranges concepts, so no ranges::sort.
        // NOLINTNEXTLINE(modernize-use-ranges)
        std::sort(list.begin(), list.end(), [](const json& x, const json& y) {
            return x["name"].get<std::string>() < y["name"].get<std::string>();
        });
        return list;
    }

    [[nodiscard]] json capture(const std::string& name, const json& body) const {
        const auto path = data_file(name, {".jsonl"});
        std::ifstream in(path);
        if (!in) {
            throw std::invalid_argument(std::format("cannot open {}", name));
        }
        const auto o = monitor_options(body);
        monitor::Monitor mon{o};
        json events = json::array();
        std::string line;
        std::size_t number = 0;
        auto last = std::chrono::system_clock::time_point{};
        bool truncated = false;
        while (std::getline(in, line)) {
            ++number;
            if (line.empty()) {
                continue;
            }
            const auto c = monitor::parse_capture_line(line);
            if (!c) {
                throw std::invalid_argument(std::format("{}:{}: {}", name, number, c.error()));
            }
            last = c->at;
            if (!c->mark.empty()) {
                events.push_back({{"t", clock_text(c->at)},
                                  {"from", ""},
                                  {"kind", "mark"},
                                  {"lines", {std::format("--- mark: {}", c->mark)}}});
                continue;
            }
            for (const auto& e : mon.feed(c->from, c->at, c->data)) {
                if (events.size() >= max_capture_events) {
                    truncated = true;
                    break;
                }
                events.push_back(event_json(e, o));
            }
        }
        for (const auto& e : mon.finish(last)) {
            events.push_back(event_json(e, o));
        }
        return {{"name", name},
                {"events", std::move(events)},
                {"truncated", truncated},
                {"summary", monitor::summary(mon)}};
    }

    // --- conformance --------------------------------------------------------

    Response conform_start(const json& body) {
        if (conform_task.running) {
            return error(409, "a conformance test is already running");
        }
        const auto iut = field<std::string>(body, "iut", "computer");
        if (iut != "computer" && iut != "instrument") {
            return error(400, "iut is computer or instrument");
        }
        const auto endpoint = io::parse_endpoint(field<std::string>(body, "endpoint", ""));
        if (!endpoint) {
            return error(400, endpoint.error());
        }
        conform::Options o{
            .iut = iut == "instrument" ? conform::Iut::instrument : conform::Iut::computer,
            .timing =
                field<bool>(body, "fast", false) ? lis01::fast_test_timing() : lis01::Timing{},
            .only = field<std::vector<std::string>>(body, "only", {}),
            .wait_for_iut = std::chrono::seconds{field<int>(body, "wait", 60)},
            .honours_receiver_interrupt = std::nullopt};
        for (const auto& id : o.only) {
            if (conform::find(id) == nullptr) {
                return error(400, std::format("unknown test case '{}'", id));
            }
        }
        std::string pics = "none";
        if (const auto profile = field<std::string>(body, "profile", ""); !profile.empty()) {
            const auto p = sim::load_profile(profile, options.profile_search);
            if (!p) {
                return error(400, p.error());
            }
            sim::apply(*p, o.timing);
            o.honours_receiver_interrupt = p->honours_receiver_interrupt;
            pics = p->id;
        }
        conform_task.begin({{"iut", iut}, {"endpoint", io::to_string(*endpoint)}, {"pics", pics}});
        publish_status();
        asio::co_spawn(executor, run_conform(*endpoint, std::move(o)), asio::detached);
        return json_response(R"({"started":true})");
    }

    asio::awaitable<void> run_conform(io::Endpoint endpoint, conform::Options o) {
        std::vector<conform::Result> results;
        // Awaited below, inside this frame, so the references stay valid.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
        const auto work = [&]() -> asio::awaitable<void> {
            auto stream = co_await open(endpoint);
            results = co_await conform::run(std::move(stream), o, [this](std::string_view line) {
                publish("conform",
                        {{"line", std::format("{} {}", clock_text(std::chrono::system_clock::now()),
                                              line)}});
            });
        };
        try {
            co_await (work() || conform_task.until_stopped());
        } catch (const std::exception& e) {
            problem("conform", e.what());
        }
        json list = json::array();
        for (const auto& r : results) {
            const auto* tc = conform::find(r.id);
            list.push_back(
                {{"id", r.id},
                 {"purpose", tc != nullptr ? tc->purpose : ""},
                 {"level", tc != nullptr ? conform::to_string(tc->level) : ""},
                 {"verdict", conform::to_string(r.verdict)},
                 {"detail", r.detail},
                 {"measured_ms", r.measured ? json(r.measured->count()) : json(nullptr)}});
        }
        const auto lines = conform::report(results);
        last_result = {{"setup", conform_task.info},
                       {"results", std::move(list)},
                       {"summary", lines.empty() ? "" : lines.back()},
                       {"stopped", conform_task.stop_requested}};
        publish("result", last_result);
        conform_task.running = false;
        publish_status();
    }

    // --- simulator -----------------------------------------------------------

    Response sim_start(const json& body) {
        if (sim_task.running) {
            return error(409, "the simulator is already running");
        }
        sim::Scenario s;
        if (const auto name = field<std::string>(body, "scenario", ""); !name.empty()) {
            auto loaded = sim::load_scenario(data_file(name, {".yaml"}));
            if (!loaded) {
                return error(400, loaded.error());
            }
            s = std::move(*loaded);
        } else {
            const auto role = field<std::string>(body, "role", "computer");
            if (role != "computer" && role != "instrument") {
                return error(400, "role is computer or instrument");
            }
            const auto endpoint = io::parse_endpoint(field<std::string>(body, "endpoint", ""));
            if (!endpoint) {
                return error(400, endpoint.error());
            }
            s.name = "web";
            s.role = role == "instrument" ? lis01::Role::instrument : lis01::Role::computer;
            s.endpoint = *endpoint;
            s.serve = field<bool>(body, "serve", false);
            for (const auto& file : field<std::vector<std::string>>(body, "send", {})) {
                auto records = sim::read_records(data_file(file, {".astm"}));
                if (!records) {
                    return error(400, records.error());
                }
                s.steps.emplace_back(sim::SendStep{.records = std::move(*records), .source = file});
            }
            if (const auto answer = field<std::string>(body, "answer", ""); !answer.empty()) {
                auto records = sim::read_records(data_file(answer, {".astm"}));
                if (!records) {
                    return error(400, records.error());
                }
                s.answer_queries = std::move(*records);
            }
        }
        for (const auto& spec : field<std::vector<std::string>>(body, "faults", {})) {
            auto f = io::parse_fault(spec);
            if (!f) {
                return error(400, f.error());
            }
            s.faults.push_back(*f);
        }
        if (field<bool>(body, "fast", false)) {
            const auto winner = s.timing.contention_winner;
            s.timing = lis01::fast_test_timing();
            s.timing.contention_winner = winner;
        }
        sim_task.begin({{"name", s.name},
                        {"role", s.role == lis01::Role::computer ? "computer" : "instrument"},
                        {"endpoint", io::to_string(s.endpoint)}});
        publish_status();
        asio::co_spawn(executor, run_sim(std::move(s)), asio::detached);
        return json_response(R"({"started":true})");
    }

    asio::awaitable<void> run_sim(sim::Scenario s) {
        sim::Observers obs;
        const auto log = [this](std::string text, std::vector<std::string> lines = {}) {
            publish("sim", {{"t", clock_text(std::chrono::system_clock::now())},
                            {"text", std::move(text)},
                            {"lines", std::move(lines)}});
        };
        obs.on_info = [log](const std::string& text) { log(text); };
        obs.on_step = [log](const sim::StepReport& r) {
            log(std::format("step {} {}: {}", r.index + 1, r.ok ? "ok" : "FAILED", r.text));
        };
        obs.session.on_warning = [log](const std::string& text) { log("warning: " + text); };
        obs.session.on_transfer = [log](const io::Transfer& t) {
            std::vector<std::string> lines;
            lines.reserve(t.records.size());
            for (const auto& r : t.records) {
                lines.push_back(printable(r));
            }
            log(std::format("received {} records{}", t.records.size(),
                            t.complete ? "" : " (INCOMPLETE: " + t.reason + ")"),
                std::move(lines));
        };
        try {
            // Awaited below, inside this frame, so the references stay valid.
            // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
            const auto work = [&]() -> asio::awaitable<void> {
                const auto report = co_await sim::run(std::move(s), obs);
                log(std::format("finished: {}", report.ok ? "ok" : "with failed steps"));
            };
            co_await (work() || sim_task.until_stopped());
        } catch (const std::exception& e) {
            problem("sim", e.what());
        }
        sim_task.running = false;
        publish_status();
    }

    // --- routes ---------------------------------------------------------------

    void install() {
        const auto post = [this](std::string path, std::function<Response(const json&)> fn) {
            // The handler lives in the server's route table as long as the server.
            server.route("POST", std::move(path),
                         // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                         [fn = std::move(fn)](Request r) -> asio::awaitable<Response> {
                             const auto body = r.body.empty() ? json::object()
                                                              : json::parse(r.body, nullptr, false);
                             if (body.is_discarded() || !body.is_object()) {
                                 co_return error(400, "the body must be a JSON object");
                             }
                             try {
                                 co_return fn(body);
                             } catch (const std::invalid_argument& e) {
                                 co_return error(400, e.what());
                             }
                         });
        };
        const auto get = [this](std::string path, std::function<Response(const Request&)> fn) {
            server.route("GET", std::move(path),
                         // NOLINTNEXTLINE(cppcoreguidelines-avoid-capturing-lambda-coroutines)
                         [fn = std::move(fn)](Request r) -> asio::awaitable<Response> {
                             try {
                                 co_return fn(r);
                             } catch (const std::invalid_argument& e) {
                                 co_return error(400, e.what());
                             }
                         });
        };
        get("/api/status", [this](const Request&) { return json_response(status().dump()); });
        get("/api/files", [this](const Request&) { return json_response(files().dump()); });
        get("/api/capture", [this](const Request& r) {
            json body = json::object();
            if (r.query.contains("watch")) {
                body["watches"] = json::array({r.query.at("watch")});
            }
            if (r.query.contains("profile")) {
                body["profile"] = r.query.at("profile");
            }
            return json_response(
                capture(r.query.contains("name") ? r.query.at("name") : "", body).dump());
        });
        get("/api/conform/result",
            [this](const Request&) { return json_response(last_result.dump()); });
        get("/api/suite", [](const Request&) {
            json list = json::array();
            for (const auto& tc : conform::suite) {
                list.push_back({{"id", tc.id},
                                {"purpose", tc.purpose},
                                {"level", conform::to_string(tc.level)}});
            }
            return json_response(list.dump());
        });
        post("/api/monitor/start", [this](const json& b) { return monitor_start(b); });
        post("/api/monitor/stop", [this](const json&) {
            monitor_task.stop();
            return json_response(R"({"stopping":true})");
        });
        post("/api/conform/start", [this](const json& b) { return conform_start(b); });
        post("/api/conform/stop", [this](const json&) {
            conform_task.stop();
            return json_response(R"({"stopping":true})");
        });
        post("/api/sim/start", [this](const json& b) { return sim_start(b); });
        post("/api/sim/stop", [this](const json&) {
            sim_task.stop();
            return json_response(R"({"stopping":true})");
        });
    }
};

App::App(Server& server, AppOptions options)
    : state_{std::make_unique<State>(server, std::move(options))} {
    state_->install();
}

App::~App() = default;

void App::stop_all() {
    state_->monitor_task.stop();
    state_->sim_task.stop();
    state_->conform_task.stop();
}

}  // namespace lisbench::web

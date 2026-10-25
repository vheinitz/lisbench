#include "lisbench/session/manager.hpp"

#include <asio/experimental/awaitable_operators.hpp>

#include <algorithm>
#include <deque>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>

#include "lisbench/io/session.hpp"
#include "lisbench/printable.hpp"
#include "lisbench/session/device_model.hpp"
#include "lisbench/sim/profile.hpp"

namespace lisbench::session {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace asio::experimental::awaitable_operators;
using std::chrono::milliseconds;

namespace {

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);
constexpr std::size_t log_keep = 500;
constexpr std::size_t specimens_keep = 50;

std::string clock_text() {
    return std::format("{:%T}", std::chrono::floor<milliseconds>(std::chrono::system_clock::now()));
}

lis01::Message message_of(const std::vector<std::string>& records) {
    lis01::Message m;
    for (const auto& r : records) {
        m.records.push_back(to_bytes(r));
    }
    return m;
}

std::string resource_of(const LinkConfig& l) {
    switch (l.transport) {
        case Transport::serial: return "serial port " + l.serial.device;
        case Transport::tcp_server: return std::format("TCP port {}", l.port);
        case Transport::tcp_client: return "";
    }
    return "";
}

asio::awaitable<void> sleep_for(milliseconds d) {
    asio::steady_timer t{co_await asio::this_coro::executor, d};
    (void)co_await t.async_wait(use_tuple);
}

}  // namespace

std::string_view to_string(RunState s) noexcept {
    switch (s) {
        case RunState::stopped: return "stopped";
        case RunState::running: return "running";
        case RunState::stopping: return "stopping";
        case RunState::failed: return "failed";
    }
    return "?";
}

std::string_view to_string(LinkState s) noexcept {
    switch (s) {
        case LinkState::waiting: return "waiting";
        case LinkState::connecting: return "connecting";
        case LinkState::connected: return "connected";
        case LinkState::closed: return "closed";
        case LinkState::failed: return "failed";
    }
    return "?";
}

json to_json(const SessionStatus& s) {
    json links = json::array();
    for (const auto& l : s.links) {
        links.push_back({{"role", l.role},
                         {"label", l.label},
                         {"state", to_string(l.state)},
                         {"detail", l.detail}});
    }
    return {{"name", s.name},
            {"type", to_string(s.type)},
            {"state", to_string(s.state)},
            {"links", std::move(links)},
            {"messages_in", s.messages_in},
            {"messages_out", s.messages_out},
            {"last_activity", s.last_activity},
            {"last_problem", s.last_problem},
            {"error", s.error}};
}

std::expected<OrderBook, std::string> load_book(const fs::path& file) {
    std::ifstream in(file);
    if (!in) {
        return OrderBook{};
    }
    const auto j = json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        return std::unexpected(std::format("{} is not valid JSON", file.filename().string()));
    }
    return OrderBook::from_json(j);
}

std::expected<void, std::string> save_book(const OrderBook& book, const fs::path& file) {
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << book.to_json().dump(2) << '\n';
        if (!out) {
            return std::unexpected(std::format("cannot write {}", tmp.string()));
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);  // whole file or the old one, never half
    if (ec) {
        return std::unexpected(std::format("cannot write {}: {}", file.string(), ec.message()));
    }
    return {};
}

// --- one link of a running session -------------------------------------------

class LinkRunner {
public:
    using OnTransfer = std::function<void(const io::Transfer&)>;

    LinkRunner(const asio::any_io_executor& ex, LinkConfig config, io::SessionOptions options,
               std::function<void()> changed)
        : config_{std::move(config)},
          options_{std::move(options)},
          retry_{ex},
          stop_timer_{ex},
          changed_{std::move(changed)} {
        status_.role = config_.role;
        status_.label = config_.label();
        stop_timer_.expires_at(asio::steady_timer::time_point::max());
    }

    void on_transfer(OnTransfer f) { on_transfer_ = std::move(f); }
    void on_warning(std::function<void(const std::string&)> f) { on_warning_ = std::move(f); }

    [[nodiscard]] const LinkStatus& status() const noexcept { return status_; }
    [[nodiscard]] bool connected() const noexcept { return session_ != nullptr; }
    [[nodiscard]] bool done() const noexcept { return done_; }

    // Accepts or connects, runs the link, and does so again after a close.
    asio::awaitable<void> run() {
        const auto endpoint = config_.endpoint();
        if (config_.transport == Transport::tcp_server) {
            try {
                acceptor_.emplace(io::make_acceptor(co_await asio::this_coro::executor,
                                                    std::get<io::Tcp>(endpoint)));
            } catch (const std::exception& e) {
                set(LinkState::failed, e.what());
                done_ = true;
                co_return;
            }
        }
        while (!stopping_) {
            try {
                std::optional<io::Stream> stream;
                // Raced against stop(): a connect to an absent partner may take minutes.
                if (acceptor_) {
                    set(LinkState::waiting, "");
                    auto r = co_await (io::accept(*acceptor_) || until_stopped());
                    if (r.index() == 1) {
                        break;
                    }
                    stream.emplace(std::move(std::get<0>(r)));
                } else {
                    set(LinkState::connecting, "");
                    auto r = co_await (io::connect(endpoint) || until_stopped());
                    if (r.index() == 1) {
                        break;
                    }
                    stream.emplace(std::move(std::get<0>(r)));
                }
                io::SessionHandlers handlers;
                handlers.on_transfer = [this](const io::Transfer& t) {
                    if (on_transfer_) {
                        on_transfer_(t);
                    }
                };
                handlers.on_warning = [this](const std::string& w) {
                    if (on_warning_) {
                        on_warning_(w);
                    }
                };
                session_ = std::make_shared<io::Session>(std::move(*stream), options_, handlers);
                set(LinkState::connected, "");
                const auto keep = session_;  // a sender may still hold it after the close
                co_await keep->run();
                session_.reset();
                set(LinkState::closed, "partner closed the connection");
            } catch (const std::exception& e) {
                session_.reset();
                if (stopping_) {
                    break;
                }
                set(LinkState::closed, e.what());
            }
            if (stopping_) {
                break;
            }
            // Server: accept the next one at once; otherwise try again shortly.
            if (!acceptor_) {
                retry_.expires_after(config_.transport == Transport::tcp_client
                                         ? std::chrono::seconds{5}
                                         : std::chrono::seconds{2});
                (void)co_await retry_.async_wait(use_tuple);
            }
        }
        set(LinkState::closed, "stopped");
        done_ = true;
    }

    // Sends one message; not connected or a failed delivery come back as text.
    asio::awaitable<std::expected<void, std::string>> send(std::vector<std::string> records) {
        const auto s = session_;
        if (!s) {
            co_return std::unexpected(std::format("link {} is not connected", config_.role));
        }
        const auto outcome = co_await s->send(message_of(records));
        if (outcome != lis01::Outcome::delivered) {
            co_return std::unexpected(std::string{lis01::to_string(outcome)});
        }
        co_return std::expected<void, std::string>{};
    }

    void stop() {
        stopping_ = true;
        std::error_code ec;
        if (acceptor_) {
            acceptor_->close(ec);
        }
        if (const auto s = session_) {
            s->stop();
        }
        retry_.cancel();
        stop_timer_.cancel();
    }

private:
    asio::awaitable<void> until_stopped() {
        if (!stopping_) {
            (void)co_await stop_timer_.async_wait(use_tuple);
        }
    }

    void set(LinkState s, std::string detail) {
        status_.state = s;
        status_.detail = std::move(detail);
        changed_();
    }

    LinkConfig config_;
    io::SessionOptions options_;
    asio::steady_timer retry_;
    asio::steady_timer stop_timer_;
    std::function<void()> changed_;
    OnTransfer on_transfer_{};
    std::function<void(const std::string&)> on_warning_{};
    std::optional<asio::ip::tcp::acceptor> acceptor_{};
    std::shared_ptr<io::Session> session_{};
    LinkStatus status_{};
    bool stopping_{false};
    bool done_{false};
};

// --- a running session -----------------------------------------------------------

class Instance {
public:
    Instance(Manager& m, asio::any_io_executor ex, SessionConfig config, lis02::TextOptions text,
             lis01::Timing timing)
        : manager_{m},
          executor_{std::move(ex)},
          config_{std::move(config)},
          text_{text},
          wake_{executor_} {
        status_.name = config_.name;
        status_.type = config_.type;
        status_.state = RunState::running;
        for (const auto& l : config_.links) {
            const auto role =
                config_.type == SessionType::lis ? lis01::Role::computer : lis01::Role::instrument;
            links_.push_back(std::make_unique<LinkRunner>(
                m_executor(), l, io::SessionOptions{.role = role, .timing = timing, .faults = {}},
                [this] { publish_status(); }));
        }
        wake_.expires_at(asio::steady_timer::time_point::max());
    }
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) = delete;
    Instance& operator=(Instance&&) = delete;
    virtual ~Instance() = default;

    asio::awaitable<void> run() {
        for (auto& l : links_) {
            asio::co_spawn(m_executor(), l->run(), asio::detached);
        }
        try {
            co_await work();
        } catch (const std::exception& e) {
            problem(e.what());
        }
        for (auto& l : links_) {
            l->stop();
        }
        // The link coroutines use this object; wait until they are through.
        for (int i = 0;
             i < 300 && !std::ranges::all_of(links_, [](const auto& l) { return l->done(); });
             ++i) {
            co_await sleep_for(milliseconds{10});
        }
        status_.state = RunState::stopped;
        publish_status();
        manager_.finished(config_.name);
    }

    void stop() {
        status_.state = RunState::stopping;
        stopping_ = true;
        for (auto& l : links_) {
            l->stop();
        }
        wake_.cancel();
        publish_status();
    }

    [[nodiscard]] SessionStatus status() const {
        auto s = status_;
        for (const auto& l : links_) {
            s.links.push_back(l->status());
        }
        return s;
    }
    [[nodiscard]] const SessionConfig& config() const noexcept { return config_; }
    [[nodiscard]] json recent_log() const {
        json out = json::array();
        for (const auto& e : log_) {
            out.push_back(e);
        }
        return out;
    }
    [[nodiscard]] virtual json state() const = 0;

protected:
    virtual asio::awaitable<void> work() = 0;

    [[nodiscard]] asio::any_io_executor m_executor() const { return executor_; }

    LinkRunner* link(std::string_view role) {
        for (auto& l : links_) {
            if (l->status().role == role) {
                return l.get();
            }
        }
        return links_.empty() ? nullptr : links_.front().get();
    }

    void log(std::string text, std::vector<std::string> lines = {}) {
        json e = {{"t", clock_text()}, {"text", std::move(text)}, {"lines", std::move(lines)}};
        manager_.publish(config_.name, "log", e);
        log_.push_back(std::move(e));
        while (log_.size() > log_keep) {
            log_.pop_front();
        }
    }
    void problem(std::string text) {
        status_.last_problem = std::format("{} {}", clock_text(), text);
        log("problem: " + text);
        publish_status();
    }
    void activity(bool in) {
        ++(in ? status_.messages_in : status_.messages_out);
        status_.last_activity = clock_text();
        publish_status();
    }
    void publish_status() const { manager_.publish(config_.name, "status", to_json(status())); }

    // Waits until woken (a transfer came, or stop) or the time is up.
    asio::awaitable<void> wait(milliseconds d) {
        asio::steady_timer t{m_executor(), d};
        (void)co_await (wake_.async_wait(use_tuple) || t.async_wait(use_tuple));
        if (!stopping_) {
            wake_.expires_at(asio::steady_timer::time_point::max());
        }
    }
    void wake() { wake_.cancel(); }

    std::expected<lis02::Message, std::string> parse(const io::Transfer& t) {
        std::vector<std::string> records;
        records.reserve(t.records.size());
        for (const auto& r : t.records) {
            records.push_back(lisbench::to_string(r));
        }
        auto m = lis02::parse(records, {.text = text_, .strict = false});
        if (!m) {
            return std::unexpected(
                std::format("record {}: {}", m.error().record + 1, m.error().text));
        }
        return std::move(*m);
    }

    static std::vector<std::string> printable_records(const io::Transfer& t) {
        std::vector<std::string> out;
        out.reserve(t.records.size());
        for (const auto& r : t.records) {
            out.push_back(printable(r));
        }
        return out;
    }

    // A closed hierarchy of two classes in this file; accessors would only add noise.
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
    Manager& manager_;
    asio::any_io_executor executor_;
    SessionConfig config_;
    lis02::TextOptions text_;
    std::vector<std::unique_ptr<LinkRunner>> links_{};
    bool stopping_{false};
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

private:
    asio::steady_timer wake_;
    SessionStatus status_{};
    std::deque<json> log_{};
};

// --- LIS simulator -----------------------------------------------------------------

class LisInstance final : public Instance {
public:
    LisInstance(Manager& m, asio::any_io_executor ex, SessionConfig c, lis02::TextOptions text,
                lis01::Timing timing, OrderBook book)
        : Instance{m, std::move(ex), std::move(c), text, timing}, book_{std::move(book)} {
        for (auto& l : links_) {
            auto* raw = l.get();
            l->on_transfer([this, raw](const io::Transfer& t) { received(*raw, t); });
            l->on_warning([this](const std::string& w) { log("link: " + w); });
        }
    }

    [[nodiscard]] json state() const override { return book_.to_json(); }
    [[nodiscard]] OrderBook& book() noexcept { return book_; }

    void changed() {
        if (auto r = save_book(book_, manager_.state_file(config_.name)); !r) {
            problem(r.error());
        }
        manager_.publish(config_.name, "book", book_.to_json());
    }

    // Sends a message on the orders link; item states change on delivery only.
    asio::awaitable<void> deliver(Outgoing out, std::string what) {
        auto* l = link("orders");
        if (l == nullptr) {
            co_return;
        }
        log(std::format("sending {}", what), out.records);
        const auto r = co_await l->send(out.records);
        if (!r) {
            problem(std::format("{} not delivered: {}", what, r.error()));
            co_return;
        }
        activity(false);
        book_.delivered(out);
        changed();
    }

protected:
    asio::awaitable<void> work() override {
        log(std::format("LIS simulator started ({} specimens in the order book)",
                        book_.specimens().size()));
        while (!stopping_) {
            co_await wait(std::chrono::hours{1});
        }
    }

private:
    void received(LinkRunner& from, const io::Transfer& t) {
        activity(true);
        if (t.records.empty()) {
            log(std::format("{}: empty transfer", from.status().role));
            return;
        }
        log(std::format("{}: received {} records{}", from.status().role, t.records.size(),
                        t.complete ? "" : " (INCOMPLETE: " + t.reason + ")"),
            printable_records(t));
        auto m = parse(t);
        if (!m) {
            problem(m.error());
            return;
        }
        auto h = book_.receive(*m, config_.lis.unsolicited);
        for (const auto& n : h.notes) {
            log(n);
        }
        if (h.changed) {
            changed();
        }
        if (h.query && config_.lis.answer_queries) {
            auto answer = book_.answer(*h.query, h.query_all, config_.lis.no_order_reply);
            asio::co_spawn(m_executor(), deliver(std::move(answer), "query answer"),
                           asio::detached);
        }
    }

    OrderBook book_;
};

// --- instrument simulator --------------------------------------------------------

class DeviceInstance final : public Instance {
public:
    DeviceInstance(Manager& m, asio::any_io_executor ex, const SessionConfig& c,
                   lis02::TextOptions text, lis01::Timing timing, const Catalog& catalog)
        : Instance{m, std::move(ex), c, text, timing}, model_{c.device, catalog, c.name} {
        auto& l = *links_.front();
        l.on_transfer([this](const io::Transfer& t) {
            activity(true);
            log(std::format("received {} records", t.records.size()), printable_records(t));
            inbox_.push_back(t);
            wake();
        });
        l.on_warning([this](const std::string& w) { log("link: " + w); });
    }

    [[nodiscard]] json state() const override {
        json list = json::array();
        for (const auto& s : recent_) {
            list.push_back({{"id", s.id},
                            {"state", to_string(s.state)},
                            {"tests", s.tests},
                            {"values", s.values},
                            {"note", s.note}});
        }
        return {{"specimens", std::move(list)}};
    }

protected:
    asio::awaitable<void> work() override {
        auto& l = *links_.front();
        log(std::format(
            "instrument simulator started, mode {}",
            config_.device.mode == DeviceMode::unsolicited ? "unsolicited" : "host query"));
        while (!stopping_) {
            if (!l.connected()) {
                co_await wait(milliseconds{200});
                continue;
            }
            DeviceSpecimen s;
            if (!model_.next(s)) {
                log("all specimens of the list are done");
                while (!stopping_) {
                    co_await wait(std::chrono::hours{1});
                }
                break;
            }
            recent_.push_back(s);
            co_await work_on(&l);
            co_await wait(config_.device.interval);
        }
    }

private:
    DeviceSpecimen& current() { return recent_.back(); }

    void update(SpecimenState st, std::string note = {}) {
        current().state = st;
        current().note = std::move(note);
        while (recent_.size() > specimens_keep) {
            recent_.pop_front();
        }
        manager_.publish(config_.name, "specimens", state());
    }

    asio::awaitable<void> work_on(LinkRunner* link) {
        auto& l = *link;
        auto& s = current();
        update(SpecimenState::queued);
        if (config_.device.mode == DeviceMode::host_query) {
            inbox_.clear();
            const auto q = model_.query(s);
            log(std::format("{}: query", s.id), q);
            if (const auto r = co_await l.send(q); !r) {
                update(SpecimenState::failed, "query not delivered: " + r.error());
                co_return;
            }
            activity(false);
            update(SpecimenState::queried);
            const auto until = std::chrono::steady_clock::now() + config_.device.query_timeout;
            std::optional<lis02::Message> answer;
            while (!stopping_ && !answer && std::chrono::steady_clock::now() < until) {
                if (inbox_.empty()) {
                    co_await wait(std::chrono::duration_cast<milliseconds>(
                        until - std::chrono::steady_clock::now()));
                    continue;
                }
                auto t = std::move(inbox_.front());
                inbox_.pop_front();
                if (auto m = parse(t)) {
                    answer = std::move(*m);
                } else {
                    problem(m.error());
                }
            }
            if (!answer) {
                update(SpecimenState::timed_out, "no answer to the query");
                co_return;
            }
            current().tests = DeviceModel::ordered_tests(*answer, current().id);
            if (current().tests.empty()) {
                update(SpecimenState::no_order, "the LIS has no order");
                co_return;
            }
            update(SpecimenState::ordered);
        }
        update(SpecimenState::measuring);
        co_await wait(config_.device.measure_time);
        if (stopping_) {
            co_return;
        }
        const auto results = model_.results(current());
        log(std::format("{}: results", current().id), results);
        if (const auto r = co_await l.send(results); !r) {
            update(SpecimenState::failed, "results not delivered: " + r.error());
            co_return;
        }
        activity(false);
        update(SpecimenState::reported);
    }

    DeviceModel model_;
    std::deque<DeviceSpecimen> recent_{};
    std::deque<io::Transfer> inbox_{};
};

// --- manager -------------------------------------------------------------------------

Manager::Manager(asio::any_io_executor executor, ManagerOptions options, EventSink sink)
    : executor_{std::move(executor)}, options_{std::move(options)}, sink_{std::move(sink)} {
    std::error_code ec;
    fs::create_directories(session_dir(), ec);
    if (const auto c = load_catalog(options_.data_dir / "catalog.yaml")) {
        catalog_ = *c;
    }
}

Manager::~Manager() = default;

fs::path Manager::session_dir() const {
    return options_.data_dir / "sessions";
}

fs::path Manager::session_file(std::string_view name) const {
    return session_dir() / (std::string{name} + ".yaml");
}

fs::path Manager::state_file(std::string_view name) const {
    return session_dir() / (std::string{name} + ".state.json");
}

void Manager::publish(std::string_view session, std::string_view kind, const json& data) const {
    if (sink_) {
        sink_(session, kind, data);
    }
}

std::vector<SessionStatus> Manager::list() const {
    std::vector<SessionStatus> out;
    std::error_code ec;
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator{session_dir(), ec}) {
        const auto file = e.path().filename().string();
        if (e.is_regular_file() && file.ends_with(".yaml") && !file.ends_with(".state.yaml")) {
            names.insert(file.substr(0, file.size() - 5));
        }
    }
    for (const auto& [name, inst] : instances_) {
        names.insert(name);
    }
    for (const auto& name : names) {
        if (const auto it = instances_.find(name); it != instances_.end()) {
            out.push_back(it->second->status());
            continue;
        }
        SessionStatus s{.name = name};
        if (const auto c = config(name)) {
            s.type = c->type;
            for (const auto& l : c->links) {
                s.links.push_back({.role = l.role, .label = l.label(), .state = LinkState::closed});
            }
        } else {
            s.state = RunState::failed;
            s.error = c.error();
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::expected<SessionConfig, std::string> Manager::config(std::string_view name) const {
    if (!valid_session_name(name)) {
        return std::unexpected(std::format("'{}' is not a valid session name", name));
    }
    std::error_code ec;
    if (!fs::exists(session_file(name), ec)) {
        return std::unexpected(std::format("no session {}", name));
    }
    return load_session(session_file(name));
}

std::expected<void, std::string> Manager::save(const SessionConfig& c) {
    if (!valid_session_name(c.name)) {
        return std::unexpected(std::format("'{}' is not a valid session name", c.name));
    }
    if (running(c.name)) {
        return std::unexpected(std::format("{} is running; stop it before changing it", c.name));
    }
    const auto text = to_yaml(c);
    if (auto check = parse_session(text); !check) {
        return std::unexpected(check.error());  // the file must read back as written
    }
    const auto file = session_file(c.name);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << text;
        if (!out) {
            return std::unexpected(std::format("cannot write {}", tmp.string()));
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        return std::unexpected(ec.message());
    }
    return {};
}

std::expected<void, std::string> Manager::remove(std::string_view name) {
    if (!valid_session_name(name)) {
        return std::unexpected(std::format("'{}' is not a valid session name", name));
    }
    if (running(name)) {
        return std::unexpected(std::format("{} is running", name));
    }
    std::error_code ec;
    if (!fs::remove(session_file(name), ec)) {
        return std::unexpected(std::format("no session {}", name));
    }
    fs::remove(state_file(name), ec);
    return {};
}

std::expected<void, std::string> Manager::save_catalog(Catalog c) {
    const auto text = to_yaml(c);
    if (auto check = parse_catalog(text); !check) {
        return std::unexpected(check.error());
    }
    const auto file = options_.data_dir / "catalog.yaml";
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp);
        out << text;
        if (!out) {
            return std::unexpected(std::format("cannot write {}", tmp.string()));
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        return std::unexpected(ec.message());
    }
    catalog_ = std::move(c);
    return {};
}

bool Manager::running(std::string_view name) const {
    return instances_.contains(name);
}

std::expected<void, std::string> Manager::start(std::string_view name) {
    if (running(name)) {
        return std::unexpected(std::format("{} is running already", name));
    }
    auto c = config(name);
    if (!c) {
        return std::unexpected(c.error());
    }
    // Two sessions cannot share a serial port or a listening TCP port.
    for (const auto& [other, inst] : instances_) {
        for (const auto& mine : c->links) {
            for (const auto& theirs : inst->config().links) {
                const auto r = resource_of(mine);
                if (!r.empty() && r == resource_of(theirs)) {
                    return std::unexpected(std::format("{} is used by session {}", r, other));
                }
            }
        }
    }
    lis02::TextOptions text;
    auto timing = c->fast_timing ? lis01::fast_test_timing() : lis01::Timing{};
    if (!c->profile.empty()) {
        const auto p = sim::load_profile(c->profile, options_.profile_search);
        if (!p) {
            return std::unexpected(p.error());
        }
        text = p->text;
        sim::apply(*p, timing);
    }
    std::unique_ptr<Instance> inst;
    if (c->type == SessionType::lis) {
        auto book = load_book(state_file(name));
        if (!book) {
            return std::unexpected(book.error());
        }
        inst = std::make_unique<LisInstance>(*this, executor_, *c, text, timing, std::move(*book));
    } else {
        inst = std::make_unique<DeviceInstance>(*this, executor_, *c, text, timing, catalog_);
    }
    auto* raw = inst.get();
    instances_.emplace(std::string{name}, std::move(inst));
    asio::co_spawn(executor_, raw->run(), asio::detached);
    publish(name, "status", to_json(raw->status()));
    return {};
}

void Manager::stop(std::string_view name) {
    if (const auto it = instances_.find(name); it != instances_.end()) {
        it->second->stop();
    }
}

void Manager::stop_all() {
    for (const auto& [name, inst] : instances_) {
        inst->stop();
    }
}

void Manager::finished(std::string_view name) {
    // After the instance's coroutine has returned, not inside it.
    asio::post(executor_, [this, n = std::string{name}] {
        instances_.erase(n);
        if (const auto c = config(n)) {
            SessionStatus s{.name = n, .type = c->type, .state = RunState::stopped};
            for (const auto& l : c->links) {
                s.links.push_back({.role = l.role, .label = l.label(), .state = LinkState::closed});
            }
            publish(n, "status", to_json(s));
        }
    });
}

std::expected<json, std::string> Manager::details(std::string_view name) const {
    auto c = config(name);
    if (!c) {
        return std::unexpected(c.error());
    }
    json j = {{"config", config_json(*c)}, {"config_yaml", to_yaml(*c)}};
    if (const auto it = instances_.find(name); it != instances_.end()) {
        j["status"] = to_json(it->second->status());
        j["log"] = it->second->recent_log();
        j["state"] = it->second->state();
    } else {
        SessionStatus s{.name = std::string{name}, .type = c->type};
        for (const auto& l : c->links) {
            s.links.push_back({.role = l.role, .label = l.label(), .state = LinkState::closed});
        }
        j["status"] = to_json(s);
        j["log"] = json::array();
        if (c->type == SessionType::lis) {
            auto book = load_book(state_file(name));
            j["state"] = book ? book->to_json() : json::object();
        } else {
            j["state"] = json{{"specimens", json::array()}};
        }
    }
    return j;
}

std::expected<void, std::string> Manager::edit_book(std::string_view name, const BookEdit& edit) {
    if (const auto it = instances_.find(name); it != instances_.end()) {
        auto* lis = dynamic_cast<LisInstance*>(it->second.get());
        if (lis == nullptr) {
            return std::unexpected(std::format("{} is not a LIS session", name));
        }
        if (auto r = edit(lis->book()); !r) {
            return r;
        }
        lis->changed();
        return {};
    }
    const auto c = config(name);
    if (!c) {
        return std::unexpected(c.error());
    }
    if (c->type != SessionType::lis) {
        return std::unexpected(std::format("{} is not a LIS session", name));
    }
    auto book = load_book(state_file(name));
    if (!book) {
        return std::unexpected(book.error());
    }
    if (auto r = edit(*book); !r) {
        return r;
    }
    if (auto r = save_book(*book, state_file(name)); !r) {
        return r;
    }
    publish(name, "book", book->to_json());
    return {};
}

std::expected<void, std::string> Manager::send_orders(std::string_view name,
                                                      const std::vector<std::string>& specimens) {
    const auto it = instances_.find(name);
    auto* lis = it == instances_.end() ? nullptr : dynamic_cast<LisInstance*>(it->second.get());
    if (lis == nullptr) {
        return std::unexpected(std::format("start LIS session {} first", name));
    }
    auto out = lis->book().orders(specimens);
    if (!out) {
        return std::unexpected("nothing new to send");
    }
    asio::co_spawn(executor_, lis->deliver(std::move(*out), "orders"), asio::detached);
    return {};
}

std::expected<void, std::string> Manager::cancel_item(std::string_view name,
                                                      const std::string& specimen,
                                                      const std::string& test) {
    const auto it = instances_.find(name);
    auto* lis = it == instances_.end() ? nullptr : dynamic_cast<LisInstance*>(it->second.get());
    if (lis == nullptr) {
        return std::unexpected(std::format("start LIS session {} first", name));
    }
    auto out = lis->book().cancel(specimen, test);
    if (!out) {
        return std::unexpected(out.error());
    }
    asio::co_spawn(executor_, lis->deliver(std::move(*out), "cancellation"), asio::detached);
    return {};
}

}  // namespace lisbench::session

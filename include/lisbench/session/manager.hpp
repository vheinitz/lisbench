#pragma once

// Runs sessions (docs/spec/features.md, 9.2): session files and the shared
// catalogue in the data directory, several instances side by side on one
// event loop, link states, state files, and events for the web UI.

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/session/catalog.hpp"
#include "lisbench/session/config.hpp"
#include "lisbench/session/order_book.hpp"

namespace lisbench::session {

enum class RunState : std::uint8_t { stopped, running, stopping, failed };
enum class LinkState : std::uint8_t { waiting, connecting, connected, closed, failed };

[[nodiscard]] std::string_view to_string(RunState s) noexcept;
[[nodiscard]] std::string_view to_string(LinkState s) noexcept;

struct LinkStatus {
    std::string role{};
    std::string label{};
    LinkState state{LinkState::closed};
    std::string detail{};
};

struct SessionStatus {
    std::string name{};
    SessionType type{SessionType::lis};
    RunState state{RunState::stopped};
    std::vector<LinkStatus> links{};
    std::size_t messages_in{0};
    std::size_t messages_out{0};
    std::string last_activity{};
    std::string last_problem{};
    std::string error{};  // the file could not be read, or the start failed
};

[[nodiscard]] nlohmann::json to_json(const SessionStatus& s);

// Events: kind "status" (SessionStatus), "log" ({t, text, lines}), "book"
// (the LIS order book), "specimens" (the instrument's recent specimens).
using EventSink = std::function<void(std::string_view session, std::string_view kind,
                                     const nlohmann::json& data)>;

struct ManagerOptions {
    std::filesystem::path data_dir{};  // holds sessions/ and catalog.yaml
    std::vector<std::filesystem::path> profile_search{};
};

class Instance;

class Manager {
public:
    Manager(asio::any_io_executor executor, ManagerOptions options, EventSink sink);
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;
    ~Manager();

    // Session files.
    [[nodiscard]] std::vector<SessionStatus> list() const;
    [[nodiscard]] std::expected<SessionConfig, std::string> config(std::string_view name) const;
    std::expected<void, std::string> save(const SessionConfig& c);
    std::expected<void, std::string> remove(std::string_view name);

    // The shared catalogue (an empty one when there is no file yet).
    [[nodiscard]] const Catalog& catalog() const noexcept { return catalog_; }
    std::expected<void, std::string> save_catalog(Catalog c);

    // Instances.
    std::expected<void, std::string> start(std::string_view name);
    void stop(std::string_view name);
    void stop_all();
    [[nodiscard]] bool running(std::string_view name) const;
    // Config, status, recent log and the type's state (order book / specimens).
    [[nodiscard]] std::expected<nlohmann::json, std::string> details(std::string_view name) const;

    // LIS sessions: the order book, also while stopped (from the state file).
    using BookEdit = std::function<std::expected<void, std::string>(OrderBook&)>;
    std::expected<void, std::string> edit_book(std::string_view name, const BookEdit& edit);
    std::expected<void, std::string> send_orders(std::string_view name,
                                                 const std::vector<std::string>& specimens);
    std::expected<void, std::string> cancel_item(std::string_view name, const std::string& specimen,
                                                 const std::string& test);

    // Called by instances.
    void publish(std::string_view session, std::string_view kind, const nlohmann::json& data) const;
    [[nodiscard]] const ManagerOptions& options() const noexcept { return options_; }
    [[nodiscard]] std::filesystem::path state_file(std::string_view name) const;
    void finished(std::string_view name);

private:
    [[nodiscard]] std::filesystem::path session_dir() const;
    [[nodiscard]] std::filesystem::path session_file(std::string_view name) const;

    asio::any_io_executor executor_;
    ManagerOptions options_;
    EventSink sink_;
    Catalog catalog_{};
    // No default member initializer: it would need the complete Instance here.
    std::map<std::string, std::unique_ptr<Instance>, std::less<>> instances_;
};

// Order book of a LIS session as stored (empty if there is no state file).
[[nodiscard]] std::expected<OrderBook, std::string> load_book(const std::filesystem::path& file);
std::expected<void, std::string> save_book(const OrderBook& book,
                                           const std::filesystem::path& file);

}  // namespace lisbench::session

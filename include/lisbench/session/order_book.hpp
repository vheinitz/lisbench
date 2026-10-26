#pragma once

// The LIS simulator's order book (docs/spec/features.md, 9.5): specimens with
// their order items, the messages that send or cancel them, query answers,
// and order-result matching of what comes back. Pure: it builds and reads
// messages; links and timers belong to the session runtime.

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/lis02_message.hpp"
#include "lisbench/session/config.hpp"

namespace lisbench::session {

// Named after the LIS02-A2 codes where one exists (O-12 action code, R-9
// result status). sent/answered: out to the instrument, no result yet.
enum class ItemState : std::uint8_t {
    created,
    sent,
    answered,
    preliminary,
    final_result,
    corrected,
    cannot_be_done,
    cancelled,
};

[[nodiscard]] std::string_view to_string(ItemState s) noexcept;

struct ResultData {
    std::string value{};
    std::string unit{};
    std::string flags{};   // R-7
    std::string status{};  // R-9
    std::string operator_id{};
    std::string completed_at{};  // R-13 as sent
    friend bool operator==(const ResultData&, const ResultData&) = default;
};

struct OrderItem {
    std::string test{};
    ItemState state{ItemState::created};
    bool unsolicited{false};  // came as a result without an order
    bool review{false};       // unsolicited with policy flag: for the user to accept or discard
    std::optional<ResultData> result{};
    friend bool operator==(const OrderItem&, const OrderItem&) = default;
};

struct Specimen {
    std::string id{};
    std::string patient_id{};
    std::string patient_name{};
    char priority{'R'};  // O-6: S stat, A asap, R routine
    std::vector<OrderItem> items{};
    friend bool operator==(const Specimen&, const Specimen&) = default;
};

// What a received message did, for the log.
struct Handled {
    std::vector<std::string> notes{};
    bool changed{false};
    // A query to answer: the specimen IDs asked for, or empty with `all`.
    std::optional<std::vector<std::string>> query{};
    bool query_all{false};
};

// Records (wire text) of a message and what it carries, so that the item
// states change only after the message was delivered.
struct Outgoing {
    std::vector<std::string> records{};
    std::vector<std::pair<std::string, std::string>> items{};  // specimen, test
    ItemState on_delivery{ItemState::sent};
};

class OrderBook {
public:
    [[nodiscard]] const std::vector<Specimen>& specimens() const noexcept { return specimens_; }
    [[nodiscard]] const Specimen* find(std::string_view id) const noexcept;

    // Editing (from the UI). Errors say why.
    std::expected<void, std::string> add_specimen(Specimen s);
    std::expected<void, std::string> remove_specimen(std::string_view id);
    std::expected<void, std::string> add_test(std::string_view specimen, std::string test);
    std::expected<void, std::string> remove_test(std::string_view specimen, std::string_view test);
    void clear() noexcept { specimens_.clear(); }

    // Orders for these specimens' items in state created (all specimens if
    // the list is empty); nullopt when there is nothing to send.
    [[nodiscard]] std::optional<Outgoing> orders(
        const std::vector<std::string>& specimen_ids) const;
    // Cancellation (O-12 = C) of one item that went out.
    [[nodiscard]] std::expected<Outgoing, std::string> cancel(std::string_view specimen,
                                                              std::string_view test) const;
    // Answer to a query: orders for the specimens asked for, or "no order".
    [[nodiscard]] Outgoing answer(const std::vector<std::string>& specimen_ids, bool all,
                                  NoOrderReply no_order) const;
    void delivered(const Outgoing& o);

    // A received message: queries to answer, results to match. Specimens
    // the instrument names that are not in the book yet are added (a query:
    // without items; a result: as the policy says), so the book can be built
    // by listening and then edited into a scenario.
    Handled receive(const lis02::Message& m, Unsolicited policy);

    // An item kept for review: accept it as an unsolicited result.
    std::expected<void, std::string> accept_item(std::string_view specimen, std::string_view test);

    [[nodiscard]] nlohmann::json to_json() const;
    [[nodiscard]] static std::expected<OrderBook, std::string> from_json(const nlohmann::json& j);

    friend bool operator==(const OrderBook&, const OrderBook&) = default;

private:
    Specimen* find_mutable(std::string_view id) noexcept;
    void record_result(const std::string& specimen, const std::string& test, ResultData r,
                       Unsolicited policy, Handled& h);

    Specimen& specimen_for(const std::string& id, Handled& h);

    std::vector<Specimen> specimens_{};
};

// The header every lisbench simulator sends.
[[nodiscard]] std::string header_record(std::string_view sender, std::string_view receiver);

}  // namespace lisbench::session

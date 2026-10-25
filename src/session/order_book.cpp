#include "lisbench/session/order_book.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <stdexcept>

namespace lisbench::session {

using lis02::RecordType;
using nlohmann::json;

namespace {

constexpr std::array state_names{"new",   "sent",      "answered",       "preliminary",
                                 "final", "corrected", "cannot-be-done", "cancelled"};

void put(lis02::Record& r, RecordType t, std::string_view name, lis02::Field value) {
    r.set(lis02::field_number_or_throw(t, name), std::move(value));
}

const std::string& get(const lis02::Record& r, RecordType t, std::string_view name,
                       std::size_t comp = 0) {
    return r.component(lis02::field_number_or_throw(t, name), 0, comp);
}

std::string now_stamp() {
    const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y%m%d%H%M%S}", t);
}

lis02::Builder start(std::string_view receiver) {
    lis02::Builder b;
    auto& h = b.add(RecordType::header).value().get();
    put(h, RecordType::header, "sender_name", lis02::make_field({"lisbench-lis"}));
    put(h, RecordType::header, "receiver_id", lis02::make_field(std::string{receiver}));
    put(h, RecordType::header, "processing_id", lis02::make_field("P"));
    put(h, RecordType::header, "version", lis02::make_field("LIS2-A2"));
    put(h, RecordType::header, "timestamp", lis02::make_field(now_stamp()));
    return b;
}

std::vector<std::string> wire(lis02::Builder& b) {
    auto lines = lis02::serialize(b.finish());
    if (!lines) {
        throw std::logic_error(lines.error().text);
    }
    return std::move(*lines);
}

void add_patient(lis02::Builder& b, const Specimen& s) {
    auto& p = b.add(RecordType::patient).value().get();
    put(p, RecordType::patient, "practice_patient_id", lis02::make_field(s.patient_id));
    if (!s.patient_name.empty()) {
        put(p, RecordType::patient, "patient_name", lis02::make_field(s.patient_name));
    }
}

void add_order(lis02::Builder& b, const Specimen& s, std::string_view test, char action,
               char report) {
    auto& o = b.add(RecordType::order).value().get();
    put(o, RecordType::order, "specimen_id", lis02::make_field(s.id));
    put(o, RecordType::order, "universal_test_id",
        lis02::make_field({"", "", "", std::string{test}}));
    put(o, RecordType::order, "priority", lis02::make_field(std::string(1, s.priority)));
    put(o, RecordType::order, "action_code", lis02::make_field(std::string(1, action)));
    put(o, RecordType::order, "report_type", lis02::make_field(std::string(1, report)));
}

ItemState state_for(std::string_view status, ItemState current) {
    if (status == "F") {
        return current == ItemState::final_result || current == ItemState::corrected
                   ? ItemState::corrected
                   : ItemState::final_result;
    }
    if (status == "C") {
        return ItemState::corrected;
    }
    if (status == "X") {
        return ItemState::cannot_be_done;
    }
    return ItemState::preliminary;  // P, I, S and anything else not final
}

json result_json(const ResultData& r) {
    return {{"value", r.value},          {"unit", r.unit},
            {"flags", r.flags},          {"status", r.status},
            {"operator", r.operator_id}, {"completed_at", r.completed_at}};
}

ResultData result_of(const json& j) {
    return {.value = j.value("value", ""),
            .unit = j.value("unit", ""),
            .flags = j.value("flags", ""),
            .status = j.value("status", ""),
            .operator_id = j.value("operator", ""),
            .completed_at = j.value("completed_at", "")};
}

}  // namespace

std::string_view to_string(ItemState s) noexcept {
    return state_names.at(static_cast<std::size_t>(s));
}

std::string header_record(std::string_view sender, std::string_view receiver) {
    lis02::Builder b;
    auto& h = b.add(RecordType::header).value().get();
    put(h, RecordType::header, "sender_name", lis02::make_field({std::string{sender}}));
    put(h, RecordType::header, "receiver_id", lis02::make_field(std::string{receiver}));
    put(h, RecordType::header, "processing_id", lis02::make_field("P"));
    put(h, RecordType::header, "version", lis02::make_field("LIS2-A2"));
    put(h, RecordType::header, "timestamp", lis02::make_field(now_stamp()));
    return wire(b).front();
}

const Specimen* OrderBook::find(std::string_view id) const noexcept {
    const auto it = std::ranges::find(specimens_, id, &Specimen::id);
    return it == specimens_.end() ? nullptr : &*it;
}

Specimen* OrderBook::find_mutable(std::string_view id) noexcept {
    const auto it = std::ranges::find(specimens_, id, &Specimen::id);
    return it == specimens_.end() ? nullptr : &*it;
}

std::expected<void, std::string> OrderBook::add_specimen(Specimen s) {
    if (s.id.empty()) {
        return std::unexpected("a specimen needs an ID");
    }
    if (find(s.id) != nullptr) {
        return std::unexpected(std::format("specimen {} exists", s.id));
    }
    if (s.priority != 'R' && s.priority != 'S' && s.priority != 'A') {
        return std::unexpected("priority is R (routine), S (stat) or A (asap)");
    }
    specimens_.push_back(std::move(s));
    return {};
}

std::expected<void, std::string> OrderBook::remove_specimen(std::string_view id) {
    const auto n = std::erase_if(specimens_, [&](const Specimen& s) { return s.id == id; });
    if (n == 0) {
        return std::unexpected(std::format("no specimen {}", id));
    }
    return {};
}

std::expected<void, std::string> OrderBook::add_test(std::string_view specimen, std::string test) {
    auto* s = find_mutable(specimen);
    if (s == nullptr) {
        return std::unexpected(std::format("no specimen {}", specimen));
    }
    if (std::ranges::find(s->items, test, &OrderItem::test) != s->items.end()) {
        return std::unexpected(std::format("{} has {} already", specimen, test));
    }
    s->items.push_back({.test = std::move(test)});
    return {};
}

std::optional<Outgoing> OrderBook::orders(const std::vector<std::string>& specimen_ids) const {
    auto b = start("instrument");
    Outgoing out;
    for (const auto& s : specimens_) {
        if (!specimen_ids.empty() && std::ranges::find(specimen_ids, s.id) == specimen_ids.end()) {
            continue;
        }
        bool patient = false;
        for (const auto& i : s.items) {
            if (i.state != ItemState::created) {
                continue;
            }
            if (!patient) {
                add_patient(b, s);
                patient = true;
            }
            add_order(b, s, i.test, 'N', 'O');
            out.items.emplace_back(s.id, i.test);
        }
    }
    if (out.items.empty()) {
        return std::nullopt;
    }
    out.records = wire(b);
    out.on_delivery = ItemState::sent;
    return out;
}

std::expected<Outgoing, std::string> OrderBook::cancel(std::string_view specimen,
                                                       std::string_view test) const {
    const auto* s = find(specimen);
    if (s == nullptr) {
        return std::unexpected(std::format("no specimen {}", specimen));
    }
    const auto it = std::ranges::find(s->items, test, &OrderItem::test);
    if (it == s->items.end()) {
        return std::unexpected(std::format("{} has no {}", specimen, test));
    }
    if (it->state != ItemState::sent && it->state != ItemState::answered) {
        return std::unexpected(std::format(
            "{} {} is {}: only an item that went out and has no result can be cancelled", specimen,
            test, to_string(it->state)));
    }
    auto b = start("instrument");
    add_patient(b, *s);
    add_order(b, *s, test, 'C', 'O');
    return Outgoing{.records = wire(b),
                    .items = {{std::string{specimen}, std::string{test}}},
                    .on_delivery = ItemState::cancelled};
}

Outgoing OrderBook::answer(const std::vector<std::string>& specimen_ids, bool all,
                           NoOrderReply no_order) const {
    auto b = start("instrument");
    Outgoing out{.on_delivery = ItemState::answered};
    std::vector<std::string> asked = specimen_ids;
    if (all) {
        asked.clear();
        for (const auto& s : specimens_) {
            asked.push_back(s.id);
        }
    }
    for (const auto& id : asked) {
        const auto* s = find(id);
        bool any = false;
        if (s != nullptr) {
            for (const auto& i : s->items) {
                if (i.state != ItemState::created) {
                    continue;
                }
                if (!any) {
                    add_patient(b, *s);
                    any = true;
                }
                // Report type Q: response to a query.
                add_order(b, *s, i.test, 'N', 'Q');
                out.items.emplace_back(s->id, i.test);
            }
        }
        if (!any && !all && no_order == NoOrderReply::y_record) {
            // O-26 = Y: no order on record for this specimen.
            Specimen unknown{.id = id};
            add_patient(b, unknown);
            add_order(b, unknown, "", 'N', 'Y');
        }
    }
    out.records = wire(b);
    return out;
}

void OrderBook::delivered(const Outgoing& o) {
    for (const auto& [specimen, test] : o.items) {
        if (auto* s = find_mutable(specimen)) {
            if (auto it = std::ranges::find(s->items, test, &OrderItem::test);
                it != s->items.end()) {
                it->state = o.on_delivery;
            }
        }
    }
}

void OrderBook::record_result(const std::string& specimen, const std::string& test, ResultData r,
                              Unsolicited policy, Handled& h) {
    auto* s = find_mutable(specimen);
    OrderItem* item = nullptr;
    if (s != nullptr) {
        if (auto it = std::ranges::find(s->items, test, &OrderItem::test); it != s->items.end()) {
            item = &*it;
        }
    }
    const bool ordered =
        item != nullptr && item->state != ItemState::created && item->state != ItemState::cancelled;
    if (ordered || (item != nullptr && item->unsolicited)) {
        item->state = state_for(r.status, item->state);
        h.notes.push_back(std::format("{} {}: {} {} ({})", specimen, test, r.value, r.unit,
                                      to_string(item->state)));
        item->result = std::move(r);
        h.changed = true;
        return;
    }
    const auto why =
        item == nullptr ? "no order" : std::format("order is {}", to_string(item->state));
    switch (policy) {
        case Unsolicited::accept: {
            if (s == nullptr) {
                specimens_.push_back({.id = specimen});
                s = &specimens_.back();
            }
            if (item == nullptr) {
                s->items.push_back({.test = test});
                item = &s->items.back();
            }
            item->unsolicited = true;
            item->state = state_for(r.status, ItemState::created);
            item->result = std::move(r);
            h.notes.push_back(
                std::format("{} {}: unsolicited ({}), accepted", specimen, test, why));
            h.changed = true;
            return;
        }
        case Unsolicited::flag:
            unmatched_.push_back({.specimen = specimen, .test = test, .result = std::move(r)});
            h.notes.push_back(
                std::format("{} {}: unsolicited ({}), kept for review", specimen, test, why));
            h.changed = true;
            return;
        case Unsolicited::ignore:
            h.notes.push_back(std::format("{} {}: unsolicited ({}), ignored", specimen, test, why));
            return;
    }
}

Handled OrderBook::receive(const lis02::Message& m, Unsolicited policy) {
    Handled h;
    std::string specimen;
    for (const auto& r : m.records) {
        switch (r.kind().value_or(RecordType::comment)) {
            case RecordType::query: {
                // Q-3 starting range: ALL, or ^specimen^... (component 2), or the ID itself.
                const auto& c1 = get(r, RecordType::query, "starting_range_id", 0);
                const auto& c2 = get(r, RecordType::query, "starting_range_id", 1);
                if (c1 == "ALL" || c2 == "ALL") {
                    h.query_all = true;
                    h.query.emplace();
                } else {
                    const auto& id = c2.empty() ? c1 : c2;
                    if (!h.query) {
                        h.query.emplace();
                    }
                    if (!id.empty()) {
                        h.query->push_back(id);
                    }
                }
                const auto& asked = c2.empty() ? c1 : c2;
                h.notes.push_back(std::format("query for {}", h.query_all ? "all" : asked));
                break;
            }
            case RecordType::order: specimen = get(r, RecordType::order, "specimen_id"); break;
            case RecordType::result: {
                ResultData d{.value = get(r, RecordType::result, "value"),
                             .unit = get(r, RecordType::result, "units"),
                             .flags = get(r, RecordType::result, "abnormal_flags"),
                             .status = get(r, RecordType::result, "result_status"),
                             .operator_id = get(r, RecordType::result, "operator_id"),
                             .completed_at = get(r, RecordType::result, "completed_at")};
                const auto& test = get(r, RecordType::result, "universal_test_id", 3);
                if (specimen.empty()) {
                    h.notes.push_back(
                        std::format("result for {} without an O record before it", test));
                    break;
                }
                record_result(specimen, test, std::move(d), policy, h);
                break;
            }
            default: break;
        }
    }
    return h;
}

std::expected<void, std::string> OrderBook::accept_unmatched(std::size_t index) {
    if (index >= unmatched_.size()) {
        return std::unexpected("no such unmatched result");
    }
    auto u = std::move(unmatched_[index]);
    unmatched_.erase(unmatched_.begin() + static_cast<std::ptrdiff_t>(index));
    Handled h;
    record_result(u.specimen, u.test, std::move(u.result), Unsolicited::accept, h);
    return {};
}

std::expected<void, std::string> OrderBook::discard_unmatched(std::size_t index) {
    if (index >= unmatched_.size()) {
        return std::unexpected("no such unmatched result");
    }
    unmatched_.erase(unmatched_.begin() + static_cast<std::ptrdiff_t>(index));
    return {};
}

json OrderBook::to_json() const {
    json specimens = json::array();
    for (const auto& s : specimens_) {
        json items = json::array();
        for (const auto& i : s.items) {
            json item = {
                {"test", i.test}, {"state", to_string(i.state)}, {"unsolicited", i.unsolicited}};
            if (i.result) {
                item["result"] = result_json(*i.result);
            }
            items.push_back(std::move(item));
        }
        specimens.push_back({{"id", s.id},
                             {"patient_id", s.patient_id},
                             {"patient_name", s.patient_name},
                             {"priority", std::string(1, s.priority)},
                             {"items", std::move(items)}});
    }
    json unmatched = json::array();
    for (const auto& u : unmatched_) {
        unmatched.push_back(
            {{"specimen", u.specimen}, {"test", u.test}, {"result", result_json(u.result)}});
    }
    return {{"specimens", std::move(specimens)}, {"unmatched", std::move(unmatched)}};
}

std::expected<OrderBook, std::string> OrderBook::from_json(const json& j) {
    try {
        OrderBook b;
        for (const auto& s : j.at("specimens")) {
            Specimen sp{.id = s.at("id"),
                        .patient_id = s.value("patient_id", ""),
                        .patient_name = s.value("patient_name", ""),
                        .priority = s.value("priority", std::string{"R"}).front()};
            for (const auto& i : s.at("items")) {
                OrderItem item{.test = i.at("test"), .unsolicited = i.value("unsolicited", false)};
                const auto state = i.at("state").get<std::string>();
                const auto it = std::ranges::find(state_names, state);
                if (it == state_names.end()) {
                    return std::unexpected(std::format("unknown item state '{}'", state));
                }
                item.state = static_cast<ItemState>(it - state_names.begin());
                if (i.contains("result")) {
                    item.result = result_of(i["result"]);
                }
                sp.items.push_back(std::move(item));
            }
            b.specimens_.push_back(std::move(sp));
        }
        for (const auto& u : j.value("unmatched", json::array())) {
            b.unmatched_.push_back({.specimen = u.at("specimen"),
                                    .test = u.at("test"),
                                    .result = result_of(u.at("result"))});
        }
        return b;
    } catch (const json::exception& e) {
        return std::unexpected(std::format("state file: {}", e.what()));
    }
}

}  // namespace lisbench::session

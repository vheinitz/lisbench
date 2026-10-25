// Sessions without I/O: catalogue and session files, the LIS order book with
// its item states and order-result matching, and the instrument model.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "lisbench/session/catalog.hpp"
#include "lisbench/session/config.hpp"
#include "lisbench/session/device_model.hpp"
#include "lisbench/session/order_book.hpp"

using namespace lisbench;
using namespace lisbench::session;
using namespace std::chrono_literals;

namespace {

const char* const catalog_yaml = R"(
lisbench: catalog/1
tests:
  - {code: GLU, name: Glucose, unit: mg/dL, reference: {low: 70, high: 110}, simulate: {low: 60, high: 180}}
  - {code: FLUA, name: Influenza A, kind: qualitative, values: [DETECTED, NOT DETECTED], abnormal: [DETECTED]}
)";

Catalog catalog() {
    return parse_catalog(catalog_yaml).value();
}

lis02::Message message(const std::vector<std::string>& records) {
    return lis02::parse(records).value();
}

std::string state_of(const OrderBook& b, std::string_view specimen, std::string_view test) {
    const auto* s = b.find(specimen);
    REQUIRE(s != nullptr);
    const auto it = std::ranges::find(s->items, test, &OrderItem::test);
    REQUIRE(it != s->items.end());
    return std::string{to_string(it->state)};
}

}  // namespace

TEST_CASE("catalogue: read, checked, written back, flags") {
    const auto c = catalog();
    REQUIRE(c.tests.size() == 2);
    CHECK(c.find("GLU")->unit == "mg/dL");
    CHECK(c.find("FLUA")->kind == TestDef::Kind::qualitative);
    CHECK(parse_catalog(to_yaml(c)).value() == c);

    CHECK(abnormal_flag(*c.find("GLU"), "65") == "L");
    CHECK(abnormal_flag(*c.find("GLU"), "90") == "N");
    CHECK(abnormal_flag(*c.find("GLU"), "150") == "H");
    CHECK(abnormal_flag(*c.find("GLU"), "n/a").empty());
    CHECK(abnormal_flag(*c.find("FLUA"), "DETECTED") == "A");
    CHECK(abnormal_flag(*c.find("FLUA"), "NOT DETECTED") == "N");

    for (const auto* bad : {
             "lisbench: catalog/2\ntests: []",
             "lisbench: catalog/1\ntests: [{code: A}, {code: A}]",
             "lisbench: catalog/1\ntests: [{code: A, colour: red}]",
             "lisbench: catalog/1\ntests: [{code: 'A|B'}]",
             "lisbench: catalog/1\ntests: [{code: A, kind: qualitative}]",
             "lisbench: catalog/1\ntests: [{code: A, reference: {low: 9, high: 1}}]",
             "lisbench: catalog/1\ntests: [{code: A, kind: qualitative, values: [X], abnormal: "
             "[Y]}]",
         }) {
        INFO(bad);
        CHECK_FALSE(parse_catalog(bad));
    }
    const auto err = parse_catalog("lisbench: catalog/1\ntests:\n  - code: A\n    colour: red\n");
    REQUIRE_FALSE(err);
    CHECK(err.error().starts_with("line 4: unknown key 'colour'"));
}

TEST_CASE("session file: typed, checked, without URIs, written back") {
    const auto c = parse_session(R"(
lisbench: session/1
name: bench-uno
type: device
timing: fast-test
links:
  - {role: main, transport: serial, serial: {port: /dev/ttyUSB1, baud: 9600, parity: even, stop_bits: 2}}
device: {mode: host-query, specimens: {prefix: X-, start: 7}, interval: 1s, query_timeout: 5s}
)");
    REQUIRE(c);
    CHECK(c->type == SessionType::device);
    CHECK(c->fast_timing);
    CHECK(c->device.mode == DeviceMode::host_query);
    CHECK(c->device.interval == 1s);
    const auto& l = c->links.at(0);
    CHECK(l.label() == "serial /dev/ttyUSB1 9600 8E2");
    CHECK(std::get<io::Serial>(l.endpoint()).parity == io::Parity::even);
    CHECK(parse_session(to_yaml(*c)).value() == *c);

    const auto lis = parse_session(R"(
lisbench: session/1
name: lis-two-ports
type: lis
links:
  - {role: orders, transport: tcp-server, tcp: {port: 15000}}
  - {role: results, transport: tcp-client, tcp: {host: 10.0.0.5, port: 15001}}
lis: {unsolicited: ignore, no_order_reply: empty}
)");
    REQUIRE(lis);
    CHECK(lis->links.at(0).label() == "waits on port 15000 (0.0.0.0)");
    CHECK(lis->links.at(1).label() == "connects to 10.0.0.5:15001");
    CHECK(lis->lis.unsolicited == Unsolicited::ignore);
    CHECK(parse_session(to_yaml(*lis)).value() == *lis);

    const auto base = std::string{"lisbench: session/1\nname: x\n"};
    for (const auto& bad : {
             base + "type: lis\n",  // no links
             base + "type: robot\nlinks: [{transport: tcp-server, tcp: {port: 1}}]\n",
             base + "type: lis\nlinks: [{transport: tcp-server, tcp: {port: 70000}}]\n",
             base + "type: lis\nlinks: [{transport: serial}]\n",
             base + "type: lis\nlinks: [{transport: tcp-client, tcp: {port: 1}}]\n",  // no host
             base + "type: lis\nlinks: [{role: orders, transport: tcp-server, tcp: {port: 1}}]\n",
             base + "type: lis\nlinks: [{transport: tcp-server, tcp: {port: 1}}]\nwhat: 1\n",
             base +
                 "type: device\nlinks: [{transport: tcp-server, tcp: {port: 1}}]\n",  // no device:
             base + "type: device\nlinks: [{role: orders, transport: tcp-server, tcp: {port: "
                    "1}}]\ndevice: {mode: host-query}\n",
             base + "type: device\nlinks: [{transport: tcp-server, tcp: {port: 1}}]\ndevice: "
                    "{mode: unsolicited}\n",  // no tests
             base + "type: device\nlinks: [{transport: tcp-server, tcp: {port: 1}}]\ndevice: "
                    "{mode: host-query, interval: soon}\n",
             std::string{"lisbench: session/1\nname: ../evil\ntype: lis\nlinks: [{transport: "
                         "tcp-server, tcp: {port: 1}}]\n"},
         }) {
        INFO(bad);
        CHECK_FALSE(parse_session(bad));
    }
    CHECK(valid_session_name("bench-uno.2"));
    CHECK_FALSE(valid_session_name(".hidden"));
    CHECK_FALSE(valid_session_name("a/b"));
}

TEST_CASE("order book: host query through to final, item states follow the messages") {
    OrderBook b;
    REQUIRE(b.add_specimen({.id = "S-1", .patient_id = "P-9", .patient_name = "Doe^Jane"}));
    REQUIRE(b.add_test("S-1", "GLU"));
    REQUIRE(b.add_test("S-1", "FLUA"));
    CHECK_FALSE(b.add_test("S-1", "GLU"));       // already there
    CHECK_FALSE(b.add_specimen({.id = "S-1"}));  // exists
    CHECK(state_of(b, "S-1", "GLU") == "new");

    // The instrument asks for S-1.
    const auto h =
        b.receive(message({"H|\\^&|||uno", "Q|1|^S-1||ALL||||||||O", "L|1|N"}), Unsolicited::flag);
    REQUIRE(h.query);
    CHECK(*h.query == std::vector<std::string>{"S-1"});
    CHECK_FALSE(h.query_all);

    const auto answer = b.answer(*h.query, false, NoOrderReply::y_record);
    REQUIRE(answer.records.size() == 5);  // H P O O L
    CHECK(answer.records[2].starts_with("O|1|S-1||^^^GLU|R||||||N||||||||||||||Q"));
    CHECK(answer.records[3].starts_with("O|2|S-1||^^^FLUA|"));
    // Not yet delivered: still new.
    CHECK(state_of(b, "S-1", "GLU") == "new");
    b.delivered(answer);
    CHECK(state_of(b, "S-1", "GLU") == "answered");

    // The instrument reports: preliminary, then final, then a correction.
    const auto report = [&](std::string_view status, std::string_view value) {
        return b.receive(
            message({"H|\\^&|||uno", "P|1", "O|1|S-1||^^^GLU",
                     std::format("R|1|^^^GLU|{}|mg/dL||H||{}", value, status), "L|1|N"}),
            Unsolicited::flag);
    };
    CHECK(report("P", "140").changed);
    CHECK(state_of(b, "S-1", "GLU") == "preliminary");
    (void)report("F", "141");
    CHECK(state_of(b, "S-1", "GLU") == "final");
    CHECK(b.find("S-1")->items[0].result->value == "141");
    CHECK(b.find("S-1")->items[0].result->flags == "H");
    (void)report("C", "139");
    CHECK(state_of(b, "S-1", "GLU") == "corrected");
    CHECK(b.unmatched().empty());

    // Saved and restored as it was.
    CHECK(OrderBook::from_json(b.to_json()).value() == b);
}

TEST_CASE("order book: batch orders, cancellation, query for unknown specimens") {
    OrderBook b;
    REQUIRE(b.add_specimen({.id = "S-1", .priority = 'S'}));
    REQUIRE(b.add_test("S-1", "GLU"));
    REQUIRE(b.add_specimen({.id = "S-2"}));
    REQUIRE(b.add_test("S-2", "GLU"));
    CHECK_FALSE(b.cancel("S-1", "GLU"));  // not sent yet

    const auto o = b.orders({"S-1"});
    REQUIRE(o);
    CHECK(o->records.size() == 4);  // H P O L, only S-1
    CHECK(o->records[2].starts_with("O|1|S-1||^^^GLU|S||||||N"));
    b.delivered(*o);
    CHECK(state_of(b, "S-1", "GLU") == "sent");
    CHECK(state_of(b, "S-2", "GLU") == "new");
    CHECK_FALSE(b.orders({"S-1"}));  // nothing new for S-1 any more

    const auto c = b.cancel("S-1", "GLU");
    REQUIRE(c);
    CHECK(c->records[2].starts_with("O|1|S-1||^^^GLU|S||||||C"));
    b.delivered(*c);
    CHECK(state_of(b, "S-1", "GLU") == "cancelled");

    // Query for a specimen the LIS does not know: O-26 = Y, or only H and L.
    const auto y = b.answer({"S-404"}, false, NoOrderReply::y_record);
    REQUIRE(y.records.size() == 4);
    CHECK(y.records[2].starts_with("O|1|S-404||^^^|"));
    CHECK(y.records[2].ends_with("|Y"));
    CHECK(y.items.empty());
    CHECK(b.answer({"S-404"}, false, NoOrderReply::empty).records.size() == 2);

    // Query ALL: everything still new.
    const auto all = b.answer({}, true, NoOrderReply::y_record);
    REQUIRE(all.items.size() == 1);
    CHECK(all.items[0].first == "S-2");
}

TEST_CASE("order book: unsolicited results by policy") {
    const auto result =
        message({"H|\\^&|||uno", "P|1", "O|1|S-9||^^^GLU", "R|1|^^^GLU|90|mg/dL||N||F", "L|1|N"});
    {
        OrderBook b;
        const auto h = b.receive(result, Unsolicited::ignore);
        CHECK_FALSE(h.changed);
        CHECK(b.specimens().empty());
        CHECK(h.notes.at(0).contains("ignored"));
    }
    {
        OrderBook b;
        (void)b.receive(result, Unsolicited::flag);
        CHECK(b.specimens().empty());
        REQUIRE(b.unmatched().size() == 1);
        CHECK(b.unmatched()[0].specimen == "S-9");
        REQUIRE(b.accept_unmatched(0));
        CHECK(b.unmatched().empty());
        CHECK(state_of(b, "S-9", "GLU") == "final");
        CHECK(b.find("S-9")->items[0].unsolicited);
        CHECK_FALSE(b.discard_unmatched(0));
    }
    {
        OrderBook b;
        (void)b.receive(result, Unsolicited::accept);
        CHECK(state_of(b, "S-9", "GLU") == "final");
        CHECK(b.find("S-9")->items[0].unsolicited);
    }
    {
        // An item that exists but was never sent: the result is still unsolicited.
        OrderBook b;
        REQUIRE(b.add_specimen({.id = "S-9"}));
        REQUIRE(b.add_test("S-9", "GLU"));
        const auto h = b.receive(result, Unsolicited::flag);
        CHECK(state_of(b, "S-9", "GLU") == "new");
        CHECK(b.unmatched().size() == 1);
        CHECK(h.notes.at(0).contains("order is new"));
    }
}

TEST_CASE("device model: specimens, query, answer, stable results") {
    DeviceConfig cfg{.mode = DeviceMode::host_query, .prefix = "S-", .start = 41};
    DeviceModel m{cfg, catalog(), "uno"};
    DeviceSpecimen s;
    REQUIRE(m.next(s));
    CHECK(s.id == "S-00041");
    CHECK(s.tests.empty());  // host query: tests come from the LIS
    const auto q = m.query(s);
    REQUIRE(q.size() == 3);
    CHECK(q[1] == "Q|1|^S-00041||ALL||||||||O");

    // The LIS answers with two tests (and something for another specimen).
    const auto answer =
        message({"H|\\^&|||lis", "P|1", "O|1|S-00041||^^^GLU|R||||||N||||||||||||||Q",
                 "O|2|S-00041||^^^FLUA|R||||||N||||||||||||||Q",
                 "O|3|S-77||^^^GLU|R||||||N||||||||||||||Q", "L|1|F"});
    s.tests = DeviceModel::ordered_tests(answer, s.id);
    CHECK(s.tests == std::vector<std::string>{"GLU", "FLUA"});
    CHECK(DeviceModel::ordered_tests(
              message({"H|\\^&|||lis", "P|1", "O|1|S-00041||^^^|||||||N||||||||||||||Y", "L|1|N"}),
              s.id)
              .empty());

    const auto r = m.results(s);
    // H P O R O R L
    REQUIRE(r.size() == 7);
    CHECK(r[2].starts_with("O|1|S-00041||^^^GLU|"));
    CHECK(r[3].starts_with("R|1|^^^GLU|" + s.values[0] + "|mg/dL|70-110|"));
    CHECK(r[5].starts_with("R|1|^^^FLUA|" + s.values[1]));  // R numbering restarts under each O
    // Same specimen, same values.
    DeviceSpecimen again = s;
    (void)m.results(again);
    CHECK(again.values == s.values);

    // A fixed list runs out.
    DeviceModel listed{{.mode = DeviceMode::unsolicited, .specimens = {"A", "B"}, .tests = {"GLU"}},
                       catalog(),
                       "x"};
    DeviceSpecimen a;
    CHECK(listed.next(a));
    CHECK(a.tests == std::vector<std::string>{"GLU"});
    CHECK(listed.next(a));
    CHECK_FALSE(listed.next(a));
}

TEST_CASE("LIS and instrument models together: Q, O, R without a wire") {
    OrderBook lis;
    REQUIRE(lis.add_specimen({.id = "S-00001"}));
    REQUIRE(lis.add_test("S-00001", "GLU"));
    DeviceModel dev{{.mode = DeviceMode::host_query}, catalog(), "uno"};
    DeviceSpecimen s;
    REQUIRE(dev.next(s));

    const auto h = lis.receive(message(dev.query(s)), Unsolicited::flag);
    REQUIRE(h.query);
    const auto answer = lis.answer(*h.query, h.query_all, NoOrderReply::y_record);
    lis.delivered(answer);
    s.tests = DeviceModel::ordered_tests(message(answer.records), s.id);
    REQUIRE(s.tests == std::vector<std::string>{"GLU"});
    (void)lis.receive(message(dev.results(s)), Unsolicited::flag);
    CHECK(state_of(lis, "S-00001", "GLU") == "final");
    CHECK(lis.find("S-00001")->items[0].result->value == s.values[0]);
    CHECK(lis.unmatched().empty());
}

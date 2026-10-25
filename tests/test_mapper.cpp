// Mapper acceptance (docs/spec/features.md §5), against in-memory SQLite.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "lisbench/mapper/mapping.hpp"

using namespace lisbench;
using namespace lisbench::mapper;

namespace {

Database fixture() {
    auto db = Database::open(":memory:").value();
    REQUIRE(db.script(R"(
        CREATE TABLE patients (id TEXT, name TEXT);
        CREATE TABLE results (id TEXT, patient TEXT, test TEXT, value TEXT, note TEXT, sent TEXT);
        INSERT INTO patients VALUES ('P1', 'Doe|John^Jr'), ('P2', 'Müller');
        INSERT INTO results VALUES ('R1', 'P1', 'GLU', '5.4', NULL, NULL),
                                   ('R2', 'P1', 'NA', '140', 'hemolysed', NULL),
                                   ('R3', 'P2', 'K', '4.1', NULL, NULL);
    )"));
    return db;
}

Context context(std::map<std::string, std::string> args = {}) {
    return Context{.cfg = {{"sender", "lab|1"}},
                   .args = std::move(args),
                   .rows = {},
                   .now = "20261025120000",
                   .message_id = "ID1",
                   .error = {}};
}

std::vector<std::string> wire(const lis02::Message& m, lis02::TextOptions t = {}) {
    return lis02::serialize(m, t).value();
}

constexpr std::string_view two_levels = R"(
mapping: 1
message:
  - H: {sender_name: "${cfg.sender}", message_control_id: "${message_id}"}
  - query: SELECT id, name FROM patients ORDER BY id
    as: patient
    each:
      - P: {laboratory_patient_id: "${patient.id}", patient_name: "${patient.name}"}
      - O: {specimen_id: "S-${patient.id}"}
      - query: SELECT id, test, value, note FROM results WHERE patient = :p ORDER BY id
        as: result
        params: {p: "${patient.id}"}
        each:
          - R: {universal_test_id: ["", "", "", "${result.test}"], value: "${result.value}",
                instrument_id: "${patient.id}/${result.id}"}
          - C: {comment_source: I, comment_text: "${result.note?}", skip_if_empty: true}
  - L: {termination_code: N}
)";

}  // namespace

TEST_CASE("mapper: templates") {
    const auto t = parse_template("a${p.x}b$$c${now}", 3).value();
    CHECK(t.pieces.size() == 4);
    CHECK_FALSE(t.is_literal());
    CHECK(parse_template("plain $ text").value().is_literal());
    CHECK(render(parse_template("$$5 ${cfg.sender}").value(), context()).value() == "$5 lab|1");

    const auto unterminated = parse_template("x ${p.x", 7);
    REQUIRE_FALSE(unterminated);
    CHECK(unterminated.error().contains("line 7"));
    CHECK_FALSE(parse_template("${today}"));  // unknown built-in
    CHECK_FALSE(parse_template("${p.x-y}"));  // invalid name
    // A value that looks like a placeholder is not resolved again.
    Context ctx = context();
    ctx.rows.push_back(Row{.scope = "p", .columns = {{"x", "${cfg.sender}"}}});
    CHECK(render(parse_template("${p.x}").value(), ctx).value() == "${cfg.sender}");
}

TEST_CASE("mapper: scopes, missing columns and NULL") {
    Context ctx = context();
    ctx.rows.push_back(Row{.scope = "outer", .columns = {{"id", "O1"}, {"n", std::nullopt}}});
    ctx.rows.push_back(Row{.scope = "inner", .columns = {{"id", "I1"}}});
    CHECK(render(parse_template("${outer.id}/${inner.id}").value(), ctx).value() == "O1/I1");
    CHECK(render(parse_template("${outer.n?}").value(), ctx).value().empty());
    const auto null = render(parse_template("${outer.n}", 4).value(), ctx);
    REQUIRE_FALSE(null);
    CHECK(null.error().contains("NULL"));
    CHECK(null.error().contains("line 4"));
    CHECK_FALSE(render(parse_template("${inner.nope}").value(), ctx));
    CHECK_FALSE(render(parse_template("${other.id}").value(), ctx));
    CHECK_FALSE(render(parse_template("${arg.missing}").value(), ctx));
}

TEST_CASE("mapper: nested queries build a valid message with correct sequence numbers") {
    auto db = fixture();
    const auto m = parse_mapping(two_levels, "two.yaml").value();
    const auto message = build_message(m, db, context()).value();
    CHECK(lis02::validate(message).empty());
    CHECK(wire(message) == std::vector<std::string>{
                               "H|\\^&|ID1||lab&F&1",
                               "P|1||P1||Doe&F&John&S&Jr",  // database values are escaped
                               "O|1|S-P1",
                               "R|1|^^^GLU|5.4||||||||||P1/R1",  // inner id does not hide outer id
                               "R|2|^^^NA|140||||||||||P1/R2",
                               "C|1|I|hemolysed",
                               "P|2||P2||M&XFC&ller",
                               "O|1|S-P2",
                               "R|1|^^^K|4.1||||||||||P2/R3",
                               "L|1|N",
                           });
}

TEST_CASE("mapper: a record outside the hierarchy aborts with its line") {
    auto db = fixture();
    const auto m = parse_mapping(R"(
mapping: 1
message:
  - H: {}
  - P: {}
  - R: {value: "1"}
)",
                                 "bad.yaml")
                       .value();
    const auto result = build_message(m, db, context());
    REQUIRE_FALSE(result);
    CHECK(result.error().contains("line 6"));
    CHECK(result.error().contains("R without a preceding O"));
}

TEST_CASE("mapper: mapping errors name the line") {
    const auto error_of = [](std::string_view yaml) {
        const auto m = parse_mapping(yaml, "x.yaml");
        REQUIRE_FALSE(m);
        return m.error();
    };
    CHECK(error_of("message: []").contains("mapping: 1"));
    CHECK(error_of("mapping: 1\nmessage:\n  - R: {no_such_field: x}\n").contains("line 3"));
    CHECK(
        error_of("mapping: 1\nmessage:\n  - R: {sequence_number: 5}\n").contains("automatically"));
    CHECK(error_of("mapping: 1\nmessage:\n  - X: {}\n").contains("unknown key"));
    CHECK(
        error_of("mapping: 1\nmessage:\n  - query: SELECT 1\n    as: cfg\n").contains("reserved"));
    CHECK(error_of("mapping: 1\nmessage:\n  - C: {comment_text: \"${r.x\"}\n")
              .contains("not closed"));
}

TEST_CASE("mapper: parameters are bound, not pasted") {
    auto db = fixture();
    const auto m = parse_mapping(R"(
mapping: 1
message:
  - H: {}
  - query: SELECT id FROM patients WHERE id = :id
    as: p
    params: {id: "${arg.id}"}
    each:
      - P: {laboratory_patient_id: "${p.id}"}
)",
                                 "bind.yaml")
                       .value();
    const auto normal = build_message(m, db, context({{"id", "P1"}})).value();
    CHECK(normal.records.size() == 3);  // H P L
    const auto injected = build_message(m, db, context({{"id", "P1' OR '1'='1"}})).value();
    CHECK(injected.records.size() == 2);  // H L: the text matched no patient
}

TEST_CASE("mapper: UTF-8 file with BOM, non-ASCII from the database per text options") {
    auto db = fixture();
    const std::string yaml = std::string{"\xEF\xBB\xBF"} + R"(mapping: 1
text: {charset: utf8, non_ascii: ascii-hex}
message:
  - H: {}
  - query: SELECT name FROM patients WHERE id = 'P2'
    as: p
    each:
      - P: {patient_name: "${p.name}"}
)";
    const auto path = std::filesystem::temp_directory_path() / "lisbench_bom_test.yaml";
    {
        std::ofstream out(path, std::ios::binary);
        out << yaml;
    }
    const auto m = load_mapping(path);
    std::filesystem::remove(path);
    REQUIRE(m);
    const auto message = build_message(*m, db, context()).value();
    CHECK(wire(message, m->text)[1] == "P|1||||M&XC3BC&ller");
}

TEST_CASE("mapper: write-back, message IDs, time stamps") {
    auto db = fixture();
    const auto m = parse_mapping(R"(
mapping: 1
message:
  - H: {}
on_success: {sql: "UPDATE results SET sent = :now WHERE patient = :p", params: {now: "${now}", p: "${arg.p}"}}
)",
                                 "wb.yaml")
                       .value();
    REQUIRE(m.on_success);
    CHECK(write_back(*m.on_success, db, context({{"p", "P1"}})).value() == 2);
    CHECK(db.query("SELECT count(*) AS n FROM results WHERE sent = '20261025120000'", {})
              .value()[0]
              .at("n") == "2");
    CHECK_FALSE(db.execute("UPDATE results SET sent = :x", {}));  // unbound parameter

    CHECK(stable_message_id("a.yaml", {{"id", "1"}}) == stable_message_id("a.yaml", {{"id", "1"}}));
    CHECK(stable_message_id("a.yaml", {{"id", "1"}}) != stable_message_id("a.yaml", {{"id", "2"}}));
    CHECK(stable_message_id("a.yaml", {}).size() == 16);
    const auto ts = local_timestamp(std::chrono::system_clock::now());
    CHECK(ts.size() == 14);
    CHECK(std::ranges::all_of(ts, [](char c) { return c >= '0' && c <= '9'; }));
}

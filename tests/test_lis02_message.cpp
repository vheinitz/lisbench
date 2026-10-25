#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <stdexcept>

#include "lisbench/lis02_message.hpp"

using namespace lisbench::lis02;

namespace {

bool has_error(const std::vector<Issue>& issues) {
    return std::ranges::any_of(issues,
                               [](const Issue& i) { return i.severity == Severity::error; });
}

}  // namespace

TEST_CASE("records are split into fields, repeats and components") {
    const auto m = parse_text("H|\\^&|||sender^1.0\rO|1|S1||^^^GLU\\^^^NA|R\rL|1|N\r");
    REQUIRE(m);
    REQUIRE(m->records.size() == 3);
    const auto& h = m->records[0];
    CHECK(h.kind() == RecordType::header);
    CHECK(h.component(2) == "\\^&");
    CHECK(h.component(5, 0, 0) == "sender");
    CHECK(h.component(5, 0, 1) == "1.0");

    const auto& o = m->records[1];
    REQUIRE(o.field(5) != nullptr);
    CHECK(o.field(5)->size() == 2);
    CHECK(o.component(5, 0, 3) == "GLU");
    CHECK(o.component(5, 1, 3) == "NA");
    CHECK(o.component(6) == "R");
    CHECK(o.component(30) == "");  // beyond the record: empty, not an error
}

TEST_CASE("serialize reproduces the wire text and drops trailing empty fields") {
    const std::string text = "H|\\^&|||sender^1.0\rP|1||PID&F&1||Doe^John\rL|1|N";
    const auto m = parse_text(text);
    REQUIRE(m);
    const auto lines = serialize(*m);
    REQUIRE(lines);
    CHECK((*lines)[0] == "H|\\^&|||sender^1.0");
    CHECK((*lines)[1] == "P|1||PID&F&1||Doe^John");
    CHECK(m->records[1].component(4) == "PID|1");

    Record r{.type = 'R', .fields = {make_field("R"), make_field("1"), make_field("x"), {}, {}}};
    CHECK(serialize(r, Delimiters{}).value() == "R|1|x");
}

TEST_CASE("the record type cannot be assigned") {
    Record r{.type = 'R', .fields = {make_field("R")}};
    CHECK_THROWS_AS(r.set(1, make_field("P")), std::invalid_argument);
}

TEST_CASE("validate accepts a correct message") {
    const auto m = parse_text(
        "H|\\^&\rP|1\rO|1|S1\rR|1|^^^A|1\rC|1|I|x\rC|2|I|y\rR|2|^^^B|2\rO|2|S2\rP|2\rO|1|S3\rL|1|"
        "N\r");
    REQUIRE(m);
    const auto issues = validate(*m);
    CHECK(issues.empty());
}

TEST_CASE("validate reports hierarchy and sequence errors") {
    SECTION("R without O") {
        CHECK(has_error(validate(parse_text("H|\\^&\rP|1\rR|1\rL|1\r").value())));
    }
    SECTION("O without P, unless allowed") {
        const auto m = parse_text("H|\\^&\rO|1|QC1\rR|1|^^^A|1\rL|1\r").value();
        CHECK(has_error(validate(m)));
        CHECK_FALSE(has_error(validate(m, {.allow_order_without_patient = true})));
    }
    SECTION("sequence restarts per parent") {
        CHECK(has_error(validate(parse_text("H|\\^&\rP|1\rO|1\rR|1\rO|2\rR|2\rL|1\r").value())));
    }
    SECTION("missing L") {
        CHECK(has_error(validate(parse_text("H|\\^&\rP|1\r").value())));
    }
    SECTION("record before H") {
        CHECK(has_error(validate(parse_text("P|1\rL|1\r").value())));
    }
    SECTION("two messages in one transfer are fine") {
        CHECK(validate(parse_text("H|\\^&\rP|1\rL|1\rH|\\^&\rQ|1|^S1\rL|1\r").value()).empty());
    }
}

TEST_CASE("builder places records and numbers them") {
    Builder b;
    REQUIRE(b.add(RecordType::header));
    REQUIRE(b.add(RecordType::patient));
    REQUIRE(b.add(RecordType::order));
    REQUIRE(b.add(RecordType::result));
    REQUIRE(b.add(RecordType::comment));
    REQUIRE(b.add(RecordType::comment));
    REQUIRE(b.add(RecordType::result));
    REQUIRE(b.add(RecordType::patient));
    auto o = b.add(RecordType::order);
    REQUIRE(o);
    o->get().set(3, make_field("S-2"));
    const auto m = b.finish();
    const auto lines = serialize(m).value();
    CHECK(lines == std::vector<std::string>{"H|\\^&", "P|1", "O|1", "R|1", "C|1", "C|2", "R|2",
                                            "P|2", "O|1|S-2", "L|1"});
    CHECK(validate(m).empty());
}

TEST_CASE("builder refuses records that do not fit") {
    Builder b;
    CHECK_FALSE(b.add(RecordType::patient));  // before H
    REQUIRE(b.add(RecordType::header));
    CHECK_FALSE(b.add(RecordType::result));  // no order
    CHECK_FALSE(b.add(RecordType::order));   // no patient
    CHECK_FALSE(b.add(RecordType::header));  // second H
    REQUIRE(b.add(RecordType::terminator));
    CHECK_FALSE(b.add(RecordType::comment));  // after L
}

TEST_CASE("field names come from one table") {
    CHECK(field_number(RecordType::result, "value") == 4U);
    CHECK(field_number(RecordType::order, "specimen_id") == 3U);
    CHECK(field_name(RecordType::header, 14) == "timestamp");
    CHECK(info(RecordType::patient).fields.size() == 35);
    CHECK(info(RecordType::order).fields.size() == 31);
    CHECK(info(RecordType::result).fields.size() == 14);
    CHECK(info(RecordType::query).fields.size() == 13);
    CHECK_FALSE(field_number(RecordType::result, "no_such_field"));
}

TEST_CASE("odd first fields survive the round trip") {
    for (const std::string text : {"|abc", "XY|1|2", "M|1|vendor^data"}) {
        INFO(text);
        const auto m = parse_text(text);
        REQUIRE(m);
        CHECK(serialize(*m).value() == std::vector<std::string>{text});
    }
    CHECK_FALSE(parse_text("XY|1", {.text = {}, .strict = true}));
}

TEST_CASE("delimiters may not change within one transfer") {
    CHECK_FALSE(parse_text("H|\\^&\rL|1\rH!@#$\rL!1\r"));
    CHECK(parse_text("H|\\^&\rL|1\rH|\\^&\rL|1\r"));
}

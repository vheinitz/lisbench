#include <catch2/catch_test_macros.hpp>

#include "lisbench/watch.hpp"

using namespace lisbench;

namespace {

lis02::Record rec(std::string_view wire) {
    return lis02::parse_record(wire, {}).value();
}

bool hit(std::string_view expr, std::string_view wire) {
    const auto e = watch::parse(expr);
    REQUIRE(e);
    return watch::matches(*e, rec(wire), {});
}

}  // namespace

TEST_CASE("watch expressions are parsed") {
    const auto e = watch::parse("O.universal_test_id.4 == TSH");
    REQUIRE(e);
    CHECK(e->record_type == 'O');
    CHECK(e->field == 5);
    CHECK(e->component == 4U);
    CHECK(e->op == watch::Op::eq);
    CHECK(e->value == "TSH");
    CHECK(watch::parse("R.4>=5.5")->op == watch::Op::ge);
    CHECK(watch::parse("P.patient_name contains \"Doe\"")->value == "Doe");
    // Flags such as "<" (below the scale) must be quoted.
    CHECK(watch::parse("R.abnormal_flags == \"<\"")->value == "<");

    for (const auto* bad :
         {"R.value", "X.value == 1", "R.nope == 1", "R.1 == R", "R.value ~ 1", "R.value.0 == 1",
          "R.value matches (", "R.value >> 100", "R.value => 100", "R.abnormal_flags == <"}) {
        INFO(bad);
        CHECK_FALSE(watch::parse(bad));
    }
}

TEST_CASE("watch expressions compare numbers as numbers, text as text") {
    const auto r = "R|1|^^^GLU|150|mg/dL||H";
    CHECK(hit("R.value > 100", r));
    CHECK_FALSE(hit("R.value > 1000", r));
    CHECK(hit("R.value == 150.0", r));  // numeric equality
    CHECK(hit("R.units == mg/dL", r));
    CHECK(hit("R.universal_test_id.4 == GLU", r));
    CHECK(hit("R.universal_test_id == ^^^GLU", r));  // whole field
    CHECK(hit("R.abnormal_flags matches ^(H|HH)$", r));
    CHECK(hit("R.units contains dL", r));
    CHECK(hit("R.value != 151", r));
    CHECK_FALSE(hit("O.specimen_id == x", r));  // other record type never matches
    CHECK_FALSE(hit("R.13 == x", r));           // missing field is empty
    CHECK(hit("R.13 == \"\"", r));
}

TEST_CASE("watch: an ordering against a number needs a number") {
    // Seen in the web UI with GeneXpert-style values: "DETECTED^" > "100" as text.
    CHECK_FALSE(hit("R.value > 100", "R|1|^^^FLU|DETECTED^"));
    CHECK_FALSE(hit("R.value > 100", "R|1|^^^FLU|^27.4"));  // a component holds the number
    CHECK_FALSE(hit("R.value >= 0", "R|1|^^^ANA|1:320"));
    CHECK_FALSE(hit("R.value < 100", "R|1|^^^GLU|"));  // empty is no number either
    CHECK(hit("R.value.2 > 20", "R|1|^^^FLU|^27.4"));
    CHECK(hit("R.value > -1", "R|1|^^^GLU|-0.5"));
    // Text ordering stays available when the right side is not a number.
    CHECK(hit("R.units > a", "R|1|^^^GLU|1|mg/dL"));
    // Equality keeps comparing text when one side is not a number.
    CHECK(hit("R.value != 100", "R|1|^^^FLU|DETECTED^"));
}

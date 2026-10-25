#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "lisbench/sim/profile.hpp"

using namespace lisbench;
namespace fs = std::filesystem;

TEST_CASE("profile: the GeneXpert profile by id, by directory and by file") {
    const fs::path devices{LISBENCH_DEVICES_DIR};
    for (const auto& ref :
         {std::string{"cepheid-genexpert"}, (devices / "cepheid-genexpert").string(),
          (devices / "cepheid-genexpert" / "profile.yaml").string()}) {
        INFO(ref);
        const auto p = sim::load_profile(ref, {devices});
        REQUIRE(p);
        CHECK(p->id == "cepheid-genexpert");
        CHECK(p->vendor == "Cepheid");
        CHECK(p->text.local_escape == lis02::LocalEscape::utf16_hex);
        CHECK(p->text.non_ascii == lis02::NonAscii::raw);
        CHECK(p->max_text == 240U);
        CHECK_FALSE(p->contention_winner);  // "unclear" in the profile
    }
    CHECK_FALSE(sim::load_profile("no-such-instrument", {devices}));
    // An id is not looked up when it contains a path separator.
    CHECK_FALSE(sim::load_profile("../devices/cepheid-genexpert", {devices / "x"}));
}

TEST_CASE("profile: values are checked, timing takes what the profile knows") {
    CHECK_FALSE(sim::parse_profile("vendor: X", {}));  // no id
    CHECK_FALSE(sim::parse_profile("id: x\nlis02: {charset: ebcdic}", {}));
    CHECK_FALSE(sim::parse_profile("id: x\nlis02: {local_escape: utf32}", {}));
    CHECK_FALSE(sim::parse_profile("id: x\nlis01: {max_text: 0}", {}));
    CHECK_FALSE(sim::parse_profile("id: x\nlis01: {contention_winner: both}", {}));

    const auto p =
        sim::parse_profile("id: x\nlis01: {max_text: 64, contention_winner: computer}", {});
    REQUIRE(p);
    lis01::Timing t;
    t.reply_timeout = std::chrono::milliseconds{1234};
    sim::apply(*p, t);
    CHECK(t.contention_winner == lis01::Role::computer);
    CHECK(t.max_text == 64U);
    CHECK(t.reply_timeout == std::chrono::milliseconds{1234});  // untouched

    lis01::Timing unchanged;
    sim::apply(sim::parse_profile("id: y", {}).value(), unchanged);
    CHECK(unchanged.contention_winner == lis01::Role::instrument);
    CHECK(unchanged.max_text == lis01::max_text_size);
}

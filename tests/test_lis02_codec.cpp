#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include "lisbench/lis02_codec.hpp"

using namespace lisbench::lis02;

namespace {

std::string utf8_of(char32_t cp) {
    std::string s;
    utf8_append(s, cp);
    return s;
}

bool is_7bit(std::string_view s) {
    return std::ranges::all_of(s, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

const Delimiters std_delims{};

}  // namespace

TEST_CASE("delimiters are read from the header") {
    const auto d = delimiters_from_header("H|\\^&|||sender");
    REQUIRE(d);
    CHECK(*d == Delimiters{});
    CHECK(d->definition() == "|\\^&");

    const auto custom = delimiters_from_header("H!@#$");
    REQUIRE(custom);
    CHECK(custom->field == '!');
    CHECK(custom->escape == '$');

    CHECK_FALSE(delimiters_from_header("H|\\^"));  // too short
    CHECK_FALSE(delimiters_from_header("H||^&"));  // not distinct
    CHECK_FALSE(delimiters_from_header("H|A^&"));  // alphanumeric
    CHECK_FALSE(delimiters_from_header("P|1"));    // not a header
}

TEST_CASE("delimiters and controls are escaped") {
    const auto e = encode("a|b^c\\d&e\rf\ng", std_delims, {});
    REQUIRE(e);
    CHECK(e->wire == "a&F&b&S&c&R&d&E&e&X0D&f&X0A&g");
    const auto back = decode(e->wire, std_delims, {});
    REQUIRE(back);
    CHECK(*back == "a|b^c\\d&e\rf\ng");
}

TEST_CASE("every code point U+0000..U+00FF in every mode") {
    for (char32_t cp = 0; cp <= 0xFF; ++cp) {
        const auto value = "x" + utf8_of(cp) + "y";
        INFO("U+" << std::hex << static_cast<unsigned>(cp));

        {  // ascii-hex, Latin-1: 7-bit wire, exact round trip
            const TextOptions o{.charset = Charset::latin1, .non_ascii = NonAscii::ascii_hex};
            const auto e = encode(value, std_delims, o);
            REQUIRE(e);
            CHECK(is_7bit(e->wire));
            CHECK(decode(e->wire, std_delims, o).value() == value);
        }
        {  // ascii-hex, UTF-8: 7-bit wire, exact round trip
            const TextOptions o{.charset = Charset::utf8, .non_ascii = NonAscii::ascii_hex};
            const auto e = encode(value, std_delims, o);
            REQUIRE(e);
            CHECK(is_7bit(e->wire));
            CHECK(decode(e->wire, std_delims, o).value() == value);
        }
        {  // raw, Latin-1: one byte per character above 0x7F, exact round trip
            const TextOptions o{.charset = Charset::latin1, .non_ascii = NonAscii::raw};
            const auto e = encode(value, std_delims, o);
            REQUIRE(e);
            if (cp >= 0xA0) {
                CHECK(e->wire.size() == 3);
            }
            CHECK(decode(e->wire, std_delims, o).value() == value);
        }
        {  // transliterate: 7-bit wire, loss is reported
            const TextOptions o{.charset = Charset::latin1, .non_ascii = NonAscii::transliterate};
            const auto e = encode(value, std_delims, o);
            REQUIRE(e);
            CHECK(is_7bit(e->wire));
            const auto back = decode(e->wire, std_delims, o).value();
            if (cp < 0x80) {
                CHECK(back == value);
            } else if (transliteration(cp).empty()) {
                CHECK(e->dropped == std::vector<char32_t>{cp});
            } else {
                CHECK(e->dropped.empty());
                CHECK(back == "x" + std::string{transliteration(cp)} + "y");
            }
        }
    }
}

TEST_CASE("hex escapes carry bytes, not code points") {
    const TextOptions latin1{.charset = Charset::latin1, .non_ascii = NonAscii::ascii_hex};
    const TextOptions utf8{.charset = Charset::utf8, .non_ascii = NonAscii::ascii_hex};
    CHECK(encode("é", std_delims, latin1)->wire == "&XE9&");
    CHECK(encode("é", std_delims, utf8)->wire == "&XC3A9&");
    CHECK(decode("Ren&XE9&", std_delims, latin1).value() == "René");
}

TEST_CASE("characters outside Latin-1") {
    const TextOptions latin1{.charset = Charset::latin1, .non_ascii = NonAscii::ascii_hex};
    CHECK_FALSE(encode("€", std_delims, latin1));
    const TextOptions translit{.charset = Charset::latin1, .non_ascii = NonAscii::transliterate};
    CHECK(encode("5 €", std_delims, translit)->wire == "5 EUR");
    CHECK(encode("Müller", std_delims, translit)->wire == "Mueller");
    const TextOptions utf8{.charset = Charset::utf8, .non_ascii = NonAscii::ascii_hex};
    CHECK(encode("€", std_delims, utf8)->wire == "&XE282AC&");
}

TEST_CASE("unknown and unterminated escapes") {
    const TextOptions lenient{};
    const TextOptions strict{.strict = true};
    CHECK(decode("a&H&b", std_delims, lenient).value() == "a&H&b");
    CHECK_FALSE(decode("a&H&b", std_delims, strict));
    CHECK(decode("a&Fb", std_delims, lenient).value() == "a&Fb");
    CHECK_FALSE(decode("a&Fb", std_delims, strict));
    CHECK(decode("&X4&", std_delims, lenient).value() == "&X4&");  // odd digit count
}

TEST_CASE("local escape Z as UTF-16 code units") {
    const Delimiters bs{.field = '|', .repeat = '@', .component = '^', .escape = '\\'};
    const TextOptions keep{.strict = true};
    const TextOptions utf16{.local_escape = LocalEscape::utf16_hex};
    const TextOptions utf16_strict{.local_escape = LocalEscape::utf16_hex, .strict = true};
    CHECK(decode("caf\\Z00E9\\", bs, keep).value() == "caf\\Z00E9\\");
    CHECK(decode("caf\\Z00E9\\", bs, utf16).value() == "café");
    CHECK(decode("\\Z34C8004100E9\\", bs, utf16).value() == "\u34C8Aé");
    CHECK(decode("x\\ZD83DDE00\\y", bs, utf16).value() == "x\U0001F600y");  // surrogate pair
    // Latin-1 bytes before and after the escape are converted as usual.
    CHECK(decode("\xE9\\Z20AC\\\xE9", bs, utf16).value() == "é€é");
    CHECK(decode("\\ZD83D\\", bs, utf16).value() == "\\ZD83D\\");  // lone surrogate kept
    CHECK_FALSE(decode("\\ZD83D\\", bs, utf16_strict));
    CHECK_FALSE(decode("\\Z12\\", bs, utf16_strict));

    CHECK(encode("€ 😀", bs, utf16)->wire == "\\Z20AC\\ \\ZD83DDE00\\");
    CHECK(encode("é", bs, utf16)->wire == "\\XE9\\");  // in the code page: hex as before
    CHECK_FALSE(encode("€", bs, {}));
    for (const char32_t cp : {char32_t{0x100}, char32_t{0x34C8}, char32_t{0xFFFD},
                              char32_t{0x10000}, char32_t{0x10FFFF}}) {
        std::string s;
        utf8_append(s, cp);
        CHECK(decode(encode(s, bs, utf16)->wire, bs, utf16).value() == s);
    }
}

TEST_CASE("invalid UTF-8 on the wire") {
    const TextOptions lenient{.charset = Charset::utf8};
    const TextOptions strict{.charset = Charset::utf8, .strict = true};
    CHECK(decode("a\xE9"
                 "b",
                 std_delims, lenient)
              .value() ==
          "a\xEF\xBF\xBD"
          "b");
    CHECK_FALSE(
        decode("a\xE9"
               "b",
               std_delims, strict));
    CHECK_FALSE(encode("bad \xC3", std_delims, {}));  // input must be UTF-8
}

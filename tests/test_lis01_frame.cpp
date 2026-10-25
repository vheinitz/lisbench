#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <random>

#include "lisbench/lis01_frame.hpp"

using namespace lisbench;
using namespace lisbench::lis01;

namespace {

bytes text_of_length(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::uniform_int_distribution<int> printable{0x20, 0x7E};
    bytes out(n);
    for (auto& b : out) {
        b = static_cast<std::byte>(printable(rng));
    }
    return out;
}

}  // namespace

TEST_CASE("checksum matches values computed independently") {
    // Sums computed outside this code base over FN..ETX inclusive.
    const auto header = to_bytes("1H|\\^&|||lisbench^0.1|||||||P|1|20261025120000\r\x03");
    CHECK(checksum(header) == 0x20);
    CHECK(checksum(to_bytes("2L|1|N\r\x03")) == 0x05);
    static_assert(checksum(bytes_view{}) == 0);
}

TEST_CASE("encoded frame has the LIS01-A2 layout") {
    const Frame f{.number = 2, .last = true, .text = to_bytes("L|1|N\r")};
    const auto raw = encode(f);
    CHECK(to_string(raw) ==
          "\x02"
          "2L|1|N\r\x03"
          "05\r\n");
    const auto back = decode(raw);
    REQUIRE(back);
    CHECK(*back == f);
}

TEST_CASE("split and reassemble: every length from 0 to 2000") {
    // Property: the frame texts concatenate to the input, only the last frame
    // carries ETX, no frame is longer than 247 bytes, every checksum verifies
    // and frame numbers count on modulo 8.
    for (std::size_t n = 0; n <= 2000; ++n) {
        const auto text = text_of_length(n, static_cast<std::uint32_t>(n));
        const auto frames = split(text, 1);
        REQUIRE_FALSE(frames.empty());
        bytes joined;
        std::uint8_t fn = 1;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto& f = frames[i];
            REQUIRE(f.number == fn);
            fn = next_frame_number(fn);
            REQUIRE(f.last == (i + 1 == frames.size()));
            const auto raw = encode(f);
            REQUIRE(raw.size() <= max_frame_size);
            const auto decoded = decode(raw);
            REQUIRE(decoded);
            REQUIRE(*decoded == f);
            joined.insert(joined.end(), f.text.begin(), f.text.end());
        }
        REQUIRE(joined == text);
        REQUIRE(frames.size() == (n == 0 ? 1 : (n + max_text_size - 1) / max_text_size));
    }
}

TEST_CASE("a record of exactly 240 bytes is a single ETX frame") {
    const auto frames = split(text_of_length(240, 7), 1);
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].last);
}

TEST_CASE("frame numbers wrap from 7 to 0") {
    const auto frames = split(text_of_length(3 * 240, 3), 6);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].number == 6);
    CHECK(frames[1].number == 7);
    CHECK(frames[2].number == 0);
}

TEST_CASE("decode rejects malformed frames") {
    auto good = encode(Frame{.number = 1, .last = false, .text = to_bytes("R|1|^^^GLU|5.4")});
    SECTION("checksum mismatch") {
        auto bad = good;
        bad[4] = std::byte{'X'};
        CHECK(decode(bad).error() == FrameError::checksum_mismatch);
    }
    SECTION("bad frame number") {
        auto bad = good;
        bad[1] = std::byte{'8'};
        CHECK(decode(bad).error() == FrameError::bad_frame_number);
    }
    SECTION("missing CR LF") {
        good.pop_back();
        CHECK(decode(good).error() == FrameError::no_crlf);
    }
    SECTION("no STX") {
        good[0] = std::byte{'x'};
        CHECK(decode(good).error() == FrameError::no_stx);
    }
    SECTION("too short") {
        CHECK(decode(to_bytes("\x02"
                              "1"))
                  .error() == FrameError::too_short);
    }
    SECTION("lower-case checksum digits are accepted") {
        const Frame f{.number = 3, .last = true, .text = to_bytes("C|1|L|x\r")};
        auto raw = encode(f);
        for (auto i : {raw.size() - 4, raw.size() - 3}) {
            const auto c = std::to_integer<char>(raw[i]);
            if (c >= 'A' && c <= 'F') {
                raw[i] = static_cast<std::byte>(c - 'A' + 'a');
            }
        }
        CHECK(decode(raw));
    }
    SECTION("text above the limit is rejected unless allowed") {
        const Frame f{.number = 1, .last = true, .text = text_of_length(250, 1)};
        CHECK(decode(encode(f)).error() == FrameError::text_too_long);
        CHECK(decode(encode(f), 300));
    }
}

TEST_CASE("deframe reassembles records and reports problems") {
    bytes stream{ctl::enq, ctl::ack};
    const auto long_record = text_of_length(500, 11);
    bytes record = long_record;
    record.push_back(ctl::cr);
    std::uint8_t fn = 1;
    const auto append = [&stream](const Frame& f) {
        const auto raw = encode(f);
        stream.insert(stream.end(), raw.begin(), raw.end());
    };
    for (const auto& f : split(record, fn)) {
        append(f);
        if (f.number == 2) {
            append(f);  // retransmission after a lost ACK
        }
        fn = next_frame_number(f.number);
    }
    append(Frame{.number = fn, .last = true, .text = to_bytes("L|1|N\r")});
    stream.push_back(ctl::eot);

    const auto capture = deframe(stream);
    CHECK(capture.problems.empty());
    REQUIRE(capture.records.size() == 2);
    CHECK(capture.records[0] == long_record);
    CHECK(to_string(capture.records[1]) == "L|1|N");

    SECTION("corrupted frame is reported") {
        stream[10] = std::byte{'#'};
        const auto broken = deframe(stream);
        CHECK_FALSE(broken.problems.empty());
    }
}

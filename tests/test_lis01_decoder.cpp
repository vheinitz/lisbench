#include <catch2/catch_test_macros.hpp>

#include "lisbench/lis01_decoder.hpp"

using namespace lisbench;
using namespace lisbench::lis01;

namespace {

template <typename T>
std::vector<T> of(const std::vector<DecoderEvent>& events) {
    std::vector<T> out;
    for (const auto& e : events) {
        if (const auto* p = std::get_if<T>(&e)) {
            out.push_back(*p);
        }
    }
    return out;
}

bytes frame(std::uint8_t n, std::string_view text, bool last = true) {
    return encode(Frame{.number = n, .last = last, .text = to_bytes(text)});
}

}  // namespace

TEST_CASE("decoder reports handshake, frames, records and noise in order") {
    StreamDecoder d;
    std::vector<DecoderEvent> ev;
    bytes s{ctl::enq};
    for (const auto& f : {frame(1, "H|\\^&\r"), frame(2, "L|1|N\r")}) {
        s.insert(s.end(), f.begin(), f.end());
    }
    s.push_back(std::byte{'x'});
    s.push_back(std::byte{'y'});
    s.push_back(ctl::eot);
    // Fed byte by byte: chunk boundaries must not matter.
    for (const auto b : s) {
        d.feed(bytes_view{&b, 1}, ev);
    }
    d.finish(ev);
    CHECK(of<dec::Control>(ev).size() == 2);
    CHECK(of<dec::FrameOk>(ev).size() == 2);
    REQUIRE(of<dec::Record>(ev).size() == 2);
    CHECK(to_string(of<dec::Record>(ev)[1].text) == "L|1|N");
    REQUIRE(of<dec::Noise>(ev).size() == 1);
    CHECK(of<dec::Noise>(ev)[0].data.size() == 2);
    CHECK(of<dec::Problem>(ev).empty());
    CHECK(std::holds_alternative<dec::Control>(ev.front()));
    CHECK(std::holds_alternative<dec::Control>(ev.back()));
}

TEST_CASE("decoder flags retransmissions, bad frames and broken records") {
    StreamDecoder d;
    std::vector<DecoderEvent> ev;
    const auto f1 = frame(1, "R|1|^^^A|", false);
    auto bad = frame(2, "1\r");
    bad[3] = std::byte{'9'};  // checksum no longer matches
    const auto f2 = frame(2, "1\r");
    for (const auto& part : {bytes{ctl::enq}, f1, f1, bad, f2, bytes{ctl::eot}}) {
        d.feed(part, ev);
    }
    const auto ok = of<dec::FrameOk>(ev);
    REQUIRE(ok.size() == 3);
    CHECK_FALSE(ok[0].repeated);
    CHECK(ok[1].repeated);
    CHECK(of<dec::FrameBad>(ev).size() == 1);
    REQUIRE(of<dec::Record>(ev).size() == 1);
    CHECK(to_string(of<dec::Record>(ev)[0].text) == "R|1|^^^A|1");

    SECTION("EOT inside a record") {
        std::vector<DecoderEvent> e2;
        StreamDecoder d2;
        d2.feed(bytes{ctl::enq}, e2);
        d2.feed(f1, e2);
        d2.feed(bytes{ctl::eot}, e2);
        CHECK(of<dec::Record>(e2).empty());
        CHECK(of<dec::Problem>(e2).size() == 1);
    }
    SECTION("unfinished frame at the end") {
        std::vector<DecoderEvent> e2;
        StreamDecoder d2;
        d2.feed(bytes(f1.begin(), f1.begin() + 5), e2);
        d2.finish(e2);
        CHECK(of<dec::FrameBad>(e2).size() == 1);
    }
}

TEST_CASE("decoder notes restricted characters in frame text but keeps the frame") {
    StreamDecoder d;
    std::vector<DecoderEvent> ev;
    bytes s{ctl::enq};
    const auto f = frame(1,
                         "C|1|I|a\nb\x11"
                         "c\r");
    s.insert(s.end(), f.begin(), f.end());
    d.feed(s, ev);
    const auto problems = of<dec::Problem>(ev);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].text.starts_with("restricted character 0x0A"));
    CHECK(of<dec::FrameOk>(ev).size() == 1);
    CHECK(of<dec::Record>(ev).size() == 1);
}

TEST_CASE("restricted characters of LIS01-A2") {
    for (const unsigned c : {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x0AU, 0x10U, 0x11U, 0x12U,
                             0x13U, 0x14U, 0x15U, 0x16U, 0x17U}) {
        CHECK(is_restricted(std::byte(c)));
    }
    for (const unsigned c : {0x09U, 0x0DU, 0x20U, 0x41U, 0x7CU, 0xE9U}) {
        CHECK_FALSE(is_restricted(std::byte(c)));
    }
    CHECK(first_restricted(to_bytes("abc")) == std::nullopt);
    CHECK(first_restricted(to_bytes("ab\x05"
                                    "c")) == 2U);
}

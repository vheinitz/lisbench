// Randomised inputs with the same invariants as the libFuzzer targets in
// fuzz/, so they are also checked in every normal test run.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <map>
#include <random>
#include <string>

#include "lisbench/lis01_frame.hpp"
#include "lisbench/lis01_link.hpp"
#include "lisbench/lis02_message.hpp"

using namespace lisbench;

namespace {

// Biased towards characters that matter: delimiters, escapes, record types,
// control bytes, 8-bit bytes.
std::string random_text(std::mt19937& rng, std::size_t max_len) {
    static constexpr std::string_view alphabet =
        "HPORCQLMS|\\^&!@#$FSREX0123456789AF \r\n\x02\x03\x17\x04\x05\xE9\xFC";
    std::uniform_int_distribution<std::size_t> len{0, max_len};
    std::uniform_int_distribution<std::size_t> pick{0, alphabet.size() - 1};
    std::uniform_int_distribution<int> any{0, 255};
    std::bernoulli_distribution random_byte{0.05};
    std::string out(len(rng), '\0');
    for (auto& c : out) {
        c = random_byte(rng) ? static_cast<char>(any(rng)) : alphabet[pick(rng)];
    }
    return out;
}

}  // namespace

TEST_CASE("robustness: parse -> serialize -> parse is stable") {
    std::mt19937 rng{20261025};
    for (int i = 0; i < 20'000; ++i) {
        const auto text =
            (i % 3 == 0 ? std::string{"H|\\^&|"} : std::string{}) + random_text(rng, 120);
        const auto message = lis02::parse_text(text);
        if (!message) {
            continue;
        }
        (void)lis02::validate(*message);
        const auto lines = lis02::serialize(*message);
        if (!lines) {
            continue;
        }
        const auto again = lis02::parse(*lines);
        INFO("input: " << text);
        INFO("serialized first: " << (lines->empty() ? std::string{} : lines->front()));
        INFO("error: " << (again ? std::string{} : again.error().text));
        REQUIRE(again);
        REQUIRE(again->records.size() == message->records.size());
        for (std::size_t r = 0; r < message->records.size(); ++r) {
            auto a = message->records[r];
            auto b = again->records[r];
            a.trim();
            b.trim();
            INFO(text);
            REQUIRE(a == b);
        }
    }
}

TEST_CASE("robustness: deframe output survives framing again") {
    std::mt19937 rng{4711};
    for (int i = 0; i < 5'000; ++i) {
        // Mix valid frames with noise so that deframe actually finds records.
        bytes stream;
        lis01::Message m;
        m.records.push_back(to_bytes(random_text(rng, 600)));
        for (const auto& f : lis01::frames_for(m)) {
            const auto raw = lis01::encode(f);
            stream.insert(stream.end(), raw.begin(), raw.end());
        }
        const auto noise = to_bytes(random_text(rng, 40));
        const auto at = std::uniform_int_distribution<std::size_t>{0, stream.size()}(rng);
        stream.insert(stream.begin() + static_cast<std::ptrdiff_t>(at), noise.begin(), noise.end());

        const auto capture = lis01::deframe(stream);
        bytes again_stream;
        for (const auto& f : lis01::frames_for(lis01::Message{capture.records})) {
            const auto raw = lis01::encode(f);
            again_stream.insert(again_stream.end(), raw.begin(), raw.end());
        }
        REQUIRE(lis01::deframe(again_stream).records == capture.records);
    }
}

TEST_CASE("robustness: a link fed with garbage reports every message exactly once") {
    std::mt19937 rng{99};
    std::uniform_int_distribution<int> any{0, 255};
    for (int run = 0; run < 2'000; ++run) {
        lis01::Link link{run % 2 == 0 ? lis01::Role::instrument : lis01::Role::computer};
        lis01::Events ev;
        std::map<lis01::MessageId, int> results;
        lis01::TimerId timer = 0;
        std::vector<lis01::MessageId> ids;
        const auto drain = [&] {
            for (const auto& e : ev) {
                if (const auto* f = std::get_if<lis01::ev::SendFinished>(&e)) {
                    ++results[f->id];
                } else if (const auto* t = std::get_if<lis01::ev::ArmTimer>(&e)) {
                    timer = t->id;
                }
            }
            ev.clear();
        };
        const lis01::Message msg{{to_bytes("H|\\^&"), to_bytes("L|1|N")}};
        ids.push_back(link.submit(msg, ev));
        drain();
        for (int step = 0; step < 200; ++step) {
            const auto b = any(rng);
            if (b > 250) {
                link.on_timer(timer, ev);
            } else if (b > 247) {
                ids.push_back(link.submit(msg, ev));
            } else if (b > 245) {
                link.on_disconnect(ev);
                link.resume(ev);
            } else {
                // Mostly protocol bytes, so that transfers make progress.
                static constexpr std::array control{0x05, 0x06, 0x15, 0x04, 0x02,
                                                    0x03, 0x0D, 0x0A, 0x31};
                const auto value =
                    b < 200 ? control.at(static_cast<std::size_t>(b) % control.size()) : b;
                const auto byte = static_cast<std::byte>(value);
                link.on_bytes(bytes_view{&byte, 1}, ev);
            }
            drain();
        }
        link.cancel_all(ev);
        drain();
        for (const auto id : ids) {
            REQUIRE(results[id] == 1);
        }
    }
}

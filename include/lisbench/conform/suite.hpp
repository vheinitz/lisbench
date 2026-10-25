#pragma once

// LIS01-A2 conformance test suite: test cases, verdicts and the report.
// Terms after ISO/IEC 9646: the IUT (implementation under test) is checked
// by a tester that plays the other role; each test case has a test purpose
// and ends with a verdict. See docs/spec/features.md, section 7.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lisbench::conform {

enum class Verdict : std::uint8_t { pass, fail, inconclusive };
enum class Level : std::uint8_t { shall, should };

[[nodiscard]] std::string_view to_string(Verdict v) noexcept;
[[nodiscard]] std::string_view to_string(Level l) noexcept;

struct TestCase {
    std::string_view id;
    std::string_view purpose;
    Level level;
    bool iut_sends;  // TS: the IUT is the sender
};

// A table: four columns read better by position than with designators.
// NOLINTBEGIN(modernize-use-designated-initializers)
inline constexpr auto suite = std::to_array<TestCase>({
    {"TR01", "accepts a valid message, every frame answered with ACK", Level::shall, false},
    {"TR02", "rejects a frame with a wrong checksum with NAK", Level::shall, false},
    {"TR03", "rejects a frame with an unexpected frame number with NAK", Level::shall, false},
    {"TR04", "accepts a repeated frame (lost ACK) with ACK", Level::shall, false},
    {"TR05", "accepts ETB frames, 240 characters of text and frame numbers 7 -> 0", Level::shall,
     false},
    {"TR06", "answers ENQ and frames within the sender's reply timeout", Level::shall, false},
    {"TR07", "rejects a frame with a restricted character (LF) in its text", Level::should, false},
    {"TS01", "sends well-formed frames (checksum, numbers, length, ETB/ETX, CR LF)", Level::shall,
     true},
    {"TS02", "repeats a frame answered with NAK, with the same frame number", Level::shall, true},
    {"TS03", "gives up with EOT after six transmissions of a refused frame", Level::shall, true},
    {"TS04", "gives up with EOT when a frame is not answered in time", Level::shall, true},
    {"TS05", "waits the busy time before the next ENQ after ENQ was answered with NAK",
     Level::shall, true},
    {"TS06", "resolves contention (ENQ answered with ENQ) by its role", Level::shall, true},
    {"TS07", "handles a receiver interrupt (frame answered with EOT) as permitted", Level::shall,
     true},
});
// NOLINTEND(modernize-use-designated-initializers)

[[nodiscard]] const TestCase* find(std::string_view id) noexcept;

struct Result {
    std::string id{};
    Verdict verdict{Verdict::inconclusive};
    std::string detail{};  // what was observed
    std::optional<std::chrono::milliseconds> measured{};
};

// Plain-text report: one line per case, then the counts.
[[nodiscard]] std::vector<std::string> report(const std::vector<Result>& results);

// A measured duration against a nominal one: passes from 95 % of nominal and,
// when `upper` is given, up to that factor of nominal.
[[nodiscard]] bool within(std::chrono::milliseconds measured, std::chrono::milliseconds nominal,
                          std::optional<double> upper = std::nullopt) noexcept;

}  // namespace lisbench::conform

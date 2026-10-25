#pragma once

// Fault injection on the wire, applied to what a link sends. Working at this
// level keeps the link itself correct and makes the other side see exactly
// what a faulty partner would produce:
//   ack -> nak       the frame is refused; the sender repeats it, the link
//                    on this side recognises the repetition and drops it
//   frame corrupt    wrong checksum; the receiver must answer NAK
//   drop             the bytes never arrive; the other side must time out
//   delay            the bytes arrive late (e.g. after the reply timeout)

#include <array>
#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/bytes.hpp"

namespace lisbench::io {

using std::chrono::milliseconds;

enum class FaultTarget : std::uint8_t { ack, nak, enq, eot, frame };
enum class FaultAction : std::uint8_t { drop, nak, corrupt, delay };

struct Fault {
    FaultTarget target{FaultTarget::frame};
    FaultAction action{FaultAction::drop};
    std::size_t nth{0};    // only the n-th occurrence (1-based) ...
    std::size_t every{0};  // ... or every k-th; neither: every occurrence
    milliseconds delay{0};
};

[[nodiscard]] std::optional<FaultTarget> fault_target(std::string_view name) noexcept;
[[nodiscard]] std::optional<FaultAction> fault_action(std::string_view name) noexcept;
[[nodiscard]] std::string_view to_string(FaultTarget t) noexcept;
[[nodiscard]] std::string_view to_string(FaultAction a) noexcept;

// Command-line form "on:which:do[:ms]": "ack:2:nak", "frame:every3:corrupt",
// "ack:all:delay:20000".
[[nodiscard]] std::expected<Fault, std::string> parse_fault(std::string_view spec);

struct Outgoing {
    bytes data{};
    milliseconds delay{0};
    std::optional<Fault> applied{};
};

class FaultFilter {
public:
    FaultFilter() = default;
    explicit FaultFilter(std::vector<Fault> faults);

    // One unit as the link emits it (a control byte or a whole frame).
    // nullopt: dropped.
    [[nodiscard]] std::optional<Outgoing> apply(bytes data);

    [[nodiscard]] static std::optional<FaultTarget> classify(bytes_view data) noexcept;

private:
    std::vector<Fault> faults_{};
    std::array<std::size_t, 5> seen_{};
};

}  // namespace lisbench::io

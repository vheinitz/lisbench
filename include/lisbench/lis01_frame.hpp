#pragma once

// CLSI LIS01-A2 (formerly ASTM E1381): frame layout and checksum.
//
//   <STX> FN text <ETB|ETX> C1 C2 <CR> <LF>
//
// FN is the frame number '0'..'7'; the first frame after ENQ is '1'. The
// checksum is the sum of all bytes from FN up to and including ETB/ETX,
// modulo 256, as two upper-case hex digits. A frame holds at most 240 text
// characters, 247 characters in total. ETX ends the last frame of a record,
// ETB an intermediate one.

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/bytes.hpp"

namespace lisbench::lis01 {

namespace ctl {
inline constexpr std::byte stx{0x02};
inline constexpr std::byte etx{0x03};
inline constexpr std::byte eot{0x04};
inline constexpr std::byte enq{0x05};
inline constexpr std::byte ack{0x06};
inline constexpr std::byte lf{0x0A};
inline constexpr std::byte cr{0x0D};
inline constexpr std::byte nak{0x15};
inline constexpr std::byte etb{0x17};
}  // namespace ctl

inline constexpr std::size_t max_text_size = 240;
inline constexpr std::size_t frame_overhead = 7;  // STX FN ETX C1 C2 CR LF
inline constexpr std::size_t max_frame_size = max_text_size + frame_overhead;

// Sum modulo 256 over FN .. ETB/ETX inclusive.
[[nodiscard]] constexpr std::uint8_t checksum(bytes_view fn_to_terminator) noexcept {
    return std::ranges::fold_left(
        fn_to_terminator, std::uint8_t{0}, [](std::uint8_t sum, std::byte b) {
            return static_cast<std::uint8_t>(sum + std::to_integer<std::uint8_t>(b));
        });
}

[[nodiscard]] constexpr std::uint8_t next_frame_number(std::uint8_t fn) noexcept {
    return static_cast<std::uint8_t>((fn + 1U) % 8U);
}

struct Frame {
    std::uint8_t number{1};  // 0..7
    bool last{true};         // true: ETX (record complete), false: ETB
    bytes text;

    friend bool operator==(const Frame&, const Frame&) = default;
};

[[nodiscard]] bytes encode(const Frame& frame);

// Splits one record (its text including the terminating CR) into frames of
// at most max_text_size text bytes. Only the final frame carries ETX; an
// empty record still yields one (empty) ETX frame. Frame numbers start at
// first_number and continue modulo 8.
[[nodiscard]] std::vector<Frame> split(bytes_view record_text, std::uint8_t first_number);

enum class FrameError : std::uint8_t {
    too_short,
    no_stx,
    bad_frame_number,
    no_terminator,
    bad_checksum_digits,
    checksum_mismatch,
    no_crlf,
    text_too_long,
};

[[nodiscard]] std::string_view to_string(FrameError e) noexcept;

// Decodes exactly one complete frame. The input must start with STX and end
// with LF. max_text can be raised for devices that exceed the 240 limit.
[[nodiscard]] std::expected<Frame, FrameError> decode(bytes_view raw,
                                                      std::size_t max_text = max_text_size);

// Reassembles records from a captured byte stream of one direction (for
// example a sender's output recorded by a tap). Handshake bytes are skipped,
// a repeated frame after a lost ACK is dropped, and every irregularity is
// reported instead of being silently fixed.
struct Capture {
    std::vector<bytes> records;  // without terminating CR
    std::vector<std::string> problems;
};

[[nodiscard]] Capture deframe(bytes_view stream, std::size_t max_text = max_text_size);

}  // namespace lisbench::lis01

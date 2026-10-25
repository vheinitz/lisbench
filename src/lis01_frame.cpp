#include "lisbench/lis01_frame.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <utility>

namespace lisbench::lis01 {

namespace {

constexpr std::array<char, 16> hex_digits{'0', '1', '2', '3', '4', '5', '6', '7',
                                          '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

constexpr std::byte digit_byte(std::uint8_t value) {
    return static_cast<std::byte>(hex_digits.at(value & 0x0FU));
}

// Accepts upper and lower case; the standard mandates upper case on send.
constexpr std::expected<std::uint8_t, FrameError> hex_value(std::byte b) {
    const auto c = std::to_integer<unsigned char>(b);
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    return std::unexpected(FrameError::bad_checksum_digits);
}

}  // namespace

bytes encode(const Frame& frame) {
    const auto n = frame.text.size();
    bytes out(n + frame_overhead);
    out[0] = ctl::stx;
    out[1] = static_cast<std::byte>('0' + (frame.number % 8U));
    std::ranges::copy(frame.text, out.begin() + 2);
    out[n + 2] = frame.last ? ctl::etx : ctl::etb;
    const auto sum = checksum(bytes_view{out}.subspan(1, n + 2));
    out[n + 3] = digit_byte(static_cast<std::uint8_t>(sum >> 4U));
    out[n + 4] = digit_byte(sum);
    out[n + 5] = ctl::cr;
    out[n + 6] = ctl::lf;
    return out;
}

std::vector<Frame> split(bytes_view record_text, std::uint8_t first_number) {
    std::vector<Frame> frames;
    std::uint8_t number = first_number % 8U;
    // Whether a frame is the last one depends only on whether text follows,
    // never on the frame's length: a record of exactly 240 bytes is one ETX
    // frame, not an ETB frame followed by nothing.
    for (auto chunk : record_text | std::views::chunk(max_text_size)) {
        frames.push_back(
            Frame{.number = number, .last = false, .text = bytes(chunk.begin(), chunk.end())});
        number = next_frame_number(number);
    }
    if (frames.empty()) {
        frames.push_back(Frame{.number = number, .last = false, .text = {}});
    }
    frames.back().last = true;
    return frames;
}

std::string_view to_string(FrameError e) noexcept {
    switch (e) {
        case FrameError::too_short: return "frame too short";
        case FrameError::no_stx: return "frame does not start with STX";
        case FrameError::bad_frame_number: return "frame number is not a digit 0..7";
        case FrameError::no_terminator: return "no ETX/ETB before checksum";
        case FrameError::bad_checksum_digits: return "checksum is not two hex digits";
        case FrameError::checksum_mismatch: return "checksum mismatch";
        case FrameError::no_crlf: return "frame does not end with CR LF";
        case FrameError::text_too_long: return "frame text longer than allowed";
    }
    return "unknown frame error";
}

std::expected<Frame, FrameError> decode(bytes_view raw, std::size_t max_text) {
    // STX FN <text> ETX C1 C2 CR LF: at least 7 bytes.
    if (raw.size() < frame_overhead) {
        return std::unexpected(FrameError::too_short);
    }
    if (raw.front() != ctl::stx) {
        return std::unexpected(FrameError::no_stx);
    }
    const auto n = raw.size();
    if (raw[n - 2] != ctl::cr || raw[n - 1] != ctl::lf) {
        return std::unexpected(FrameError::no_crlf);
    }
    const auto terminator = raw[n - 5];
    if (terminator != ctl::etx && terminator != ctl::etb) {
        return std::unexpected(FrameError::no_terminator);
    }
    const auto fn = std::to_integer<unsigned char>(raw[1]);
    if (fn < '0' || fn > '7') {
        return std::unexpected(FrameError::bad_frame_number);
    }

    const auto hi = hex_value(raw[n - 4]);
    const auto lo = hex_value(raw[n - 3]);
    if (!hi || !lo) {
        return std::unexpected(FrameError::bad_checksum_digits);
    }
    const auto expected_sum = static_cast<std::uint8_t>((*hi << 4U) | *lo);
    if (checksum(raw.subspan(1, n - 5)) != expected_sum) {
        return std::unexpected(FrameError::checksum_mismatch);
    }

    const auto text = raw.subspan(2, n - frame_overhead);
    if (text.size() > max_text) {
        return std::unexpected(FrameError::text_too_long);
    }
    return Frame{.number = static_cast<std::uint8_t>(fn - '0'),
                 .last = terminator == ctl::etx,
                 .text = bytes(text.begin(), text.end())};
}

Capture deframe(bytes_view stream, std::size_t max_text) {
    Capture out;
    bytes record;
    std::optional<Frame> previous;
    std::uint8_t expected = 1;
    for (std::size_t i = 0; i < stream.size();) {
        const auto b = stream[i];
        if (b == ctl::enq) {
            expected = 1;
            previous.reset();
            ++i;
            continue;
        }
        if (b == ctl::eot) {
            if (!record.empty()) {
                out.problems.push_back(std::format("offset {}: EOT inside a record", i));
                record.clear();
            }
            ++i;
            continue;
        }
        if (b != ctl::stx) {
            ++i;  // ACK, NAK and line noise
            continue;
        }
        // Frame end: terminator followed by C1 C2 CR LF.
        std::size_t end = i + 1;
        while (end < stream.size() && stream[end] != ctl::etx && stream[end] != ctl::etb &&
               stream[end] != ctl::stx) {
            ++end;
        }
        if (end >= stream.size() || stream[end] == ctl::stx || end + 4 >= stream.size()) {
            out.problems.push_back(std::format("offset {}: incomplete frame", i));
            i = end;
            continue;
        }
        const auto raw = stream.subspan(i, end + 5 - i);
        auto frame = decode(raw, max_text);
        if (!frame) {
            out.problems.push_back(std::format("offset {}: {}", i, to_string(frame.error())));
            i = end + 5;
            continue;
        }
        i = end + 5;
        if (previous && *previous == *frame) {
            continue;  // retransmission
        }
        if (frame->number != expected) {
            out.problems.push_back(std::format("offset {}: frame number {} where {} was expected",
                                               i - raw.size(), frame->number, expected));
        }
        expected = next_frame_number(frame->number);
        record.insert(record.end(), frame->text.begin(), frame->text.end());
        if (frame->last) {
            if (!record.empty() && record.back() == ctl::cr) {
                record.pop_back();
            } else {
                out.problems.push_back(
                    std::format("offset {}: record does not end with CR", i - raw.size()));
            }
            out.records.push_back(std::exchange(record, {}));
        }
        previous = std::move(frame).value();
    }
    if (!record.empty()) {
        out.problems.emplace_back("stream ends inside a record");
    }
    return out;
}

}  // namespace lisbench::lis01

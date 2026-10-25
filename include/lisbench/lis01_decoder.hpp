#pragma once

// Passive LIS01-A2 decoder for one direction of a link: what a monitor or a
// tap sees. Unlike lis01::Link it never answers; it reports what went over
// the wire, including everything that is wrong with it.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "lisbench/bytes.hpp"
#include "lisbench/lis01_frame.hpp"

namespace lisbench::lis01 {

namespace dec {
struct Control {  // ENQ, ACK, NAK, EOT
    std::size_t offset{};
    std::byte value{};
};
struct FrameOk {
    std::size_t offset{};
    Frame frame{};
    bool repeated{};  // same as the previous frame: a retransmission
};
struct FrameBad {
    std::size_t offset{};
    FrameError error{};
    bytes raw{};
};
struct Record {  // complete, without terminating CR
    std::size_t offset{};
    bytes text{};
};
struct Noise {  // bytes outside frames that are not handshake characters
    std::size_t offset{};
    bytes data{};
};
struct Problem {
    std::size_t offset{};
    std::string text{};
};
}  // namespace dec

using DecoderEvent =
    std::variant<dec::Control, dec::FrameOk, dec::FrameBad, dec::Record, dec::Noise, dec::Problem>;

class StreamDecoder {
public:
    explicit StreamDecoder(std::size_t max_text = max_text_size) : max_text_{max_text} {}

    void feed(bytes_view data, std::vector<DecoderEvent>& out);
    // End of stream: reports an unfinished frame or record.
    void finish(std::vector<DecoderEvent>& out);

private:
    void on_byte(std::byte b, std::vector<DecoderEvent>& out);
    void complete_frame(std::vector<DecoderEvent>& out);
    void flush_noise(std::vector<DecoderEvent>& out);

    std::size_t max_text_;
    std::size_t offset_{0};
    bytes frame_{};
    std::size_t frame_start_{0};
    std::optional<std::size_t> terminator_at_{};
    bytes noise_{};
    std::size_t noise_start_{0};
    bytes record_{};
    std::size_t record_start_{0};
    std::optional<Frame> previous_{};
    std::uint8_t expected_{1};
};

// Reassembles records from a captured byte stream of one direction (for
// example a sender's output recorded by a tap). Handshake bytes are skipped,
// a repeated frame after a lost ACK is dropped, and every irregularity is
// reported instead of being silently fixed.
struct Capture {
    std::vector<bytes> records{};  // without terminating CR
    std::vector<std::string> problems{};
};

[[nodiscard]] Capture deframe(bytes_view stream, std::size_t max_text = max_text_size);

}  // namespace lisbench::lis01

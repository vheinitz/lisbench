#pragma once

// LIS02-A2 messages on a byte stream without the LIS01-A2 low-level protocol:
// no ENQ/ACK/EOT, no frames, no checksum. Seen on TCP, where the stream is
// already reliable. Two forms are common:
//   none   records, each ended by CR; the message ends with its L record
//   mllp   the message in an HL7 MLLP block: VT (0x0B) ... FS (0x1C) CR
// Neither acknowledges anything: a message counts as delivered once it is
// handed to the stream. Like lis01::Link this is a state machine without I/O,
// and it speaks the same events, so a driver can run either.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "lisbench/bytes.hpp"
#include "lisbench/lis01_link.hpp"

namespace lisbench {

// How a link carries its messages.
enum class LowLevel : std::uint8_t { lis01_a2, none, mllp };

[[nodiscard]] std::string_view to_string(LowLevel l) noexcept;
// "lis01-a2", "none", "mllp".
[[nodiscard]] std::optional<LowLevel> low_level_from(std::string_view name) noexcept;

namespace mllp {
inline constexpr std::byte start_block{0x0B};  // VT
inline constexpr std::byte end_block{0x1C};    // FS, followed by CR
}  // namespace mllp

// The bytes of a message on the wire: records with CR, in an MLLP block for
// LowLevel::mllp. Not for lis01_a2 (see lis01::frames_for).
[[nodiscard]] bytes encode_unframed(LowLevel low_level, const lis01::Message& message);

namespace unframed {
struct Record {  // complete, without terminating CR
    std::size_t offset{};
    bytes text{};
};
struct MessageEnd {
    std::size_t offset{};
    bool complete{};  // none: ended by its L record; mllp: by FS
    std::string reason{};
};
struct Noise {  // mllp: bytes outside a block
    std::size_t offset{};
    bytes data{};
};
struct Problem {
    std::size_t offset{};
    std::string text{};
};
}  // namespace unframed

// "3 bytes outside an MLLP block", with a hint when they look like LIS01-A2.
[[nodiscard]] std::string describe_noise(bytes_view noise);

using UnframedEvent =
    std::variant<unframed::Record, unframed::MessageEnd, unframed::Noise, unframed::Problem>;

// Passive decoder for one direction, used by the link and by the monitor.
class UnframedDecoder {
public:
    explicit UnframedDecoder(LowLevel low_level) : low_level_{low_level} {}

    // Bytes outside an MLLP block are reported at the end of each call.
    void feed(bytes_view data, std::vector<UnframedEvent>& out);
    // End of stream: reports an unfinished record or message.
    void finish(std::vector<UnframedEvent>& out);
    // Drops an unfinished message (receive timeout) and reports it.
    void abandon(std::string reason, std::vector<UnframedEvent>& out);

    [[nodiscard]] bool in_message() const noexcept { return records_ > 0 || !record_.empty(); }

    // Longest record kept; longer ones are reported and dropped.
    static constexpr std::size_t max_record{1U << 20U};

private:
    void on_byte(std::byte b, std::vector<UnframedEvent>& out);
    void end_record(std::vector<UnframedEvent>& out);
    void end_message(bool complete, std::string reason, std::vector<UnframedEvent>& out);
    void flush_noise(std::vector<UnframedEvent>& out);

    LowLevel low_level_;
    std::size_t offset_{0};
    bytes record_{};
    std::size_t record_start_{0};
    std::size_t records_{0};  // in the current message
    bool too_long_{false};
    bool after_cr_{false};    // none: an LF right after CR is part of the line end
    bool in_block_{false};    // mllp
    bool after_fs_{false};    // mllp: CR expected
    bool told_lis01_{false};  // none: control characters reported once per message
    bytes noise_{};
    std::size_t noise_start_{0};
};

// Sending and receiving messages without LIS01-A2. receive_timeout of the
// timing ends a message that stops arriving; the other values do not apply.
class UnframedLink {
public:
    explicit UnframedLink(LowLevel low_level, lis01::Timing timing = {});

    [[nodiscard]] lis01::MessageId submit(const lis01::Message& message, lis01::Events& out);
    void on_bytes(bytes_view data, lis01::Events& out);
    void on_timer(lis01::TimerId id, lis01::Events& out);
    void on_disconnect(lis01::Events& out);
    void resume(lis01::Events& out);
    void cancel(lis01::MessageId id, lis01::Events& out);
    void cancel_all(lis01::Events& out);
    // Without a handshake there is no way to refuse a message.
    void set_receiver_ready(bool /*ready*/) noexcept {}

    [[nodiscard]] lis01::State state() const noexcept {
        return decoder_.in_message() ? lis01::State::receiving : lis01::State::neutral;
    }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }

private:
    struct Queued {
        lis01::MessageId id;
        bytes data;
    };

    void resume_sending(lis01::Events& out);
    static void forward(std::vector<UnframedEvent>& decoded, lis01::Events& out);
    void watch_timer(lis01::Events& out);

    LowLevel low_level_;
    lis01::Timing timing_;
    UnframedDecoder decoder_;
    bool connected_{true};
    std::deque<Queued> queue_{};
    lis01::MessageId next_id_{1};
    lis01::TimerId armed_{0};
    lis01::TimerId next_timer_{1};
};

}  // namespace lisbench

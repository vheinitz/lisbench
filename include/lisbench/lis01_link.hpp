#pragma once

// CLSI LIS01-A2 data link layer as a state machine without I/O.
//
// The link is fed with received bytes, timer expiries and connection changes,
// and answers with events: bytes to send, timers to arm, finished transfers,
// received records. The driver (TCP, serial, simulator, test) owns the real
// I/O and the clock. This keeps every timeout testable without sleeping.
//
// Phases per LIS01-A2: establishment (ENQ / ACK, NAK, contention), transfer
// (frames, each acknowledged), termination (EOT).

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "lisbench/bytes.hpp"
#include "lisbench/lis01_frame.hpp"

namespace lisbench::lis01 {

using std::chrono::milliseconds;

// Which side of the link this is. Decides who yields on contention: the
// instrument has priority, the computer system (LIS) backs off.
enum class Role : std::uint8_t { instrument, computer };

struct Timing {
    milliseconds reply_timeout{15'000};         // sender: wait for ACK/NAK
    milliseconds receive_timeout{30'000};       // receiver: wait for next frame/EOT
    milliseconds busy_retry{10'000};            // sender: ENQ answered with NAK
    milliseconds contention_instrument{1'000};  // instrument: re-send ENQ after contention
    milliseconds contention_computer{20'000};   // computer: hold off after contention
    int max_frame_attempts{6};                  // transmissions of one frame before abort
    std::size_t max_text{max_text_size};        // accepted frame text on receive
};

using MessageId = std::uint64_t;
using TimerId = std::uint64_t;

// A message is a sequence of records, each without its terminating CR.
struct Message {
    std::vector<bytes> records;
};

enum class Outcome : std::uint8_t {
    delivered,     // every frame acknowledged, EOT sent
    rejected,      // a frame was refused max_frame_attempts times
    timeout,       // no reply within reply_timeout
    disconnected,  // connection lost during the transfer
    cancelled,     // cancelled by the caller
};

[[nodiscard]] std::string_view to_string(Outcome o) noexcept;

namespace ev {
struct Send {
    bytes data;
};
// Arm the single link timer; replaces any armed timer.
struct ArmTimer {
    TimerId id;
    milliseconds after;
};
struct CancelTimer {};
// Emitted exactly once for every submitted message.
struct SendFinished {
    MessageId id;
    Outcome outcome;
    bool interrupt_requested;  // receiver answered a frame with EOT
};
struct RecordReceived {
    bytes record;  // without terminating CR
};
struct ReceiveFinished {
    bool complete;  // ended by EOT
    std::string reason;
};
// Protocol deviation worth logging; the link continues.
struct Warning {
    std::string text;
};
}  // namespace ev

using Event = std::variant<ev::Send, ev::ArmTimer, ev::CancelTimer, ev::SendFinished,
                           ev::RecordReceived, ev::ReceiveFinished, ev::Warning>;
using Events = std::vector<Event>;

enum class State : std::uint8_t {
    neutral,
    establishing,     // ENQ sent, waiting for reply
    busy_wait,        // ENQ refused, waiting busy_retry
    contention_wait,  // instrument after contention, waiting to re-send ENQ
    sending,          // frame sent, waiting for reply
    receiving,
};

[[nodiscard]] std::string_view to_string(State s) noexcept;

// Encodes a message the way it appears on the wire after a successful
// handshake (frames only, no ENQ/EOT). Frame numbers start at 1.
[[nodiscard]] std::vector<Frame> frames_for(const Message& message);

class Link {
public:
    explicit Link(Role role, Timing timing = {});

    // Queues a message. Sending starts as soon as the link is neutral.
    [[nodiscard]] MessageId submit(const Message& message, Events& out);

    void on_bytes(bytes_view data, Events& out);
    void on_timer(TimerId id, Events& out);

    // Connection lost: the running transfer ends as disconnected, queued
    // messages stay queued until resume().
    void on_disconnect(Events& out);
    void resume(Events& out);

    // Cancels one message (queued or running) or all of them. Every affected
    // message gets its SendFinished. Call cancel_all before discarding a link.
    void cancel(MessageId id, Events& out);
    void cancel_all(Events& out);

    // Receiver side: answer ENQ with NAK while not ready.
    void set_receiver_ready(bool ready) noexcept { receiver_ready_ = ready; }

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }

private:
    struct Outgoing {
        MessageId id;
        std::vector<bytes> frames;  // encoded
        std::size_t index{0};
        int attempts{0};
        bool interrupt_requested{false};
    };

    Outgoing& active();
    void on_byte(std::byte b, Events& out);
    void on_byte_receiving(std::byte b, Events& out);
    void on_frame(Events& out);

    void kick(Events& out);
    void send_current_frame(Events& out);
    void finish(Outcome outcome, Events& out);
    void end_receive(bool complete, std::string reason, Events& out);
    void arm(milliseconds after, Events& out);
    void disarm(Events& out);
    static void send(std::byte b, Events& out);

    Role role_;
    Timing timing_;
    State state_{State::neutral};
    bool connected_{true};
    bool receiver_ready_{true};
    bool holdoff_{false};  // computer after contention

    std::deque<Outgoing> queue_;
    std::optional<Outgoing> current_;
    MessageId next_id_{1};

    TimerId armed_{0};
    TimerId next_timer_{1};

    // Receiving.
    bytes frame_buf_;
    std::optional<std::size_t> terminator_at_;
    std::uint8_t expected_fn_{1};
    bytes record_buf_;
};

}  // namespace lisbench::lis01

#include "lisbench/lis01_link.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <utility>

namespace lisbench::lis01 {

std::string_view to_string(Outcome o) noexcept {
    switch (o) {
        case Outcome::delivered: return "delivered";
        case Outcome::rejected: return "rejected";
        case Outcome::timeout: return "timeout";
        case Outcome::disconnected: return "disconnected";
        case Outcome::cancelled: return "cancelled";
    }
    return "unknown";
}

std::string_view to_string(State s) noexcept {
    switch (s) {
        case State::neutral: return "neutral";
        case State::establishing: return "establishing";
        case State::busy_wait: return "busy_wait";
        case State::contention_wait: return "contention_wait";
        case State::sending: return "sending";
        case State::receiving: return "receiving";
    }
    return "unknown";
}

std::vector<Frame> frames_for(const Message& message) {
    std::vector<Frame> frames;
    std::uint8_t fn = 1;
    for (const auto& record : message.records) {
        bytes text = record;
        text.push_back(ctl::cr);
        auto part = split(text, fn);
        fn = next_frame_number(part.back().number);
        frames.insert(frames.end(), std::make_move_iterator(part.begin()),
                      std::make_move_iterator(part.end()));
    }
    return frames;
}

Link::Link(Role role, Timing timing) : role_{role}, timing_{timing} {}

MessageId Link::submit(const Message& message, Events& out) {
    Outgoing job{.id = next_id_++, .frames = {}};
    for (const auto& frame : frames_for(message)) {
        job.frames.push_back(encode(frame));
    }
    const auto id = job.id;
    queue_.push_back(std::move(job));
    kick(out);
    return id;
}

void Link::on_bytes(bytes_view data, Events& out) {
    for (const auto b : data) {
        on_byte(b, out);
    }
}

void Link::send(std::byte b, Events& out) {
    out.emplace_back(ev::Send{bytes{b}});
}

void Link::arm(milliseconds after, Events& out) {
    armed_ = next_timer_++;
    out.emplace_back(ev::ArmTimer{.id = armed_, .after = after});
}

void Link::disarm(Events& out) {
    if (armed_ != 0) {
        armed_ = 0;
        out.emplace_back(ev::CancelTimer{});
    }
}

void Link::kick(Events& out) {
    if (state_ != State::neutral || !connected_ || holdoff_ || current_ || queue_.empty()) {
        return;
    }
    current_ = std::move(queue_.front());
    queue_.pop_front();
    send(ctl::enq, out);
    state_ = State::establishing;
    arm(timing_.reply_timeout, out);
}

// States establishing and sending always have a running transfer; reaching
// this without one is a bug in the state machine, not a protocol event.
Link::Outgoing& Link::active() {
    if (!current_) {
        throw std::logic_error(
            std::format("lis01::Link: no transfer in state {}", to_string(state_)));
    }
    return *current_;
}

void Link::send_current_frame(Events& out) {
    auto& job = active();
    ++job.attempts;
    out.emplace_back(ev::Send{job.frames.at(job.index)});
    state_ = State::sending;
    arm(timing_.reply_timeout, out);
}

// The only place that reports a send result, and it consumes current_, so a
// message cannot be reported twice or not at all once it has started.
void Link::finish(Outcome outcome, Events& out) {
    if (!current_) {
        return;
    }
    out.emplace_back(ev::SendFinished{.id = current_->id,
                                      .outcome = outcome,
                                      .interrupt_requested = current_->interrupt_requested});
    current_.reset();
    disarm(out);
    state_ = State::neutral;
}

void Link::end_receive(bool complete, std::string reason, Events& out) {
    if (!record_buf_.empty() || !frame_buf_.empty()) {
        out.emplace_back(ev::Warning{"transfer ended inside a record; partial record discarded"});
    }
    frame_buf_.clear();
    record_buf_.clear();
    terminator_at_.reset();
    disarm(out);
    state_ = State::neutral;
    out.emplace_back(ev::ReceiveFinished{.complete = complete, .reason = std::move(reason)});
}

void Link::on_byte(std::byte b, Events& out) {
    switch (state_) {
        case State::receiving: on_byte_receiving(b, out); return;

        case State::neutral:
        case State::busy_wait:
        case State::contention_wait:
            if (b == ctl::enq) {
                // A pending retry yields to the remote side's request.
                if (!receiver_ready_) {
                    send(ctl::nak, out);
                    return;
                }
                disarm(out);
                if (current_ && state_ != State::neutral) {
                    // Keep the message: put it back at the front.
                    queue_.push_front(std::move(*current_));
                    current_.reset();
                }
                holdoff_ = false;
                send(ctl::ack, out);
                state_ = State::receiving;
                expected_fn_ = 1;
                got_frame_ = false;
                arm(timing_.receive_timeout, out);
                return;
            }
            if (b != ctl::eot) {
                out.emplace_back(
                    ev::Warning{std::format("unexpected byte 0x{:02X} in state {}",
                                            std::to_integer<unsigned>(b), to_string(state_))});
            }
            return;

        case State::establishing:
            if (b == ctl::ack) {
                active().attempts = 0;
                send_current_frame(out);
            } else if (b == ctl::nak) {
                state_ = State::busy_wait;
                arm(timing_.busy_retry, out);
            } else if (b == ctl::enq) {
                // Contention: both sides sent ENQ.
                if (role_ != timing_.contention_winner) {
                    queue_.push_front(std::move(active()));
                    current_.reset();
                    state_ = State::neutral;
                    holdoff_ = true;
                    arm(timing_.contention_computer, out);
                } else {
                    state_ = State::contention_wait;
                    arm(timing_.contention_instrument, out);
                }
            } else {
                out.emplace_back(
                    ev::Warning{std::format("unexpected byte 0x{:02X} while waiting for ENQ reply",
                                            std::to_integer<unsigned>(b))});
            }
            return;

        case State::sending: {
            auto& job = active();
            if (b == ctl::ack || b == ctl::eot) {
                if (b == ctl::eot) {
                    // Receiver interrupt request; LIS01-A2 lets the sender continue.
                    job.interrupt_requested = true;
                }
                ++job.index;
                job.attempts = 0;
                if (job.index == job.frames.size()) {
                    send(ctl::eot, out);
                    finish(Outcome::delivered, out);
                    kick(out);
                } else {
                    send_current_frame(out);
                }
            } else if (b == ctl::nak) {
                if (job.attempts >= timing_.max_frame_attempts) {
                    send(ctl::eot, out);
                    finish(Outcome::rejected, out);
                    kick(out);
                } else {
                    send_current_frame(out);
                }
            } else {
                out.emplace_back(ev::Warning{
                    std::format("unexpected byte 0x{:02X} while waiting for frame reply",
                                std::to_integer<unsigned>(b))});
            }
            return;
        }
    }
}

void Link::on_byte_receiving(std::byte b, Events& out) {
    if (frame_buf_.empty()) {
        if (b == ctl::stx) {
            frame_buf_.push_back(b);
        } else if (b == ctl::eot) {
            end_receive(true, "EOT", out);
            kick(out);
        } else if (b == ctl::enq && !got_frame_) {
            // The sender did not see our ACK and starts again.
            send(ctl::ack, out);
            arm(timing_.receive_timeout, out);
        } else {
            out.emplace_back(ev::Warning{std::format("unexpected byte 0x{:02X} between frames",
                                                     std::to_integer<unsigned>(b))});
        }
        return;
    }

    if (b == ctl::stx) {
        out.emplace_back(ev::Warning{"STX inside a frame; previous bytes discarded"});
        frame_buf_.assign(1, b);
        terminator_at_.reset();
        return;
    }
    frame_buf_.push_back(b);
    if (!terminator_at_ && (b == ctl::etx || b == ctl::etb)) {
        terminator_at_ = frame_buf_.size() - 1;
    }
    if (terminator_at_ && frame_buf_.size() == *terminator_at_ + 5) {
        on_frame(out);
        return;
    }
    // A frame that never terminates must not grow without bound.
    if (frame_buf_.size() > timing_.max_text + frame_overhead) {
        out.emplace_back(ev::Warning{"frame exceeds maximum size; rejected"});
        frame_buf_.clear();
        terminator_at_.reset();
        send(ctl::nak, out);
        arm(timing_.receive_timeout, out);
    }
}

void Link::on_frame(Events& out) {
    auto decoded = decode(frame_buf_, timing_.max_text);
    frame_buf_.clear();
    terminator_at_.reset();
    arm(timing_.receive_timeout, out);

    if (!decoded) {
        out.emplace_back(
            ev::Warning{std::format("frame rejected: {}", to_string(decoded.error()))});
        send(ctl::nak, out);
        return;
    }
    auto& frame = *decoded;
    const auto previous = static_cast<std::uint8_t>((expected_fn_ + 7U) % 8U);
    if (frame.number == previous) {
        // Our ACK got lost and the sender repeated the frame.
        out.emplace_back(
            ev::Warning{std::format("duplicate frame {} acknowledged and dropped", frame.number)});
        send(ctl::ack, out);
        return;
    }
    if (frame.number != expected_fn_) {
        out.emplace_back(ev::Warning{
            std::format("frame number {} where {} was expected", frame.number, expected_fn_)});
        send(ctl::nak, out);
        return;
    }

    send(ctl::ack, out);
    got_frame_ = true;
    expected_fn_ = next_frame_number(expected_fn_);
    record_buf_.insert(record_buf_.end(), frame.text.begin(), frame.text.end());
    if (frame.last) {
        if (!record_buf_.empty() && record_buf_.back() == ctl::cr) {
            record_buf_.pop_back();
        } else {
            out.emplace_back(ev::Warning{"record does not end with CR"});
        }
        out.emplace_back(ev::RecordReceived{std::exchange(record_buf_, {})});
    }
}

void Link::on_timer(TimerId id, Events& out) {
    if (id != armed_) {
        return;  // stale
    }
    armed_ = 0;
    switch (state_) {
        case State::establishing:
        case State::sending:
            send(ctl::eot, out);
            finish(Outcome::timeout, out);
            kick(out);
            return;
        case State::busy_wait:
        case State::contention_wait:
            send(ctl::enq, out);
            state_ = State::establishing;
            arm(timing_.reply_timeout, out);
            return;
        case State::receiving:
            end_receive(false, "receive timeout", out);
            kick(out);
            return;
        case State::neutral:
            holdoff_ = false;
            kick(out);
            return;
    }
}

void Link::on_disconnect(Events& out) {
    connected_ = false;
    holdoff_ = false;
    if (state_ == State::receiving) {
        end_receive(false, "disconnected", out);
    }
    finish(Outcome::disconnected, out);
    disarm(out);
    state_ = State::neutral;
}

void Link::resume(Events& out) {
    connected_ = true;
    kick(out);
}

void Link::cancel(MessageId id, Events& out) {
    if (current_ && current_->id == id) {
        if (state_ == State::establishing || state_ == State::sending) {
            send(ctl::eot, out);
        }
        finish(Outcome::cancelled, out);
        kick(out);
        return;
    }
    const auto it = std::ranges::find(queue_, id, &Outgoing::id);
    if (it != queue_.end()) {
        out.emplace_back(ev::SendFinished{
            .id = id, .outcome = Outcome::cancelled, .interrupt_requested = false});
        queue_.erase(it);
    }
}

void Link::cancel_all(Events& out) {
    while (!queue_.empty()) {
        out.emplace_back(ev::SendFinished{
            .id = queue_.front().id, .outcome = Outcome::cancelled, .interrupt_requested = false});
        queue_.pop_front();
    }
    if (current_) {
        cancel(current_->id, out);
    }
}

}  // namespace lisbench::lis01

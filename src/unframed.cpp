#include "lisbench/unframed.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace lisbench {

namespace {

using lis01::ctl::cr;

// The LIS01-A2 handshake and frame characters. On a link without LIS01 they
// mean the partner speaks it after all: worth saying, not worth keeping.
constexpr bool is_lis01_control(std::byte b) noexcept {
    using namespace lis01::ctl;
    return b == stx || b == etx || b == etb || b == eot || b == enq || b == ack || b == nak;
}

}  // namespace

std::string_view to_string(LowLevel l) noexcept {
    switch (l) {
        case LowLevel::lis01_a2: return "lis01-a2";
        case LowLevel::none: return "none";
        case LowLevel::mllp: return "mllp";
    }
    return "?";
}

std::optional<LowLevel> low_level_from(std::string_view name) noexcept {
    for (const auto l : {LowLevel::lis01_a2, LowLevel::none, LowLevel::mllp}) {
        if (to_string(l) == name) {
            return l;
        }
    }
    return std::nullopt;
}

bytes encode_unframed(LowLevel low_level, const lis01::Message& message) {
    bytes out;
    if (low_level == LowLevel::mllp) {
        out.push_back(mllp::start_block);
    }
    for (const auto& r : message.records) {
        out.insert(out.end(), r.begin(), r.end());
        out.push_back(cr);
    }
    if (low_level == LowLevel::mllp) {
        out.push_back(mllp::end_block);
        out.push_back(cr);
    }
    return out;
}

// --- decoder -------------------------------------------------------------------

void UnframedDecoder::feed(bytes_view data, std::vector<UnframedEvent>& out) {
    for (const auto b : data) {
        on_byte(b, out);
        ++offset_;
    }
    flush_noise(out);
}

std::string describe_noise(bytes_view noise) {
    auto text = std::format("{} bytes outside an MLLP block", noise.size());
    if (std::ranges::any_of(noise, is_lis01_control)) {
        text += " (LIS01-A2 control characters: does the partner use LIS01-A2?)";
    }
    return text;
}

void UnframedDecoder::flush_noise(std::vector<UnframedEvent>& out) {
    if (!noise_.empty()) {
        out.emplace_back(
            unframed::Noise{.offset = noise_start_, .data = std::exchange(noise_, {})});
    }
}

void UnframedDecoder::on_byte(std::byte b, std::vector<UnframedEvent>& out) {
    if (low_level_ == LowLevel::mllp) {
        if (after_fs_) {
            after_fs_ = false;
            if (b == cr) {
                return;
            }
            out.emplace_back(unframed::Problem{.offset = offset_, .text = "FS not followed by CR"});
        }
        if (b == mllp::start_block) {
            flush_noise(out);
            if (in_block_ && in_message()) {
                end_message(false, "VT inside a block: new block before FS", out);
            }
            in_block_ = true;
            return;
        }
        if (!in_block_) {
            if (noise_.empty()) {
                noise_start_ = offset_;
            }
            noise_.push_back(b);
            return;
        }
        if (b == mllp::end_block) {
            in_block_ = false;
            after_fs_ = true;
            end_message(true, "", out);
            return;
        }
    }
    if (b == lis01::ctl::lf && after_cr_) {
        after_cr_ = false;
        return;
    }
    after_cr_ = false;
    if (is_lis01_control(b)) {
        if (!told_lis01_) {
            told_lis01_ = true;
            out.emplace_back(unframed::Problem{
                .offset = offset_,
                .text = std::format("LIS01-A2 control character 0x{:02X} on a link without "
                                    "LIS01-A2: dropped (does the partner use LIS01-A2?)",
                                    std::to_integer<unsigned>(b))});
        }
        return;
    }
    if (b == cr) {
        after_cr_ = true;
        end_record(out);
        return;
    }
    if (record_.empty()) {
        record_start_ = offset_;
    }
    if (record_.size() >= max_record) {
        if (!too_long_) {
            too_long_ = true;
            out.emplace_back(unframed::Problem{
                .offset = record_start_,
                .text = std::format("record longer than {} bytes: dropped", max_record)});
        }
        return;
    }
    record_.push_back(b);
}

void UnframedDecoder::end_record(std::vector<UnframedEvent>& out) {
    if (std::exchange(too_long_, false)) {
        record_.clear();
        return;
    }
    if (record_.empty()) {
        return;  // empty line
    }
    const auto type = std::to_integer<char>(record_.front());
    if (low_level_ == LowLevel::none && type == 'H' && records_ > 0) {
        end_message(false, "H record before the L record of the previous message", out);
    }
    ++records_;
    out.emplace_back(unframed::Record{.offset = record_start_, .text = std::exchange(record_, {})});
    if (low_level_ == LowLevel::none && type == 'L') {
        end_message(true, "", out);
    }
}

void UnframedDecoder::end_message(bool complete, std::string reason,
                                  std::vector<UnframedEvent>& out) {
    if (!record_.empty()) {
        // mllp: a last record without CR before FS still counts.
        too_long_ = false;
        ++records_;
        out.emplace_back(
            unframed::Record{.offset = record_start_, .text = std::exchange(record_, {})});
    }
    if (records_ == 0 && complete && low_level_ == LowLevel::none) {
        return;
    }
    out.emplace_back(
        unframed::MessageEnd{.offset = offset_, .complete = complete, .reason = std::move(reason)});
    records_ = 0;
    told_lis01_ = false;
}

void UnframedDecoder::finish(std::vector<UnframedEvent>& out) {
    flush_noise(out);
    if (in_message()) {
        end_message(false, "stream ended inside a message", out);
    }
    in_block_ = false;
}

void UnframedDecoder::abandon(std::string reason, std::vector<UnframedEvent>& out) {
    if (in_message()) {
        end_message(false, std::move(reason), out);
    }
    in_block_ = false;
    after_cr_ = false;
}

// --- link ------------------------------------------------------------------------

UnframedLink::UnframedLink(LowLevel low_level, lis01::Timing timing)
    : low_level_{low_level}, timing_{timing}, decoder_{low_level} {}

lis01::MessageId UnframedLink::submit(const lis01::Message& message, lis01::Events& out) {
    const auto id = next_id_++;
    queue_.push_back(Queued{.id = id, .data = encode_unframed(low_level_, message)});
    resume_sending(out);
    return id;
}

void UnframedLink::resume_sending(lis01::Events& out) {
    // Without acknowledgements a message is done when it is on its way.
    while (connected_ && !queue_.empty()) {
        auto q = std::move(queue_.front());
        queue_.pop_front();
        out.emplace_back(lis01::ev::Send{.data = std::move(q.data)});
        out.emplace_back(lis01::ev::SendFinished{
            .id = q.id, .outcome = lis01::Outcome::delivered, .interrupt_requested = false});
    }
}

void UnframedLink::forward(std::vector<UnframedEvent>& decoded, lis01::Events& out) {
    for (auto& e : decoded) {
        if (auto* r = std::get_if<unframed::Record>(&e)) {
            out.emplace_back(lis01::ev::RecordReceived{.record = std::move(r->text)});
        } else if (auto* m = std::get_if<unframed::MessageEnd>(&e)) {
            out.emplace_back(lis01::ev::ReceiveFinished{.complete = m->complete,
                                                        .reason = std::move(m->reason)});
        } else if (const auto* n = std::get_if<unframed::Noise>(&e)) {
            out.emplace_back(lis01::ev::Warning{.text = describe_noise(n->data) + ": ignored"});
        } else if (auto* p = std::get_if<unframed::Problem>(&e)) {
            out.emplace_back(lis01::ev::Warning{.text = std::move(p->text)});
        }
    }
    decoded.clear();
}

void UnframedLink::watch_timer(lis01::Events& out) {
    if (decoder_.in_message()) {
        armed_ = next_timer_++;
        out.emplace_back(lis01::ev::ArmTimer{.id = armed_, .after = timing_.receive_timeout});
    } else if (armed_ != 0) {
        armed_ = 0;
        out.emplace_back(lis01::ev::CancelTimer{});
    }
}

void UnframedLink::on_bytes(bytes_view data, lis01::Events& out) {
    std::vector<UnframedEvent> decoded;
    decoder_.feed(data, decoded);
    forward(decoded, out);
    watch_timer(out);
}

void UnframedLink::on_timer(lis01::TimerId id, lis01::Events& out) {
    if (id != armed_) {
        return;
    }
    armed_ = 0;
    std::vector<UnframedEvent> decoded;
    decoder_.abandon(
        std::format("no data for {} ms inside a message", timing_.receive_timeout.count()),
        decoded);
    forward(decoded, out);
}

void UnframedLink::on_disconnect(lis01::Events& out) {
    connected_ = false;
    std::vector<UnframedEvent> decoded;
    decoder_.finish(decoded);
    for (auto& e : decoded) {
        if (auto* m = std::get_if<unframed::MessageEnd>(&e)) {
            m->reason = "connection lost inside a message";
        }
    }
    forward(decoded, out);
    if (std::exchange(armed_, 0) != 0) {
        out.emplace_back(lis01::ev::CancelTimer{});
    }
}

void UnframedLink::resume(lis01::Events& out) {
    connected_ = true;
    decoder_ = UnframedDecoder{low_level_};
    resume_sending(out);
}

void UnframedLink::cancel(lis01::MessageId id, lis01::Events& out) {
    const auto it = std::ranges::find(queue_, id, &Queued::id);
    if (it != queue_.end()) {
        queue_.erase(it);
        out.emplace_back(lis01::ev::SendFinished{
            .id = id, .outcome = lis01::Outcome::cancelled, .interrupt_requested = false});
    }
}

void UnframedLink::cancel_all(lis01::Events& out) {
    for (const auto& q : queue_) {
        out.emplace_back(lis01::ev::SendFinished{
            .id = q.id, .outcome = lis01::Outcome::cancelled, .interrupt_requested = false});
    }
    queue_.clear();
}

}  // namespace lisbench

#include "lisbench/lis01_decoder.hpp"

#include <format>
#include <utility>

namespace lisbench::lis01 {

void StreamDecoder::feed(bytes_view data, std::vector<DecoderEvent>& out) {
    for (const auto b : data) {
        on_byte(b, out);
        ++offset_;
    }
}

void StreamDecoder::flush_noise(std::vector<DecoderEvent>& out) {
    if (!noise_.empty()) {
        out.emplace_back(dec::Noise{.offset = noise_start_, .data = std::exchange(noise_, {})});
    }
}

void StreamDecoder::on_byte(std::byte b, std::vector<DecoderEvent>& out) {
    if (!frame_.empty()) {
        if (b == ctl::stx) {
            out.emplace_back(dec::Problem{.offset = offset_,
                                          .text = "STX inside a frame; previous bytes discarded"});
            frame_.assign(1, b);
            frame_start_ = offset_;
            terminator_at_.reset();
            return;
        }
        frame_.push_back(b);
        if (!terminator_at_ && (b == ctl::etx || b == ctl::etb)) {
            terminator_at_ = frame_.size() - 1;
        }
        if (terminator_at_ && frame_.size() == *terminator_at_ + 5) {
            complete_frame(out);
        } else if (!terminator_at_ && frame_.size() > max_text_ + frame_overhead) {
            out.emplace_back(dec::FrameBad{.offset = frame_start_,
                                           .error = FrameError::text_too_long,
                                           .raw = std::exchange(frame_, {})});
        }
        return;
    }

    if (b == ctl::stx) {
        flush_noise(out);
        frame_.push_back(b);
        frame_start_ = offset_;
        return;
    }
    if (b == ctl::enq || b == ctl::ack || b == ctl::nak || b == ctl::eot) {
        flush_noise(out);
        out.emplace_back(dec::Control{.offset = offset_, .value = b});
        if (b == ctl::enq) {
            expected_ = 1;
            previous_.reset();
        } else if (b == ctl::eot && !record_.empty()) {
            out.emplace_back(dec::Problem{.offset = offset_,
                                          .text = "EOT inside a record; partial record discarded"});
            record_.clear();
        }
        return;
    }
    if (noise_.empty()) {
        noise_start_ = offset_;
    }
    noise_.push_back(b);
}

void StreamDecoder::complete_frame(std::vector<DecoderEvent>& out) {
    auto raw = std::exchange(frame_, {});
    terminator_at_.reset();
    auto frame = decode(raw, max_text_);
    if (!frame) {
        out.emplace_back(
            dec::FrameBad{.offset = frame_start_, .error = frame.error(), .raw = std::move(raw)});
        return;
    }
    if (previous_ && *previous_ == *frame) {
        out.emplace_back(
            dec::FrameOk{.offset = frame_start_, .frame = std::move(*frame), .repeated = true});
        return;
    }
    if (frame->number != expected_) {
        out.emplace_back(dec::Problem{.offset = frame_start_,
                                      .text = std::format("frame number {} where {} was expected",
                                                          frame->number, expected_)});
    }
    expected_ = next_frame_number(frame->number);
    if (record_.empty()) {
        record_start_ = frame_start_;
    }
    record_.insert(record_.end(), frame->text.begin(), frame->text.end());
    const bool last = frame->last;
    previous_ = *frame;
    out.emplace_back(
        dec::FrameOk{.offset = frame_start_, .frame = std::move(*frame), .repeated = false});
    if (last) {
        if (!record_.empty() && record_.back() == ctl::cr) {
            record_.pop_back();
        } else {
            out.emplace_back(
                dec::Problem{.offset = frame_start_, .text = "record does not end with CR"});
        }
        out.emplace_back(dec::Record{.offset = record_start_, .text = std::exchange(record_, {})});
    }
}

void StreamDecoder::finish(std::vector<DecoderEvent>& out) {
    flush_noise(out);
    if (!frame_.empty()) {
        out.emplace_back(dec::FrameBad{.offset = frame_start_,
                                       .error = FrameError::too_short,
                                       .raw = std::exchange(frame_, {})});
        terminator_at_.reset();
    }
    if (!record_.empty()) {
        out.emplace_back(dec::Problem{.offset = offset_, .text = "stream ends inside a record"});
        record_.clear();
    }
}

Capture deframe(bytes_view stream, std::size_t max_text) {
    StreamDecoder decoder{max_text};
    std::vector<DecoderEvent> events;
    decoder.feed(stream, events);
    decoder.finish(events);
    Capture out;
    for (auto& e : events) {
        if (auto* r = std::get_if<dec::Record>(&e)) {
            out.records.push_back(std::move(r->text));
        } else if (const auto* p = std::get_if<dec::Problem>(&e)) {
            out.problems.push_back(std::format("offset {}: {}", p->offset, p->text));
        } else if (const auto* f = std::get_if<dec::FrameBad>(&e)) {
            out.problems.push_back(std::format("offset {}: {}", f->offset, to_string(f->error)));
        }
    }
    return out;
}

}  // namespace lisbench::lis01

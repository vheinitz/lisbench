#include "lisbench/monitor/monitor.hpp"

#include <algorithm>
#include <format>
#include <utility>

#include "lisbench/printable.hpp"

namespace lisbench::monitor {

namespace {

template <typename... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

std::string control_name(std::byte b) {
    switch (std::to_integer<unsigned>(b)) {
        case 0x05: return "ENQ";
        case 0x06: return "ACK";
        case 0x15: return "NAK";
        case 0x04: return "EOT";
        default: return std::format("0x{:02X}", std::to_integer<unsigned>(b));
    }
}

Side other(Side s) {
    return s == Side::a ? Side::b : Side::a;
}

}  // namespace

std::string_view to_string(Kind k) noexcept {
    switch (k) {
        case Kind::control: return "control";
        case Kind::frame: return "frame";
        case Kind::repeated: return "repeated";
        case Kind::bad_frame: return "bad_frame";
        case Kind::record: return "record";
        case Kind::transfer: return "transfer";
        case Kind::problem: return "problem";
        case Kind::noise: return "noise";
        case Kind::watch: return "watch";
        case Kind::slow_reply: return "slow_reply";
    }
    return "unknown";
}

namespace {

std::variant<lis01::StreamDecoder, UnframedDecoder> decoder_for(LowLevel l, std::size_t max_text) {
    if (l == LowLevel::lis01_a2) {
        return lis01::StreamDecoder{max_text};
    }
    return UnframedDecoder{l};
}

}  // namespace

Monitor::Monitor(Options options)
    : options_{std::move(options)},
      sides_{SideState{.decoder = decoder_for(options_.low_level[0], options_.max_text)},
             SideState{.decoder = decoder_for(options_.low_level[1], options_.max_text)}} {}

std::vector<Event> Monitor::feed(Side from, Clock::time_point at, bytes_view data) {
    std::vector<Event> out;
    run_decoder(from, at, out, [&data](auto& d, auto& decoded) { d.feed(data, decoded); });
    return out;
}

std::vector<Event> Monitor::finish(Clock::time_point at) {
    std::vector<Event> out;
    for (const auto s : {Side::a, Side::b}) {
        run_decoder(s, at, out, [](auto& d, auto& decoded) { d.finish(decoded); });
    }
    return out;
}

void Monitor::on_unframed(Side from, Clock::time_point at, UnframedEvent& e,
                          std::vector<Event>& out) {
    auto& side = sides_.at(index(from));
    auto& st = stats_.at(index(from));
    std::visit(
        overloaded{
            [&](unframed::Record& r) {
                if (side.records_in_transfer == 0) {
                    side.saw_terminator = false;
                }
                on_record(from, at, std::move(r.text), out);
            },
            [&](const unframed::MessageEnd& m) {
                ++st.transfers;
                const bool whole = m.complete && side.saw_terminator;
                if (!whole) {
                    ++st.incomplete_transfers;
                }
                auto text = std::format("{} records, {}", side.records_in_transfer,
                                        whole ? "ends with L" : "INCOMPLETE");
                if (!m.reason.empty()) {
                    text += std::format(": {}", m.reason);
                } else if (!side.saw_terminator) {
                    text += ": no L record";
                }
                out.push_back(
                    Event{.at = at, .from = from, .kind = Kind::transfer, .text = std::move(text)});
                side.records_in_transfer = 0;
            },
            [&](unframed::Noise& n) {
                out.push_back(Event{.at = at,
                                    .from = from,
                                    .kind = Kind::noise,
                                    .text = describe_noise(n.data),
                                    .raw = std::move(n.data)});
            },
            [&](const unframed::Problem& p) {
                ++st.problems;
                out.push_back(Event{.at = at, .from = from, .kind = Kind::problem, .text = p.text});
            },
        },
        e);
}

void Monitor::reply_from(Side from, Clock::time_point at, std::vector<Event>& out) {
    auto& waiting = sides_.at(index(other(from))).waiting_since;
    if (!waiting) {
        return;
    }
    const auto took = std::chrono::duration_cast<milliseconds>(at - *waiting);
    waiting.reset();
    auto& st = stats_.at(index(from));
    st.max_reply = std::max(st.max_reply, took);
    if (took > options_.slow_reply) {
        out.push_back(Event{.at = at,
                            .from = from,
                            .kind = Kind::slow_reply,
                            .text = std::format("reply after {} ms (limit {} ms)", took.count(),
                                                options_.slow_reply.count())});
    }
}

void Monitor::on_decoded(Side from, Clock::time_point at, lis01::DecoderEvent& e,
                         std::vector<Event>& out) {
    auto& side = sides_.at(index(from));
    auto& st = stats_.at(index(from));
    std::visit(
        overloaded{
            [&](const lis01::dec::Control& c) {
                out.push_back(Event{
                    .at = at, .from = from, .kind = Kind::control, .text = control_name(c.value)});
                if (c.value == lis01::ctl::enq) {
                    side.in_transfer = true;
                    side.records_in_transfer = 0;
                    side.saw_terminator = false;
                    side.waiting_since = at;
                } else if (c.value == lis01::ctl::eot) {
                    if (side.in_transfer && side.records_in_transfer == 0) {
                        // ENQ, reply, EOT: a link check, or a sender that gave up waiting.
                        out.push_back(Event{.at = at,
                                            .from = from,
                                            .kind = Kind::transfer,
                                            .text = "empty transfer, no frames"});
                    } else if (side.in_transfer) {
                        ++st.transfers;
                        const bool whole = side.saw_terminator;
                        if (!whole) {
                            ++st.incomplete_transfers;
                        }
                        out.push_back(Event{
                            .at = at,
                            .from = from,
                            .kind = Kind::transfer,
                            .text =
                                std::format("{} records, {}", side.records_in_transfer,
                                            whole ? "ends with L" : "INCOMPLETE: no L record")});
                    }
                    side.in_transfer = false;
                    side.waiting_since.reset();
                    // EOT can also answer a frame (receiver interrupt).
                    reply_from(from, at, out);
                } else {
                    if (c.value == lis01::ctl::nak) {
                        ++st.naks_sent;
                    }
                    reply_from(from, at, out);
                }
            },
            [&](lis01::dec::FrameOk& f) {
                side.waiting_since = at;
                if (f.repeated) {
                    ++st.repeated_frames;
                    out.push_back(Event{.at = at,
                                        .from = from,
                                        .kind = Kind::repeated,
                                        .text = std::format("frame {} repeated", f.frame.number),
                                        .raw = lis01::encode(f.frame)});
                    return;
                }
                ++st.frames;
                out.push_back(Event{.at = at,
                                    .from = from,
                                    .kind = Kind::frame,
                                    .text = std::format("frame {} {}", f.frame.number,
                                                        f.frame.last ? "ETX" : "ETB"),
                                    .raw = lis01::encode(f.frame)});
            },
            [&](lis01::dec::FrameBad& f) {
                side.waiting_since = at;
                ++st.bad_frames;
                out.push_back(Event{.at = at,
                                    .from = from,
                                    .kind = Kind::bad_frame,
                                    .text = std::string{lis01::to_string(f.error)},
                                    .raw = std::move(f.raw)});
            },
            [&](lis01::dec::Record& r) { on_record(from, at, std::move(r.text), out); },
            [&](lis01::dec::Noise& n) {
                out.push_back(Event{.at = at,
                                    .from = from,
                                    .kind = Kind::noise,
                                    .text = std::format("{} bytes outside frames", n.data.size()),
                                    .raw = std::move(n.data)});
            },
            [&](const lis01::dec::Problem& p) {
                ++st.problems;
                out.push_back(Event{.at = at, .from = from, .kind = Kind::problem, .text = p.text});
            },
        },
        e);
}

void Monitor::on_record(Side from, Clock::time_point at, bytes text, std::vector<Event>& out) {
    auto& side = sides_.at(index(from));
    auto& st = stats_.at(index(from));
    ++st.records;
    ++side.records_in_transfer;
    const auto wire = lisbench::to_string(text);
    if (!wire.empty() && wire.front() == 'H') {
        if (const auto d = lis02::delimiters_from_header(wire)) {
            side.delimiters = *d;
        }
        side.saw_terminator = false;
    }
    if (!wire.empty() && wire.front() == 'L') {
        side.saw_terminator = true;
    }
    auto record =
        lis02::parse_record(wire, side.delimiters, {.text = options_.text, .strict = false});
    Event ev{.at = at,
             .from = from,
             .kind = Kind::record,
             .text = printable(text),
             .raw = std::move(text)};
    if (!record) {
        ev.text += std::format("   [{}]", record.error().text);
        out.push_back(std::move(ev));
        return;
    }
    ev.record = *record;
    const auto record_wire = ev.raw;
    out.push_back(std::move(ev));
    for (std::size_t i = 0; i < options_.watches.size(); ++i) {
        const auto& w = options_.watches[i];
        if (watch::matches(w, *record, side.delimiters)) {
            ++st.watch_hits;
            auto value = watch::value_of(w, *record, side.delimiters);
            out.push_back(Event{.at = at,
                                .from = from,
                                .kind = Kind::watch,
                                .text = std::format("{}   (value: {})", w.text, value),
                                .record = *record,
                                .raw = record_wire,
                                .watch = i,
                                .value = std::move(value)});
        }
    }
}

std::vector<std::string> format(const Event& e, const Options& options, const FormatOptions& f) {
    const bool always = e.kind != Kind::control && e.kind != Kind::frame;
    const bool nak = e.kind == Kind::control && e.text == "NAK";
    if (!always && !nak && !f.verbose) {
        return {};
    }
    const auto& from = options.labels.at(e.from == Side::a ? 0 : 1);
    const auto& to = options.labels.at(e.from == Side::a ? 1 : 0);
    const auto time = std::format("{:%T}", std::chrono::floor<milliseconds>(e.at));
    const auto marker = [&]() -> std::string_view {
        switch (e.kind) {
            case Kind::watch: return "!! ";
            case Kind::problem:
            case Kind::bad_frame:
            case Kind::slow_reply: return "?? ";
            default: return "";
        }
    }();
    std::vector<std::string> lines;
    std::string text = e.text;
    if (e.kind == Kind::frame || e.kind == Kind::repeated || e.kind == Kind::bad_frame) {
        if (f.verbose || e.kind == Kind::bad_frame) {
            text += "  " + printable(e.raw);
        }
    }
    lines.push_back(
        std::format("{} {}>{} {:<10} {}{}", time, from, to, to_string(e.kind), marker, text));
    if (f.decode && e.kind == Kind::record && e.record) {
        const auto& r = *e.record;
        const auto kind = r.kind();
        for (std::size_t n = 2; n <= r.fields.size(); ++n) {
            const auto& field = r.fields[n - 1];
            if (field.empty()) {
                continue;
            }
            std::string value;
            for (std::size_t rep = 0; rep < field.size(); ++rep) {
                if (rep > 0) {
                    value += " \\ ";
                }
                for (std::size_t c = 0; c < field[rep].size(); ++c) {
                    if (c > 0) {
                        value += " ^ ";
                    }
                    value += field[rep][c];
                }
            }
            const auto name = kind ? lis02::field_name(*kind, n) : std::string_view{};
            lines.push_back(
                std::format("{:>30} {:>3} {:<28} {}", "", n, name.empty() ? "-" : name, value));
        }
    }
    return lines;
}

std::vector<std::string> summary(const Monitor& m) {
    std::vector<std::string> lines;
    for (const auto s : {Side::a, Side::b}) {
        const auto& st = m.stats(s);
        const auto& label = m.options().labels.at(s == Side::a ? 0 : 1);
        lines.push_back(std::format(
            "{}: {} transfers ({} incomplete), {} records, {} frames, {} repeated, {} bad, "
            "{} NAK sent, {} problems, {} watch hits, slowest reply {} ms",
            label, st.transfers, st.incomplete_transfers, st.records, st.frames, st.repeated_frames,
            st.bad_frames, st.naks_sent, st.problems, st.watch_hits, st.max_reply.count()));
    }
    return lines;
}

}  // namespace lisbench::monitor

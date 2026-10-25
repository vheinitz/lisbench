#include "lisbench/conform/tester.hpp"

#include <asio/experimental/awaitable_operators.hpp>
#include <asio/experimental/channel.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <variant>

#include "lisbench/lis01_decoder.hpp"
#include "lisbench/lis01_frame.hpp"

namespace lisbench::conform {

namespace {

using namespace asio::experimental::awaitable_operators;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
namespace ctl = lis01::ctl;

constexpr auto use_tuple = asio::as_tuple(asio::use_awaitable);

milliseconds since(Clock::time_point t0, Clock::time_point t1) {
    return std::chrono::duration_cast<milliseconds>(t1 - t0);
}

std::string ms(milliseconds d) {
    return std::format("{} ms", d.count());
}

// What the IUT sent, as the tester sees it.
struct In {
    Clock::time_point at{};
    lis01::DecoderEvent event{};
};

std::optional<std::byte> control_of(const In& in) {
    if (const auto* c = std::get_if<lis01::dec::Control>(&in.event)) {
        return c->value;
    }
    return std::nullopt;
}

const lis01::Frame* frame_of(const In& in) {
    if (const auto* f = std::get_if<lis01::dec::FrameOk>(&in.event)) {
        return &f->frame;
    }
    return nullptr;
}

bool is_frame(const In& in) {
    return std::holds_alternative<lis01::dec::FrameOk>(in.event) ||
           std::holds_alternative<lis01::dec::FrameBad>(in.event);
}

std::string describe(const std::optional<In>& in) {
    if (!in) {
        return "no reply";
    }
    if (const auto c = control_of(*in)) {
        if (*c == ctl::ack) {
            return "ACK";
        }
        if (*c == ctl::nak) {
            return "NAK";
        }
        if (*c == ctl::eot) {
            return "EOT";
        }
        if (*c == ctl::enq) {
            return "ENQ";
        }
    }
    if (is_frame(*in)) {
        return "a frame";
    }
    return "other bytes";
}

bool is(const std::optional<In>& in, std::byte b) {
    return in && control_of(*in) == b;
}

lis01::Message message_of(std::initializer_list<std::string_view> records) {
    lis01::Message m;
    for (const auto r : records) {
        m.records.push_back(to_bytes(r));
    }
    return m;
}

// Results for a LIS, orders for an instrument; the content is not under test.
lis01::Message ordinary_message(Iut iut) {
    if (iut == Iut::computer) {
        return message_of({"H|\\^&|||lisbench-conform|||||||P|LIS2-A2", "P|1",
                           "O|1|CONF-1||^^^TEST", "R|1|^^^TEST|1|", "L|1|N"});
    }
    return message_of(
        {"H|\\^&|||lisbench-conform|||||||P|LIS2-A2", "P|1", "O|1|CONF-1||^^^TEST|R", "L|1|N"});
}

lis01::Message query_message() {
    return message_of(
        {"H|\\^&|||lisbench-conform|||||||P|LIS2-A2", "Q|1|^CONF-1||ALL||||||||O", "L|1|N"});
}

std::vector<bytes> encoded(const lis01::Message& m) {
    std::vector<bytes> out;
    for (const auto& f : lis01::frames_for(m)) {
        out.push_back(lis01::encode(f));
    }
    return out;
}

class Tester {
public:
    Tester(io::Stream& stream, const Options& options, Progress progress)
        : stream_{stream},
          o_{options},
          progress_{std::move(progress)},
          channel_{std::visit([](auto& s) { return s.get_executor(); }, stream), 4096},
          decoder_{options.timing.max_text} {}

    asio::awaitable<void> reader() {
        std::array<std::byte, 1024> buffer{};
        while (true) {
            const auto [ec, n] = co_await std::visit(
                [&buffer](auto& s) { return s.async_read_some(asio::buffer(buffer), use_tuple); },
                stream_);
            if (ec || n == 0) {
                break;
            }
            const auto now = Clock::now();
            std::vector<lis01::DecoderEvent> events;
            decoder_.feed(bytes_view{buffer.data(), n}, events);
            for (auto& e : events) {
                note_for_ts01(e);
                // Records and decoder notes are for TS01 only; replies are bytes on the wire.
                if (std::holds_alternative<lis01::dec::Record>(e) ||
                    std::holds_alternative<lis01::dec::Problem>(e)) {
                    continue;
                }
                co_await channel_.async_send({}, In{.at = now, .event = std::move(e)}, use_tuple);
            }
        }
        channel_.close();
    }

    asio::awaitable<std::vector<Result>> suite() {
        std::map<std::string, Result> results;
        const auto wanted = [this](std::string_view id) {
            return o_.only.empty() || std::ranges::find(o_.only, id) != o_.only.end();
        };
        using Case = asio::awaitable<Result> (Tester::*)();
        const std::array<std::pair<std::string_view, Case>, 12> active{{
            {"TR01", &Tester::tr01},
            {"TR02", &Tester::tr02},
            {"TR03", &Tester::tr03},
            {"TR04", &Tester::tr04},
            {"TR05", &Tester::tr05},
            {"TR07", &Tester::tr07},
            {"TS02", &Tester::ts02},
            {"TS03", &Tester::ts03},
            {"TS04", &Tester::ts04},
            {"TS05", &Tester::ts05},
            {"TS06", &Tester::ts06},
            {"TS07", &Tester::ts07},
        }};
        for (const auto& [id, fn] : active) {
            if (!wanted(id)) {
                continue;
            }
            if (closed_) {
                results[std::string{id}] = {.id = std::string{id},
                                            .verdict = Verdict::inconclusive,
                                            .detail = "connection closed"};
                continue;
            }
            progress_(std::format("{} {}", id, find(id)->purpose));
            auto r = co_await (this->*fn)();
            r.id = std::string{id};
            results[r.id] = std::move(r);
        }
        // Evaluated from what the other cases saw.
        if (wanted("TR06")) {
            results["TR06"] = tr06();
            results["TR06"].id = "TR06";
        }
        if (wanted("TS01")) {
            results["TS01"] = ts01();
            results["TS01"].id = "TS01";
        }
        std::vector<Result> out;
        for (const auto& tc : conform::suite) {
            if (auto it = results.find(std::string{tc.id}); it != results.end()) {
                out.push_back(std::move(it->second));
            }
        }
        std::visit(
            [](auto& s) {
                std::error_code ec;
                s.close(ec);
            },
            stream_);
        co_return out;
    }

private:
    // --- wire -------------------------------------------------------------

    asio::awaitable<std::optional<In>> next(milliseconds timeout) {
        if (closed_) {
            co_return std::nullopt;
        }
        asio::steady_timer t{channel_.get_executor(), timeout};
        auto r = co_await (channel_.async_receive(use_tuple) || t.async_wait(use_tuple));
        if (r.index() == 1) {
            co_return std::nullopt;
        }
        auto [ec, in] = std::get<0>(std::move(r));
        if (ec) {
            closed_ = true;
            co_return std::nullopt;
        }
        co_return std::move(in);
    }

    asio::awaitable<void> write(bytes data) {
        const auto [ec, n] = co_await std::visit(
            [&data](auto& s) { return asio::async_write(s, asio::buffer(data), use_tuple); },
            stream_);
        if (ec) {
            closed_ = true;
        }
        sent_at_ = Clock::now();
    }

    asio::awaitable<void> write(std::byte b) { co_await write(bytes{b}); }

    // The IUT's answer to what was just written; reply times are kept for TR06.
    asio::awaitable<std::optional<In>> reply() {
        auto in = co_await next(o_.timing.reply_timeout * 2);
        if (in) {
            max_reply_ = std::max(max_reply_, since(sent_at_, in->at));
            ++replies_;
        }
        co_return in;
    }

    // LIS01-A2: the instrument; a profile may state otherwise (Timing::contention_winner).
    bool iut_wins_contention() const {
        return (o_.iut == Iut::instrument) ==
               (o_.timing.contention_winner == lis01::Role::instrument);
    }

    milliseconds settle_time() const {
        return std::min(milliseconds{500}, o_.timing.reply_timeout / 2);
    }

    // Waits until the line is quiet; a transfer the IUT starts meanwhile is
    // received and acknowledged.
    asio::awaitable<void> quiet() {
        for (int i = 0; i < 20; ++i) {
            const auto in = co_await next(settle_time());
            if (!in) {
                co_return;
            }
            if (is(in, ctl::enq)) {
                co_await write(ctl::ack);
                (void)co_await receive(ack_all());
            }
        }
    }

    // The tester's ENQ until the IUT answers ACK; handles NAK and contention.
    asio::awaitable<bool> establish() {
        for (int attempt = 0; attempt < 5 && !closed_; ++attempt) {
            co_await write(ctl::enq);
            const auto r = co_await reply();
            if (is(r, ctl::ack)) {
                co_return true;
            }
            if (is(r, ctl::nak)) {
                co_await wait(o_.timing.busy_retry);
                continue;
            }
            if (is(r, ctl::enq)) {
                if (iut_wins_contention()) {
                    // The IUT has priority: take its message first.
                    if (co_await wait_enq(o_.timing.contention_computer)) {
                        co_await write(ctl::ack);
                        (void)co_await receive(ack_all());
                    }
                } else {
                    co_await wait(o_.timing.contention_instrument);
                }
                continue;
            }
            co_return false;
        }
        co_return false;
    }

    // Lets time pass while still answering a transfer the IUT starts.
    asio::awaitable<void> wait(milliseconds d) {
        const auto until = Clock::now() + d;
        while (Clock::now() < until && !closed_) {
            const auto in = co_await next(since(Clock::now(), until));
            if (is(in, ctl::enq)) {
                co_await write(ctl::ack);
                (void)co_await receive(ack_all());
            }
        }
    }

    asio::awaitable<std::optional<Clock::time_point>> wait_enq(milliseconds timeout) {
        const auto until = Clock::now() + timeout;
        while (Clock::now() < until) {
            const auto in = co_await next(since(Clock::now(), until));
            if (!in) {
                break;
            }
            if (is(in, ctl::enq)) {
                co_return in->at;
            }
        }
        co_return std::nullopt;
    }

    // The tester's reply to each received frame (by its position in the
    // transfer, repeats included); nullopt: no reply.
    using Policy = std::function<std::optional<std::byte>(std::size_t, const In&)>;

    static Policy ack_all() {
        return [](std::size_t, const In& f) -> std::optional<std::byte> {
            return frame_of(f) != nullptr ? ctl::ack : ctl::nak;
        };
    }

    struct Received {
        std::vector<In> frames{};
        std::optional<Clock::time_point> eot{};
    };

    // After the tester answered the IUT's ENQ with ACK: frames until EOT.
    asio::awaitable<Received> receive(Policy policy) {
        Received r;
        const auto limit = std::max(o_.timing.receive_timeout, o_.timing.reply_timeout * 3);
        while (true) {
            const auto in = co_await next(limit);
            if (!in) {
                co_return r;
            }
            if (is(in, ctl::eot)) {
                r.eot = in->at;
                co_return r;
            }
            if (!is_frame(*in)) {
                continue;
            }
            r.frames.push_back(*in);
            if (const auto answer = policy(r.frames.size() - 1, *in)) {
                co_await write(*answer);
            }
        }
    }

    // Sends a whole transfer frame by frame and returns the IUT's replies.
    asio::awaitable<std::vector<std::optional<In>>> send_all(std::vector<bytes> frames) {
        std::vector<std::optional<In>> replies;
        for (const auto& f : frames) {
            co_await write(f);
            replies.push_back(co_await reply());
            if (!is(replies.back(), ctl::ack)) {
                break;
            }
        }
        co_await write(ctl::eot);
        co_return replies;
    }

    // TS: makes the IUT start a transfer; returns the time of its ENQ.
    asio::awaitable<std::optional<Clock::time_point>> provoke() {
        if (o_.iut == Iut::computer) {
            co_await quiet();
            if (!co_await establish()) {
                co_return std::nullopt;
            }
            (void)co_await send_all(encoded(query_message()));
        }
        co_return co_await wait_enq(o_.wait_for_iut);
    }

    Result no_transfer() const {
        return {
            .verdict = Verdict::inconclusive,
            .detail = std::format("the IUT started no transfer within {}", ms(o_.wait_for_iut))};
    }

    void note_for_ts01(const lis01::DecoderEvent& e) {
        if (std::holds_alternative<lis01::dec::FrameOk>(e)) {
            ++frames_seen_;
        } else if (const auto* bad = std::get_if<lis01::dec::FrameBad>(&e)) {
            ++frames_seen_;
            frame_problems_.emplace_back(lis01::to_string(bad->error));
        } else if (const auto* p = std::get_if<lis01::dec::Problem>(&e)) {
            if (p->text.starts_with("frame number") || p->text.starts_with("STX inside") ||
                p->text.starts_with("restricted character")) {
                frame_problems_.push_back(p->text);
            }
        }
    }

    // --- IUT as receiver ---------------------------------------------------

    asio::awaitable<Result> tr01() {
        co_await quiet();
        if (!co_await establish()) {
            co_return Result{.detail = "the IUT did not accept ENQ"};
        }
        const auto frames = encoded(ordinary_message(o_.iut));
        const auto replies = co_await send_all(frames);
        if (replies.size() == frames.size() && is(replies.back(), ctl::ack)) {
            co_return Result{.verdict = Verdict::pass,
                             .detail = std::format("{} frames acknowledged", frames.size())};
        }
        co_return Result{.verdict = Verdict::fail,
                         .detail = std::format("frame {} answered with {}", replies.size(),
                                               describe(replies.back()))};
    }

    asio::awaitable<Result> tr02() {
        co_await quiet();
        if (!co_await establish()) {
            co_return Result{.detail = "the IUT did not accept ENQ"};
        }
        auto frames = encoded(ordinary_message(o_.iut));
        auto bad = frames.front();
        auto& c2 = bad.at(bad.size() - 3);  // second checksum digit
        c2 = c2 == std::byte{'0'} ? std::byte{'1'} : std::byte{'0'};
        co_await write(bad);
        const auto r = co_await reply();
        Result result;
        if (is(r, ctl::nak)) {
            result = {.verdict = Verdict::pass, .detail = "NAK"};
        } else {
            result = {.verdict = Verdict::fail,
                      .detail = std::format("answered with {}", describe(r))};
            if (is(r, ctl::ack)) {
                frames.erase(frames.begin());  // the IUT took the bad frame as frame 1
            }
        }
        if (is(r, ctl::nak) || is(r, ctl::ack)) {
            (void)co_await send_all(frames);
        } else {
            co_await write(ctl::eot);
        }
        co_return result;
    }

    asio::awaitable<Result> tr03() {
        co_await quiet();
        if (!co_await establish()) {
            co_return Result{.detail = "the IUT did not accept ENQ"};
        }
        const auto frames = lis01::frames_for(ordinary_message(o_.iut));
        co_await write(lis01::encode(frames.at(0)));
        if (!is(co_await reply(), ctl::ack)) {
            co_await write(ctl::eot);
            co_return Result{.detail = "the first frame was not acknowledged"};
        }
        auto skipped = frames.at(1);
        skipped.number = lis01::next_frame_number(skipped.number);
        co_await write(lis01::encode(skipped));
        const auto r = co_await reply();
        Result result =
            is(r, ctl::nak)
                ? Result{.verdict = Verdict::pass,
                         .detail = std::format("frame number {} answered with NAK", skipped.number)}
                : Result{.verdict = Verdict::fail,
                         .detail = std::format("frame number {} where {} was due: {}",
                                               skipped.number, frames.at(1).number, describe(r))};
        if (is(r, ctl::nak)) {
            std::vector<bytes> rest;
            for (std::size_t i = 1; i < frames.size(); ++i) {
                rest.push_back(lis01::encode(frames[i]));
            }
            (void)co_await send_all(rest);
        } else {
            co_await write(ctl::eot);
        }
        co_return result;
    }

    asio::awaitable<Result> tr04() {
        co_await quiet();
        if (!co_await establish()) {
            co_return Result{.detail = "the IUT did not accept ENQ"};
        }
        const auto frames = encoded(ordinary_message(o_.iut));
        co_await write(frames.at(0));
        if (!is(co_await reply(), ctl::ack)) {
            co_await write(ctl::eot);
            co_return Result{.detail = "the first frame was not acknowledged"};
        }
        co_await write(frames.at(0));  // as if our ACK had been lost
        const auto r = co_await reply();
        const auto rest = std::vector<bytes>(frames.begin() + 1, frames.end());
        if (is(r, ctl::ack)) {
            (void)co_await send_all(rest);
            co_return Result{.verdict = Verdict::pass, .detail = "repeated frame acknowledged"};
        }
        co_await write(ctl::eot);
        co_return Result{.verdict = Verdict::fail,
                         .detail = std::format("repeated frame answered with {}", describe(r))};
    }

    asio::awaitable<Result> tr05() {
        co_await quiet();
        if (!co_await establish()) {
            co_return Result{.detail = "the IUT did not accept ENQ"};
        }
        auto m = ordinary_message(o_.iut);
        // A comment long enough for intermediate frames and a frame number wrap.
        auto comment = to_bytes("C|1|I|");
        comment.resize(lis01::max_text_size * 9, std::byte{'x'});
        m.records.insert(m.records.end() - 1, comment);
        const auto frames = encoded(m);
        const auto replies = co_await send_all(frames);
        if (replies.size() == frames.size() && is(replies.back(), ctl::ack)) {
            co_return Result{.verdict = Verdict::pass,
                             .detail = std::format("{} frames, numbers wrapped", frames.size())};
        }
        co_return Result{.verdict = Verdict::fail,
                         .detail = std::format("frame {} of {} answered with {}", replies.size(),
                                               frames.size(), describe(replies.back()))};
    }

    Result tr06() const {
        if (replies_ == 0) {
            return {.verdict = Verdict::inconclusive, .detail = "no replies measured"};
        }
        const bool ok = max_reply_ < o_.timing.reply_timeout;
        return {.verdict = ok ? Verdict::pass : Verdict::fail,
                .detail = std::format("largest of {} reply times: {} (limit {})", replies_,
                                      ms(max_reply_), ms(o_.timing.reply_timeout)),
                .measured = max_reply_};
    }

    asio::awaitable<Result> tr07() {
        co_await quiet();
        if (!co_await establish()) {
            co_return Result{.detail = "the IUT did not accept ENQ"};
        }
        const auto frames = lis01::frames_for(ordinary_message(o_.iut));
        auto tainted = frames.at(0);
        tainted.text.insert(tainted.text.begin() + 2, ctl::lf);
        co_await write(lis01::encode(tainted));
        const auto r = co_await reply();
        Result result;
        auto rest = encoded(ordinary_message(o_.iut));
        if (is(r, ctl::nak)) {
            result = {.verdict = Verdict::pass, .detail = "NAK"};
        } else if (is(r, ctl::ack)) {
            result = {.verdict = Verdict::pass,
                      .detail = "note: accepted the frame (should be refused)"};
            rest.erase(rest.begin());
        } else {
            result = {.verdict = Verdict::fail,
                      .detail = std::format("answered with {}", describe(r))};
        }
        if (is(r, ctl::nak) || is(r, ctl::ack)) {
            (void)co_await send_all(rest);
        } else {
            co_await write(ctl::eot);
        }
        co_return result;
    }

    // --- IUT as sender -----------------------------------------------------

    Result ts01() const {
        if (frames_seen_ == 0) {
            return {.verdict = Verdict::inconclusive, .detail = "the IUT sent no frames"};
        }
        if (frame_problems_.empty()) {
            return {.verdict = Verdict::pass,
                    .detail = std::format("{} frames checked", frames_seen_)};
        }
        return {.verdict = Verdict::fail,
                .detail = std::format("{} of {} frames: {}", frame_problems_.size(), frames_seen_,
                                      frame_problems_.front())};
    }

    asio::awaitable<Result> ts02() {
        if (!co_await provoke()) {
            co_return no_transfer();
        }
        co_await write(ctl::ack);
        std::optional<lis01::Frame> refused;
        std::optional<bool> same;
        const auto r =
            co_await receive([&](std::size_t i, const In& f) -> std::optional<std::byte> {
                if (i == 0) {
                    if (const auto* first = frame_of(f)) {
                        refused = *first;
                    }
                    return ctl::nak;
                }
                if (i == 1) {
                    const auto* again = frame_of(f);
                    same = again != nullptr && refused && *again == *refused;
                }
                return frame_of(f) != nullptr ? ctl::ack : ctl::nak;
            });
        if (r.frames.empty()) {
            co_return Result{.detail = "the IUT sent no frame after ACK"};
        }
        if (!same) {
            co_return Result{.verdict = Verdict::fail,
                             .detail = "after NAK the IUT ended the transfer instead of repeating"};
        }
        if (!*same || !refused) {
            co_return Result{.verdict = Verdict::fail,
                             .detail = "after NAK a different frame was sent"};
        }
        co_return Result{.verdict = Verdict::pass,
                         .detail = std::format("frame {} repeated", refused->number)};
    }

    asio::awaitable<Result> ts03() {
        if (!co_await provoke()) {
            co_return no_transfer();
        }
        co_await write(ctl::ack);
        const auto limit = static_cast<std::size_t>(o_.timing.max_frame_attempts) + 3;
        const auto r = co_await receive([&](std::size_t i, const In&) -> std::optional<std::byte> {
            return i < limit ? std::optional{ctl::nak} : std::nullopt;
        });
        const auto n = r.frames.size();
        const auto want = static_cast<std::size_t>(o_.timing.max_frame_attempts);
        if (r.eot && n == want) {
            co_return Result{.verdict = Verdict::pass,
                             .detail = std::format("EOT after {} transmissions", n)};
        }
        co_return Result{
            .verdict = Verdict::fail,
            .detail = r.eot ? std::format("EOT after {} transmissions, {} expected", n, want)
                            : std::format("no EOT after {} transmissions", n)};
    }

    asio::awaitable<Result> ts04() {
        if (!co_await provoke()) {
            co_return no_transfer();
        }
        co_await write(ctl::ack);
        const auto r = co_await receive([](std::size_t, const In&) { return std::nullopt; });
        if (r.frames.empty()) {
            co_return Result{.detail = "the IUT sent no frame after ACK"};
        }
        if (r.frames.size() > 1) {
            co_return Result{.verdict = Verdict::fail,
                             .detail = "the IUT sent again without a reply"};
        }
        if (!r.eot) {
            co_return Result{.verdict = Verdict::fail, .detail = "no EOT after the missing reply"};
        }
        const auto t = since(r.frames.front().at, *r.eot);
        const bool ok = within(t, o_.timing.reply_timeout, 2.0);
        co_return Result{
            .verdict = ok ? Verdict::pass : Verdict::fail,
            .detail = std::format("EOT after {} (nominal {})", ms(t), ms(o_.timing.reply_timeout)),
            .measured = t};
    }

    asio::awaitable<Result> ts05() {
        const auto enq = co_await provoke();
        if (!enq) {
            co_return no_transfer();
        }
        co_await write(ctl::nak);
        const auto refused_at = Clock::now();
        const auto again = co_await wait_enq(o_.timing.busy_retry * 3);
        if (!again) {
            co_return Result{.detail =
                                 std::format("no new ENQ within {}", ms(o_.timing.busy_retry * 3))};
        }
        co_await write(ctl::ack);
        (void)co_await receive(ack_all());
        const auto t = since(refused_at, *again);
        const bool ok = within(t, o_.timing.busy_retry);
        co_return Result{.verdict = ok ? Verdict::pass : Verdict::fail,
                         .detail = std::format("next ENQ after {} (at least {})", ms(t),
                                               ms(o_.timing.busy_retry)),
                         .measured = t};
    }

    asio::awaitable<Result> ts06() {
        if (!co_await provoke()) {
            co_return no_transfer();
        }
        co_await write(ctl::enq);  // contention
        const auto contended_at = Clock::now();
        if (iut_wins_contention()) {
            // The IUT keeps priority and asks again after the contention delay.
            const auto again = co_await wait_enq(o_.timing.contention_computer);
            if (!again) {
                co_return Result{.verdict = Verdict::fail,
                                 .detail = "the IUT has priority but did not send ENQ again"};
            }
            co_await write(ctl::ack);
            (void)co_await receive(ack_all());
            const auto t = since(contended_at, *again);
            const bool ok = within(t, o_.timing.contention_instrument);
            co_return Result{.verdict = ok ? Verdict::pass : Verdict::fail,
                             .detail = std::format("ENQ again after {} (at least {})", ms(t),
                                                   ms(o_.timing.contention_instrument)),
                             .measured = t};
        }
        // The IUT yields: it must answer the tester's ENQ with ACK.
        co_await wait(o_.timing.contention_instrument);
        co_await write(ctl::enq);
        const auto r = co_await reply();
        if (!is(r, ctl::ack)) {
            co_await write(ctl::eot);
            co_return Result{
                .verdict = Verdict::fail,
                .detail = std::format("the tester's ENQ after contention: {}", describe(r))};
        }
        (void)co_await send_all(encoded(ordinary_message(o_.iut)));
        co_await quiet();  // the LIS sends its own message afterwards
        co_return Result{.verdict = Verdict::pass,
                         .detail = "backed off and accepted the tester's message"};
    }

    asio::awaitable<Result> ts07() {
        if (!co_await provoke()) {
            co_return no_transfer();
        }
        co_await write(ctl::ack);
        bool single_frame = false;
        const auto r =
            co_await receive([&](std::size_t i, const In& f) -> std::optional<std::byte> {
                if (i == 0) {
                    // ETX only ends a record; the message is complete when the frame holds the L
                    // record.
                    if (const auto* frame = frame_of(f)) {
                        const auto text = lisbench::to_string(frame->text);
                        single_frame =
                            frame->last && (text.starts_with("L|") || text.contains("\rL|"));
                    }
                    return ctl::eot;  // receiver interrupt
                }
                return frame_of(f) != nullptr ? ctl::ack : ctl::nak;
            });
        if (r.frames.empty()) {
            co_return Result{.detail = "the IUT sent no frame after ACK"};
        }
        const bool honoured = r.frames.size() == 1 && r.eot.has_value();
        if (honoured && single_frame) {
            co_return Result{.detail = "single-frame message: an interrupt cannot be told apart"};
        }
        const auto pics = [&](std::string text) {
            if (o_.honours_receiver_interrupt && *o_.honours_receiver_interrupt != honoured) {
                return Result{.verdict = Verdict::fail,
                              .detail = text + "; the profile states the opposite"};
            }
            return Result{.verdict = Verdict::pass, .detail = std::move(text)};
        };
        if (!honoured) {
            co_return pics(std::format("continued after the interrupt ({} frames, permitted)",
                                       r.frames.size()));
        }
        // Stopped: no new ENQ for the hold-off time, unless the tester sends meanwhile.
        const auto again = co_await wait_enq(o_.timing.reply_timeout);
        if (again) {
            co_await write(ctl::ack);
            (void)co_await receive(ack_all());
            co_return Result{.verdict = Verdict::fail,
                             .detail = std::format("stopped, but sent ENQ again after {}",
                                                   ms(since(*r.eot, *again)))};
        }
        co_return pics("stopped and held off");
    }

    io::Stream& stream_;
    const Options& o_;
    Progress progress_;
    asio::experimental::channel<void(asio::error_code, In)> channel_;
    lis01::StreamDecoder decoder_;
    bool closed_{false};
    Clock::time_point sent_at_{};
    milliseconds max_reply_{0};
    std::size_t replies_{0};
    std::size_t frames_seen_{0};
    std::vector<std::string> frame_problems_{};
};

}  // namespace

asio::awaitable<std::vector<Result>> run(io::Stream stream, Options options, Progress progress) {
    if (!progress) {
        progress = [](std::string_view) {};
    }
    Tester tester{stream, options, std::move(progress)};
    // The suite closes the stream at its end, which ends the reader; if the
    // IUT closes first, the remaining cases come back inconclusive.
    co_return co_await (tester.reader() && tester.suite());
}

}  // namespace lisbench::conform

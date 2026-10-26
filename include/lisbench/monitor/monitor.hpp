#pragma once

// What a man-in-the-middle sees on a LIS01-A2 / LIS02-A2 link, decoded.
// Pure: fed with bytes per side and time, it returns events; the I/O lives
// in io::relay and the command line tool.

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "lisbench/io/relay.hpp"
#include "lisbench/lis01_decoder.hpp"
#include "lisbench/lis02_message.hpp"
#include "lisbench/unframed.hpp"
#include "lisbench/watch.hpp"

namespace lisbench::monitor {

using io::Side;
using std::chrono::milliseconds;
using Clock = std::chrono::system_clock;

struct Options {
    std::array<std::string, 2> labels{"A", "B"};
    lis02::TextOptions text{};
    std::vector<watch::Expression> watches{};
    milliseconds slow_reply{15'000};  // LIS01-A2 sender timeout
    std::size_t max_text{lis01::max_text_size};
    // Per side: how the messages are carried. A monitor passes bytes on
    // unchanged, so both sides normally use the same.
    std::array<LowLevel, 2> low_level{LowLevel::lis01_a2, LowLevel::lis01_a2};
};

enum class Kind : std::uint8_t {
    control,     // ENQ ACK NAK EOT
    frame,       // frame accepted by the decoder
    repeated,    // frame identical to the previous one
    bad_frame,   // checksum, layout
    record,      // complete record (decoded in Event::record)
    transfer,    // EOT: summary of the transfer
    problem,     // protocol deviation
    noise,       // bytes outside frames
    watch,       // a watch expression matched
    slow_reply,  // reply took longer than slow_reply
};

[[nodiscard]] std::string_view to_string(Kind k) noexcept;

struct Event {
    Clock::time_point at{};
    Side from{Side::a};
    Kind kind{Kind::control};
    std::string text{};
    std::optional<lis02::Record> record{};
    bytes raw{};                         // record and watch: the record as received, without CR
    std::optional<std::size_t> watch{};  // Kind::watch: index into Options::watches
    std::string value{};                 // Kind::watch: the value that matched
};

struct SideStats {
    std::size_t transfers{0};
    std::size_t incomplete_transfers{0};  // ended without L record
    std::size_t records{0};
    std::size_t frames{0};
    std::size_t repeated_frames{0};
    std::size_t bad_frames{0};
    std::size_t naks_sent{0};
    std::size_t problems{0};
    std::size_t watch_hits{0};
    milliseconds max_reply{0};  // this side answering the other
};

class Monitor {
public:
    explicit Monitor(Options options);

    [[nodiscard]] std::vector<Event> feed(Side from, Clock::time_point at, bytes_view data);
    [[nodiscard]] std::vector<Event> finish(Clock::time_point at);

    [[nodiscard]] const SideStats& stats(Side s) const noexcept { return stats_.at(index(s)); }
    [[nodiscard]] const Options& options() const noexcept { return options_; }

private:
    struct SideState {
        std::variant<lis01::StreamDecoder, UnframedDecoder> decoder;
        lis02::Delimiters delimiters{};
        std::size_t records_in_transfer{0};
        bool saw_terminator{false};
        bool in_transfer{false};
        std::optional<Clock::time_point> waiting_since{};  // sent ENQ or a frame, reply due
    };

    static constexpr std::size_t index(Side s) noexcept { return s == Side::a ? 0 : 1; }
    // Runs step(decoder, events) on this side's decoder and handles the events.
    template <typename Step>
    void run_decoder(Side from, Clock::time_point at, std::vector<Event>& out, Step step) {
        std::visit(
            [&](auto& d) {
                if constexpr (std::is_same_v<std::decay_t<decltype(d)>, UnframedDecoder>) {
                    std::vector<UnframedEvent> decoded;
                    step(d, decoded);
                    for (auto& e : decoded) {
                        on_unframed(from, at, e, out);
                    }
                } else {
                    std::vector<lis01::DecoderEvent> decoded;
                    step(d, decoded);
                    for (auto& e : decoded) {
                        on_decoded(from, at, e, out);
                    }
                }
            },
            sides_.at(index(from)).decoder);
    }
    void on_decoded(Side from, Clock::time_point at, lis01::DecoderEvent& e,
                    std::vector<Event>& out);
    void on_unframed(Side from, Clock::time_point at, UnframedEvent& e, std::vector<Event>& out);
    void on_record(Side from, Clock::time_point at, bytes text, std::vector<Event>& out);
    void reply_from(Side from, Clock::time_point at, std::vector<Event>& out);

    Options options_;
    std::array<SideState, 2> sides_;
    std::array<SideStats, 2> stats_{};
};

struct FormatOptions {
    bool verbose{false};  // also ENQ/ACK/EOT and every frame
    bool decode{false};   // fields of each record by name
};

// Text lines for one event, with time and label; empty if the event is not
// shown at this verbosity.
[[nodiscard]] std::vector<std::string> format(const Event& e, const Options& options,
                                              const FormatOptions& format);

// Summary per side after a session.
[[nodiscard]] std::vector<std::string> summary(const Monitor& m);

}  // namespace lisbench::monitor

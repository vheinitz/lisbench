#pragma once

// Triggers: a watch expression and what to do when it matches.
//
//   R.value > 400 => exec notify-send "glucose high"
//   R.abnormal_flags == HH => mark critical value
//   O.specimen_id == S-1002 => stop
//
// exec   runs the command (POSIX sh -c / Windows cmd /c). The hit is passed in
//        the environment, never pasted into the command: LISBENCH_WATCH,
//        LISBENCH_VALUE, LISBENCH_RECORD, LISBENCH_FROM, LISBENCH_TIME.
// mark   writes a mark into the capture file (--save); default text is the watch
// stop   ends the monitor after the event, so the capture ends there

#include <expected>
#include <string>
#include <string_view>
#include <variant>

#include "lisbench/watch.hpp"

namespace lisbench::monitor {

struct Exec {
    std::string command{};
};
struct Mark {
    std::string text{};
};
struct Stop {};
using Action = std::variant<Exec, Mark, Stop>;

struct Trigger {
    watch::Expression watch{};
    Action action{};
};

[[nodiscard]] std::expected<Trigger, std::string> parse_trigger(std::string_view text);

}  // namespace lisbench::monitor

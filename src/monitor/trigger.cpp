#include "lisbench/monitor/trigger.hpp"

#include <format>

namespace lisbench::monitor {

namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

}  // namespace

std::expected<Trigger, std::string> parse_trigger(std::string_view text) {
    const auto arrow = text.find("=>");
    if (arrow == std::string_view::npos) {
        return std::unexpected(std::format("'{}': expected WATCH => ACTION", text));
    }
    auto w = watch::parse(trim(text.substr(0, arrow)));
    if (!w) {
        return std::unexpected(w.error());
    }
    const auto action = trim(text.substr(arrow + 2));
    const auto space = action.find_first_of(" \t");
    const auto verb = action.substr(0, space);
    const auto rest =
        space == std::string_view::npos ? std::string_view{} : trim(action.substr(space));
    Trigger t{.watch = std::move(*w), .action = Stop{}};
    if (verb == "exec") {
        if (rest.empty()) {
            return std::unexpected("exec needs a command");
        }
        t.action = Exec{.command = std::string{rest}};
    } else if (verb == "mark") {
        t.action = Mark{.text = rest.empty() ? t.watch.text : std::string{rest}};
    } else if (verb == "stop") {
        if (!rest.empty()) {
            return std::unexpected("stop takes no argument");
        }
    } else {
        return std::unexpected(std::format("unknown action '{}' (exec, mark or stop)", verb));
    }
    return t;
}

}  // namespace lisbench::monitor

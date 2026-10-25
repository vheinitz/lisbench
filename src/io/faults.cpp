#include "lisbench/io/faults.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "lisbench/lis01_frame.hpp"

namespace lisbench::io {

namespace {

constexpr std::array target_names{"ack", "nak", "enq", "eot", "frame"};
constexpr std::array action_names{"drop", "nak", "corrupt", "delay"};

bool fires(const Fault& f, std::size_t occurrence) {
    if (f.nth != 0) {
        return occurrence == f.nth;
    }
    if (f.every != 0) {
        return occurrence % f.every == 0;
    }
    return true;
}

}  // namespace

std::optional<FaultTarget> fault_target(std::string_view name) noexcept {
    const auto it = std::ranges::find(target_names, name);
    if (it == target_names.end()) {
        return std::nullopt;
    }
    return static_cast<FaultTarget>(it - target_names.begin());
}

std::optional<FaultAction> fault_action(std::string_view name) noexcept {
    const auto it = std::ranges::find(action_names, name);
    if (it == action_names.end()) {
        return std::nullopt;
    }
    return static_cast<FaultAction>(it - action_names.begin());
}

std::string_view to_string(FaultTarget t) noexcept {
    return target_names.at(static_cast<std::size_t>(t));
}
std::string_view to_string(FaultAction a) noexcept {
    return action_names.at(static_cast<std::size_t>(a));
}

FaultFilter::FaultFilter(std::vector<Fault> faults) : faults_{std::move(faults)} {}

std::optional<FaultTarget> FaultFilter::classify(bytes_view data) noexcept {
    if (data.empty()) {
        return std::nullopt;
    }
    if (data.front() == lis01::ctl::stx) {
        return FaultTarget::frame;
    }
    if (data.size() != 1) {
        return std::nullopt;
    }
    switch (std::to_integer<unsigned char>(data.front())) {
        case 0x06: return FaultTarget::ack;
        case 0x15: return FaultTarget::nak;
        case 0x05: return FaultTarget::enq;
        case 0x04: return FaultTarget::eot;
        default: return std::nullopt;
    }
}

std::optional<Outgoing> FaultFilter::apply(bytes data) {
    Outgoing out{.data = std::move(data), .delay = milliseconds{0}, .applied = std::nullopt};
    const auto target = classify(out.data);
    if (!target) {
        return out;
    }
    const auto occurrence = ++seen_.at(static_cast<std::size_t>(*target));
    const auto it = std::ranges::find_if(
        faults_, [&](const Fault& f) { return f.target == *target && fires(f, occurrence); });
    if (it == faults_.end()) {
        return out;
    }
    out.applied = *it;
    switch (it->action) {
        case FaultAction::drop: return std::nullopt;
        case FaultAction::nak: out.data = bytes{lis01::ctl::nak}; break;
        case FaultAction::corrupt:
            if (*target == FaultTarget::frame && out.data.size() >= lis01::frame_overhead) {
                // Change the second checksum digit: the frame stays well formed
                // and only the checksum check can catch it.
                auto& digit = out.data[out.data.size() - 3];
                digit = digit == std::byte{'0'} ? std::byte{'1'} : std::byte{'0'};
            } else {
                out.data = bytes{std::byte{'?'}};
            }
            break;
        case FaultAction::delay: out.delay = it->delay; break;
    }
    return out;
}

std::expected<Fault, std::string> parse_fault(std::string_view spec) {
    std::vector<std::string> parts;
    for (auto p : spec | std::views::split(':')) {
        parts.emplace_back(p.begin(), p.end());
    }
    if (parts.size() < 3) {
        return std::unexpected(std::format("fault '{}': expected on:which:do[:ms]", spec));
    }
    const auto target = fault_target(parts[0]);
    const auto action = fault_action(parts[2]);
    if (!target || !action) {
        return std::unexpected(std::format("fault '{}': unknown target or action", spec));
    }
    Fault f{.target = *target, .action = *action};
    try {
        if (parts[1].starts_with("every")) {
            f.every = std::stoul(parts[1].substr(5));
        } else if (parts[1] != "all") {
            f.nth = std::stoul(parts[1]);
        }
        if (f.action == FaultAction::delay) {
            f.delay = std::chrono::milliseconds{parts.size() > 3 ? std::stol(parts[3]) : 20'000};
        }
    } catch (const std::exception&) {
        return std::unexpected(std::format("fault '{}': bad number", spec));
    }
    return f;
}

}  // namespace lisbench::io

#pragma once

// Runs a scenario against a real peer.

#include <asio.hpp>

#include <functional>
#include <string>
#include <vector>

#include "lisbench/io/session.hpp"
#include "lisbench/sim/scenario.hpp"

namespace lisbench::sim {

struct StepReport {
    std::size_t index{};  // 0-based; connection problems use the step that was due
    bool ok{};
    std::string text{};
};

struct Report {
    bool ok{true};
    std::vector<StepReport> steps{};
    std::size_t connections{0};
};

struct Observers {
    io::SessionHandlers session{};
    std::function<void(const StepReport&)> on_step;
    std::function<void(const std::string&)> on_info;
};

// For tcp-listen endpoints an acceptor can be passed in (tests bind port 0
// first and connect to the port they got); otherwise one is created.
asio::awaitable<Report> run(Scenario scenario, Observers observers,
                            asio::ip::tcp::acceptor* acceptor = nullptr);

}  // namespace lisbench::sim

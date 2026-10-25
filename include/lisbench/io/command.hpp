#pragma once

// External commands for monitor triggers. They run one after the other on a
// worker thread, so a slow command never holds up the link; a full queue
// refuses new work instead of blocking or growing without bound.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

namespace lisbench::io {

struct Command {
    std::string command{};                     // POSIX: sh -c, Windows: cmd /c
    std::map<std::string, std::string> env{};  // added to the inherited environment
};

struct CommandResult {
    Command command{};
    int exit_code{-1};    // -1: could not be started or did not exit normally
    std::string error{};  // why it could not be started
};

// Runs one command and waits for it.
[[nodiscard]] CommandResult run_command(const Command& c);

class CommandQueue {
public:
    using Done = std::function<void(const CommandResult&)>;  // called on the worker thread

    CommandQueue(std::size_t capacity, Done done);
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;
    CommandQueue(CommandQueue&&) = delete;
    CommandQueue& operator=(CommandQueue&&) = delete;
    ~CommandQueue() = default;  // the jthread asks the worker to stop and joins

    // false: the queue is full, the command is not run.
    [[nodiscard]] bool submit(Command c);
    // Waits until every submitted command has finished.
    void drain();

private:
    void work(const std::stop_token& stop);

    std::size_t capacity_;
    Done done_;
    std::mutex mutex_{};
    std::condition_variable_any wake_{};
    std::condition_variable idle_{};
    std::deque<Command> queue_{};
    bool busy_{false};
    std::jthread worker_;  // last: started after everything it uses exists
};

}  // namespace lisbench::io

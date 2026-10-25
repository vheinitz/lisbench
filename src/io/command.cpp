#include "lisbench/io/command.hpp"

#include <cerrno>
#include <cstring>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <stdlib.h>
#else
#include <spawn.h>
#include <sys/wait.h>
// POSIX defines environ but no header declares it; it is the C runtime's, not ours.
// NOLINTNEXTLINE(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables)
extern char** environ;
#endif

namespace lisbench::io {

namespace {

// The inherited environment with the command's variables replacing any of
// the same name, as "NAME=value" strings.
std::vector<std::string> environment_for(const Command& c) {
    std::vector<std::string> out;
#if defined(_WIN32)
    char** env = _environ;
#else
    char** env = environ;
#endif
    for (; env != nullptr && *env != nullptr; ++env) {  // NOLINT(*-pointer-arithmetic): C array
        const std::string_view entry{*env};
        const auto name = entry.substr(0, entry.find('='));
        if (!c.env.contains(std::string{name})) {
            out.emplace_back(entry);
        }
    }
    for (const auto& [name, value] : c.env) {
        out.push_back(std::format("{}={}", name, value));
    }
    return out;
}

std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (auto& s : strings) {
        out.push_back(s.data());
    }
    out.push_back(nullptr);
    return out;
}

}  // namespace

CommandResult run_command(const Command& c) {
    CommandResult result{.command = c};
    auto env = environment_for(c);
    auto envp = pointers(env);
#if defined(_WIN32)
    const char* comspec = std::getenv("COMSPEC");  // NOLINT(concurrency-mt-unsafe): read only
    std::vector<std::string> args{"cmd", "/c", c.command};
    auto argv = pointers(args);
    const auto rc =
        _spawnve(_P_WAIT, comspec != nullptr ? comspec : "cmd.exe", argv.data(), envp.data());
    if (rc == -1) {
        result.error = std::strerror(errno);  // NOLINT(concurrency-mt-unsafe): message only
        return result;
    }
    result.exit_code = static_cast<int>(rc);
#else
    std::vector<std::string> args{"sh", "-c", c.command};
    auto argv = pointers(args);
    pid_t pid = 0;
    if (const int rc = ::posix_spawn(&pid, "/bin/sh", nullptr, nullptr, argv.data(), envp.data());
        rc != 0) {
        result.error = std::strerror(rc);  // NOLINT(concurrency-mt-unsafe): message only
        return result;
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) {
            result.error = std::strerror(errno);  // NOLINT(concurrency-mt-unsafe): message only
            return result;
        }
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
#endif
    return result;
}

CommandQueue::CommandQueue(std::size_t capacity, Done done)
    : capacity_{capacity},
      done_{std::move(done)},
      worker_{[this](const std::stop_token& stop) { work(stop); }} {}

bool CommandQueue::submit(Command c) {
    {
        const std::scoped_lock lock{mutex_};
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(c));
    }
    wake_.notify_one();
    return true;
}

void CommandQueue::drain() {
    std::unique_lock lock{mutex_};
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void CommandQueue::work(const std::stop_token& stop) {
    while (true) {
        Command c;
        {
            std::unique_lock lock{mutex_};
            if (!wake_.wait(lock, stop, [this] { return !queue_.empty(); })) {
                return;  // stop requested and nothing left
            }
            c = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        const auto result = run_command(c);
        if (done_) {
            done_(result);
        }
        {
            const std::scoped_lock lock{mutex_};
            busy_ = false;
        }
        idle_.notify_all();
    }
}

}  // namespace lisbench::io

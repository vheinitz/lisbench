// lisbench run: run session files without the web UI (docs/spec/features.md, 9.1).

#include "cmd_run.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <csignal>
#include <filesystem>
#include <map>
#include <utility>

#include "common.hpp"
#include "lisbench/session/manager.hpp"

namespace lisbench::cli {

int cmd_run(const RunFlags& flags) {
    namespace fs = std::filesystem;
    // A session file lives in <data>/sessions/; the data directory holds the catalogue.
    std::vector<std::string> names;
    fs::path data;
    for (const auto& f : flags.files) {
        const fs::path p = fs::absolute(f);
        if (p.extension() != ".yaml" || p.parent_path().filename() != "sessions") {
            println(stderr, "run: {} is not a session file (<data>/sessions/<name>.yaml)", f);
            return 2;
        }
        const auto dir = p.parent_path().parent_path();
        if (!data.empty() && dir != data) {
            println(stderr, "run: all session files must be in the same data directory");
            return 2;
        }
        data = dir;
        names.push_back(p.stem().string());
    }

    asio::io_context io;
    std::map<std::string, std::string> last_link;
    session::Manager manager{
        io.get_executor(),
        {.data_dir = data, .profile_search = profile_search_path()},
        [&last_link](std::string_view s, std::string_view kind, const nlohmann::json& e) {
            if (kind == "log") {
                println("{} [{}] {}", e.value("t", ""), s, e.value("text", ""));
                for (const auto& line : e.value("lines", nlohmann::json::array())) {
                    println("{:>14} {}", "", line.get<std::string>());
                }
            } else if (kind == "status") {
                // Link changes only, each once.
                for (const auto& l : e["links"]) {
                    const auto key = std::string{s} + "/" + l.value("role", "");
                    const auto text =
                        std::format("{} {}", l.value("state", ""), l.value("detail", ""));
                    if (std::exchange(last_link[key], text) != text) {
                        println("{} [{}] link {}: {}", clock_now(), s, l.value("role", ""), text);
                    }
                }
            }
        }};
    for (const auto& n : names) {
        if (auto r = manager.start(n); !r) {
            println(stderr, "run: {}: {}", n, r.error());
            manager.stop_all();
            io.run_for(std::chrono::seconds{1});
            return 2;
        }
    }
    asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&](const std::error_code&, int) { manager.stop_all(); });
    // Ends when every session has stopped (Ctrl+C, or all of them finished).
    asio::steady_timer poll{io};
    std::function<void()> check = [&] {
        poll.expires_after(std::chrono::milliseconds{200});
        poll.async_wait([&](const std::error_code& ec) {
            if (ec) {
                return;
            }
            if (std::ranges::none_of(names, [&](const auto& n) { return manager.running(n); })) {
                signals.cancel();
                return;
            }
            check();
        });
    };
    check();
    io.run();
    return 0;
}

}  // namespace lisbench::cli

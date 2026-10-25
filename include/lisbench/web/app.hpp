#pragma once

// The web UI's API on top of web::Server: monitor, captures, conformance
// test and simulator, each started and stopped from the browser, all on the
// server's event loop. Live data goes out as events on /api/events:
//
//   status     {"monitor":{...},"sim":{...},"conform":{...}}   after every change
//   monitor    {"t","from","kind","lines":[...]}                one per monitor event
//   summary    {"source":"monitor"|"capture","lines":[...]}     at the end
//   conform    {"line"}                                         progress
//   result     {"results":[...],"summary"}                      conformance report
//   sim        {"t","text","lines":[...]}                       simulator log
//   problem    {"source","text"}                                a task failed
//
// Files (captures, message files, scenarios) come only from the data
// directory, by plain file name. Triggers that run commands are not offered
// to the browser: whoever reaches the page could run programs here.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lisbench/web/server.hpp"

namespace lisbench::web {

struct AppOptions {
    std::string version{};
    std::filesystem::path data_dir{};  // empty: no captures and files
    std::vector<std::filesystem::path> profile_search{};
};

class App {
public:
    App(Server& server, AppOptions options);
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;
    ~App();

    // Stops whatever runs (monitor, simulator, conformance test).
    void stop_all();

    struct State;  // public for the implementation's free functions

private:
    std::unique_ptr<State> state_;
};

// A plain file name inside the data directory: letters, digits, '.', '_', '-',
// not starting with '.', with one of the given extensions.
[[nodiscard]] bool valid_file_name(std::string_view name,
                                   std::initializer_list<std::string_view> extensions);

}  // namespace lisbench::web

// lisbench serve: the web UI daemon (see docs/spec/features.md, section 8).

#include "cmd_serve.hpp"

#include <asio.hpp>

#include <csignal>
#include <filesystem>

#include "common.hpp"
#include "lisbench/web/app.hpp"
#include "lisbench/web/assets.hpp"
#include "lisbench/web/server.hpp"

namespace lisbench::cli {

int cmd_serve(const ServeFlags& flags) {
    const auto colon = flags.http.rfind(':');
    if (colon == std::string::npos) {
        println(stderr, "serve: --http needs host:port, e.g. 127.0.0.1:8080");
        return 2;
    }
    const auto host = flags.http.substr(0, colon);
    std::error_code ec;
    const auto address = asio::ip::make_address(host.empty() ? "0.0.0.0" : host, ec);
    if (ec) {
        println(stderr, "serve: '{}' is not an IP address", host);
        return 2;
    }
    int port = 0;
    try {
        port = std::stoi(flags.http.substr(colon + 1));
    } catch (const std::exception&) {
        port = -1;
    }
    if (port < 0 || port > 65535) {
        println(stderr, "serve: bad port in '{}'", flags.http);
        return 2;
    }
    std::filesystem::path data;
    if (!flags.captures.empty()) {
        data = std::filesystem::absolute(flags.captures);
        if (!std::filesystem::is_directory(data, ec)) {
            println(stderr, "serve: {} is not a directory", data.string());
            return 2;
        }
    }

    asio::io_context io;
    web::Server server{io.get_executor(), {address, static_cast<std::uint16_t>(port)}};
    for (const auto& a : web::assets()) {
        server.file(std::string{a.path}, std::string{a.content_type}, a.data);
    }
    web::App app{
        server,
        {.version = LISBENCH_VERSION, .data_dir = data, .profile_search = profile_search_path()}};

    println("lisbench {} web UI on http://{}:{}/", LISBENCH_VERSION,
            address.is_unspecified() ? "<this host>" : address.to_string(), server.port());
    println("files (captures, messages, scenarios): {}",
            data.empty() ? "none (use --captures DIR)" : data.string());
    if (!address.is_loopback()) {
        println(stderr,
                "warning: listening beyond this computer; anyone who reaches {} can start "
                "connections from here (no authentication)",
                flags.http);
    }

    asio::signal_set signals{io, SIGINT, SIGTERM};
    signals.async_wait([&](const std::error_code&, int) {
        app.stop_all();
        server.stop();
        io.stop();
    });
    asio::co_spawn(io, server.run(), asio::detached);
    io.run();
    return 0;
}

}  // namespace lisbench::cli

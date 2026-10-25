#pragma once

// Helpers shared by the lisbench subcommands.

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

#include "lisbench/lis02_codec.hpp"

namespace lisbench::cli {

// std::println needs GCC 14; this is the one stand-in until GCC 13 is dropped.
template <typename... Args>
inline void println(std::FILE* to, std::format_string<Args...> fmt, Args&&... args) {
    const auto text = std::format(fmt, std::forward<Args>(args)...) + '\n';
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);  // logs must be readable while a simulator runs, also when redirected
}
template <typename... Args>
inline void println(std::format_string<Args...> fmt, Args&&... args) {
    println(stdout, fmt, std::forward<Args>(args)...);
}

inline std::string read_input(const std::string& path) {
    if (path == "-") {
        return {std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw CLI::ValidationError("input", "cannot open " + path);
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

struct TextFlags {
    std::string charset{"latin1"};
    std::string non_ascii{"ascii-hex"};
    std::string local_escape{"keep"};
    bool strict{false};

    void add_to(CLI::App& cmd) {
        cmd.add_option("--charset", charset, "Wire character set")
            ->check(CLI::IsMember({"latin1", "utf8"}))
            ->capture_default_str();
        cmd.add_option("--non-ascii", non_ascii, "Non-ASCII handling when encoding")
            ->check(CLI::IsMember({"ascii-hex", "raw", "transliterate"}))
            ->capture_default_str();
        cmd.add_option("--local-escape", local_escape,
                       "Local escape Z..: keep literally, or utf16-hex (code units in hex)")
            ->check(CLI::IsMember({"keep", "utf16-hex"}))
            ->capture_default_str();
        cmd.add_flag("--strict", strict, "Reject unknown escapes, record types, invalid bytes");
    }

    [[nodiscard]] lis02::TextOptions options() const {
        static const std::map<std::string, lis02::NonAscii> modes{
            {"ascii-hex", lis02::NonAscii::ascii_hex},
            {"raw", lis02::NonAscii::raw},
            {"transliterate", lis02::NonAscii::transliterate},
        };
        return {.charset = charset == "utf8" ? lis02::Charset::utf8 : lis02::Charset::latin1,
                .non_ascii = modes.at(non_ascii),
                .local_escape = local_escape == "utf16-hex" ? lis02::LocalEscape::utf16_hex
                                                            : lis02::LocalEscape::keep,
                .strict = strict};
    }
};

inline std::string clock_now() {
    const auto now =
        std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
    return std::format("{:%T}", now);
}

}  // namespace lisbench::cli

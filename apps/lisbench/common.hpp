#pragma once

// Helpers shared by the lisbench subcommands.

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lisbench/lis02_codec.hpp"
#include "lisbench/sim/profile.hpp"

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

// Where --profile looks for an instrument id: $LISBENCH_DEVICES (a path
// list), then the profiles of the source tree this binary was built from.
inline std::vector<std::filesystem::path> profile_search_path() {
    std::vector<std::filesystem::path> out;
    if (const char* env = std::getenv("LISBENCH_DEVICES");
        env != nullptr) {  // NOLINT(concurrency-mt-unsafe): read at start
#if defined(_WIN32)
        constexpr char sep = ';';
#else
        constexpr char sep = ':';
#endif
        for (auto part : std::string_view{env} | std::views::split(sep)) {
            if (!part.empty()) {
                out.emplace_back(std::string_view{part.begin(), part.end()});
            }
        }
    }
#ifdef LISBENCH_DEVICES_DIR
    out.emplace_back(LISBENCH_DEVICES_DIR);
#endif
    return out;
}

struct TextFlags {
    std::string charset{"latin1"};
    std::string non_ascii{"ascii-hex"};
    std::string local_escape{"keep"};
    std::string profile;
    bool strict{false};
    std::optional<sim::Profile> loaded{};

    void add_to(CLI::App& cmd) {
        given_.push_back(cmd.add_option("--charset", charset, "Wire character set")
                             ->check(CLI::IsMember({"latin1", "utf8"}))
                             ->capture_default_str());
        given_.push_back(
            cmd.add_option("--non-ascii", non_ascii, "Non-ASCII handling when encoding")
                ->check(CLI::IsMember({"ascii-hex", "raw", "transliterate"}))
                ->capture_default_str());
        given_.push_back(
            cmd.add_option("--local-escape", local_escape,
                           "Local escape Z..: keep literally, or utf16-hex (code units in hex)")
                ->check(CLI::IsMember({"keep", "utf16-hex"}))
                ->capture_default_str());
        cmd.add_option("--profile", profile,
                       "Instrument profile (id such as cepheid-genexpert, or a profile.yaml): "
                       "sets charset, non-ASCII and local escape unless given here");
        cmd.add_flag("--strict", strict, "Reject unknown escapes, record types, invalid bytes");
    }

    // Loads --profile, if given. Call once after parsing.
    [[nodiscard]] std::expected<void, std::string> resolve() {
        if (profile.empty()) {
            return {};
        }
        auto p = sim::load_profile(profile, profile_search_path());
        if (!p) {
            return std::unexpected(p.error());
        }
        loaded = std::move(*p);
        return {};
    }

    [[nodiscard]] lis02::TextOptions options() const {
        static const std::map<std::string, lis02::NonAscii> modes{
            {"ascii-hex", lis02::NonAscii::ascii_hex},
            {"raw", lis02::NonAscii::raw},
            {"transliterate", lis02::NonAscii::transliterate},
        };
        auto o = loaded ? loaded->text : lis02::TextOptions{};
        // Without a profile every option counts; with one only those given on the command line.
        const auto use = [this](std::size_t i) {
            if (!loaded) {
                return true;
            }
            // add_to may have run for several subcommands; only the parsed one has a count.
            for (std::size_t k = i; k < given_.size(); k += 3) {
                if (given_[k]->count() > 0) {
                    return true;
                }
            }
            return false;
        };
        if (use(0)) {
            o.charset = charset == "utf8" ? lis02::Charset::utf8 : lis02::Charset::latin1;
        }
        if (use(1)) {
            o.non_ascii = modes.at(non_ascii);
        }
        if (use(2)) {
            o.local_escape = local_escape == "utf16-hex" ? lis02::LocalEscape::utf16_hex
                                                         : lis02::LocalEscape::keep;
        }
        o.strict = strict;
        return o;
    }

private:
    std::vector<CLI::Option*> given_{};  // charset, non-ascii, local-escape per subcommand
};

inline std::string clock_at(std::chrono::system_clock::time_point t) {
    return std::format("{:%T}", std::chrono::floor<std::chrono::milliseconds>(t));
}

inline std::string clock_now() {
    return clock_at(std::chrono::system_clock::now());
}

}  // namespace lisbench::cli

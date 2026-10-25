#include "lisbench/sim/profile.hpp"

#include <yaml-cpp/yaml.h>

#include <format>
#include <fstream>
#include <iterator>

namespace lisbench::sim {

namespace {

std::string at(const YAML::Node& node, std::string_view text) {
    return std::format("line {}: {}", node.Mark().line + 1, text);
}

}  // namespace

std::expected<Profile, std::string> parse_profile(std::string_view yaml,
                                                  std::filesystem::path file) {
    try {
        const auto root = YAML::Load(std::string{yaml});
        if (!root.IsMap() || !root["id"]) {
            return std::unexpected("a profile needs 'id'");
        }
        Profile p{.id = root["id"].as<std::string>(),
                  .vendor = root["vendor"] ? root["vendor"].as<std::string>() : "",
                  .model = root["model"] ? root["model"].as<std::string>() : "",
                  .file = std::move(file)};
        if (const auto l1 = root["lis01"]) {
            if (const auto n = l1["max_text"]) {
                const auto v = n.as<std::size_t>();
                if (v == 0 || v > lis01::max_text_size) {
                    return std::unexpected(
                        at(n, std::format("max_text is 1..{}", lis01::max_text_size)));
                }
                p.max_text = v;
            }
            if (const auto n = l1["contention_winner"]) {
                const auto v = n.as<std::string>();
                if (v == "instrument") {
                    p.contention_winner = lis01::Role::instrument;
                } else if (v == "computer") {
                    p.contention_winner = lis01::Role::computer;
                } else if (v != "unclear") {
                    return std::unexpected(
                        at(n, "contention_winner is instrument, computer or unclear"));
                }
            }
        }
        if (const auto l2 = root["lis02"]) {
            if (const auto n = l2["charset"]) {
                const auto v = n.as<std::string>();
                if (v != "latin1" && v != "utf8") {
                    return std::unexpected(at(n, "charset is latin1 or utf8"));
                }
                p.text.charset = v == "utf8" ? lis02::Charset::utf8 : lis02::Charset::latin1;
            }
            if (const auto n = l2["non_ascii"]) {
                const auto v = n.as<std::string>();
                if (v == "ascii-hex") {
                    p.text.non_ascii = lis02::NonAscii::ascii_hex;
                } else if (v == "raw") {
                    p.text.non_ascii = lis02::NonAscii::raw;
                } else if (v == "transliterate") {
                    p.text.non_ascii = lis02::NonAscii::transliterate;
                } else {
                    return std::unexpected(at(n, "non_ascii is ascii-hex, raw or transliterate"));
                }
            }
            if (const auto n = l2["local_escape"]) {
                const auto v = n.as<std::string>();
                if (v != "keep" && v != "utf16-hex") {
                    return std::unexpected(at(n, "local_escape is keep or utf16-hex"));
                }
                p.text.local_escape =
                    v == "utf16-hex" ? lis02::LocalEscape::utf16_hex : lis02::LocalEscape::keep;
            }
        }
        return p;
    } catch (const YAML::Exception& e) {
        return std::unexpected(std::format("line {}: {}", e.mark.line + 1, e.msg));
    }
}

std::expected<Profile, std::string> load_profile(std::string_view name_or_path,
                                                 const std::vector<std::filesystem::path>& search) {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates{fs::path{name_or_path},
                                     fs::path{name_or_path} / "profile.yaml"};
    // An id has no path separator; only then is it looked up in the search directories.
    if (name_or_path.find_first_of("/\\") == std::string_view::npos) {
        for (const auto& dir : search) {
            candidates.push_back(dir / name_or_path / "profile.yaml");
        }
    }
    for (const auto& c : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(c, ec)) {
            continue;
        }
        std::ifstream in(c, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
        auto p = parse_profile(text, c);
        if (!p) {
            return std::unexpected(std::format("{}: {}", c.string(), p.error()));
        }
        return p;
    }
    return std::unexpected(std::format("profile '{}' not found", name_or_path));
}

void apply(const Profile& p, lis01::Timing& timing) {
    if (p.contention_winner) {
        timing.contention_winner = *p.contention_winner;
    }
    if (p.max_text) {
        timing.max_text = *p.max_text;
    }
}

}  // namespace lisbench::sim

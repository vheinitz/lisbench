#pragma once

// Instrument profiles (docs/devices/<id>/profile.yaml): the machine-readable
// part that lisbench uses. Read are
//
//   lis01: {max_text: 240, contention_winner: instrument | computer | unclear,
//           honours_receiver_interrupt: true | false}
//   lis02: {charset: latin1 | utf8, non_ascii: ascii-hex | raw | transliterate,
//           local_escape: keep | utf16-hex}
//
// Everything else in the file documents the instrument and is not used here.

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/lis01_link.hpp"
#include "lisbench/lis02_codec.hpp"

namespace lisbench::sim {

struct Profile {
    std::string id{};
    std::string vendor{};
    std::string model{};
    lis02::TextOptions text{};
    std::optional<lis01::Role> contention_winner{};  // unset: the profile does not know
    std::optional<std::size_t> max_text{};
    std::optional<bool> honours_receiver_interrupt{};  // PICS for lisbench conform
    std::filesystem::path file{};
};

[[nodiscard]] std::expected<Profile, std::string> parse_profile(std::string_view yaml,
                                                                std::filesystem::path file);

// `name_or_path` is a profile.yaml, a directory holding one, or a profile id
// looked up as <dir>/<id>/profile.yaml in each of `search`.
[[nodiscard]] std::expected<Profile, std::string> load_profile(
    std::string_view name_or_path, const std::vector<std::filesystem::path>& search);

// Timing values the profile knows; the rest stays as it is.
void apply(const Profile& p, lis01::Timing& timing);

}  // namespace lisbench::sim

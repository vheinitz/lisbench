#pragma once

// CLSI LIS02-A2 delimiters, escape sequences and character set handling.
//
// Values inside the library are UTF-8. On the wire, the standard assumes
// 7-bit ASCII; 8-bit data only by agreement between the partners. Escapes:
//   &F& field   &S& component   &R& repeat   &E& escape   &Xhh..& hex bytes
// (shown with '&' as escape delimiter; the real delimiters come from the
// header record).
//
// How non-ASCII text is put on the wire is a per-installation decision and
// must be stated in the interface description towards the LIS operator:
//   ascii_hex      &Xhh..& over the bytes of the wire charset (stays 7-bit)
//   raw            the bytes of the wire charset unescaped (8-bit on the wire)
//   transliterate  nearest ASCII (é -> e, ä -> ae); unmappable characters are
//                  dropped and reported

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace lisbench::lis02 {

struct Delimiters {
    char field{'|'};
    char repeat{'\\'};
    char component{'^'};
    char escape{'&'};

    friend bool operator==(const Delimiters&, const Delimiters&) = default;

    // The four characters after "H" in a header record, e.g. "|\^&".
    [[nodiscard]] std::string definition() const;
};

enum class Charset : std::uint8_t { latin1, utf8 };
enum class NonAscii : std::uint8_t { ascii_hex, raw, transliterate };

struct TextOptions {
    Charset charset{Charset::latin1};
    NonAscii non_ascii{NonAscii::ascii_hex};
    // Strict: unknown or unterminated escapes and invalid bytes are errors.
    // Lenient: they are kept literally (or replaced by U+FFFD).
    bool strict{false};
};

struct CodecError {
    std::size_t offset;  // byte offset in the input
    std::string text;
};

// Reads the delimiters from the start of a header record ("H|\^&...").
// All four must be distinct and not alphanumeric.
[[nodiscard]] std::expected<Delimiters, CodecError> delimiters_from_header(std::string_view record);

struct Encoded {
    std::string wire;
    std::vector<char32_t> dropped;  // transliterate: characters without ASCII form
};

// UTF-8 value -> wire text of one component (delimiters escaped).
[[nodiscard]] std::expected<Encoded, CodecError> encode(std::string_view utf8, const Delimiters& d,
                                                        const TextOptions& options);

// Wire text of one component -> UTF-8 value (escapes resolved).
[[nodiscard]] std::expected<std::string, CodecError> decode(std::string_view wire,
                                                            const Delimiters& d,
                                                            const TextOptions& options);

// ASCII replacement for a code point, empty if none is known.
[[nodiscard]] std::string_view transliteration(char32_t cp) noexcept;

// Minimal UTF-8 helpers used by the codec and the tools.
[[nodiscard]] std::expected<std::vector<char32_t>, CodecError> utf8_decode(std::string_view text);
void utf8_append(std::string& out, char32_t cp);

}  // namespace lisbench::lis02

#include "lisbench/lis02_codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <ranges>
#include <utility>

#include "lisbench/compat.hpp"

namespace lisbench::lis02 {

namespace {

constexpr char32_t replacement_char = 0xFFFD;

bool is_printable_ascii(char32_t cp) {
    return cp >= 0x20 && cp <= 0x7E;
}

void append_hex_escape(std::string& out, const std::vector<std::uint8_t>& bytes, char esc) {
    out += esc;
    out += 'X';
    for (const auto b : bytes) {
        out += std::format("{:02X}", b);
    }
    out += esc;
}

std::vector<std::uint8_t> charset_bytes(char32_t cp, Charset charset) {
    if (charset == Charset::latin1) {
        if (cp > 0xFF) {
            return {};
        }
        return {static_cast<std::uint8_t>(cp)};
    }
    std::string tmp;
    utf8_append(tmp, cp);
    return compat::to_vector(
        tmp | std::views::transform([](char c) { return static_cast<std::uint8_t>(c); }));
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// Pairs of code point and ASCII form. German umlauts follow the usual
// two-letter convention (DIN 5007-2), other letters lose their diacritics.
constexpr auto transliterations = std::to_array<std::pair<char32_t, std::string_view>>({
    {0x00A0, " "},   {0x00A1, "!"},   {0x00A2, "c"},   {0x00A3, "GBP"}, {0x00A5, "JPY"},
    {0x00A7, "S"},   {0x00A9, "(c)"}, {0x00AB, "<<"},  {0x00AD, "-"},   {0x00AE, "(R)"},
    {0x00B0, "deg"}, {0x00B1, "+-"},  {0x00B2, "2"},   {0x00B3, "3"},   {0x00B5, "u"},
    {0x00B7, "."},   {0x00B9, "1"},   {0x00BB, ">>"},  {0x00BC, "1/4"}, {0x00BD, "1/2"},
    {0x00BE, "3/4"}, {0x00BF, "?"},   {0x00C0, "A"},   {0x00C1, "A"},   {0x00C2, "A"},
    {0x00C3, "A"},   {0x00C4, "Ae"},  {0x00C5, "A"},   {0x00C6, "AE"},  {0x00C7, "C"},
    {0x00C8, "E"},   {0x00C9, "E"},   {0x00CA, "E"},   {0x00CB, "E"},   {0x00CC, "I"},
    {0x00CD, "I"},   {0x00CE, "I"},   {0x00CF, "I"},   {0x00D0, "D"},   {0x00D1, "N"},
    {0x00D2, "O"},   {0x00D3, "O"},   {0x00D4, "O"},   {0x00D5, "O"},   {0x00D6, "Oe"},
    {0x00D7, "x"},   {0x00D8, "O"},   {0x00D9, "U"},   {0x00DA, "U"},   {0x00DB, "U"},
    {0x00DC, "Ue"},  {0x00DD, "Y"},   {0x00DE, "Th"},  {0x00DF, "ss"},  {0x00E0, "a"},
    {0x00E1, "a"},   {0x00E2, "a"},   {0x00E3, "a"},   {0x00E4, "ae"},  {0x00E5, "a"},
    {0x00E6, "ae"},  {0x00E7, "c"},   {0x00E8, "e"},   {0x00E9, "e"},   {0x00EA, "e"},
    {0x00EB, "e"},   {0x00EC, "i"},   {0x00ED, "i"},   {0x00EE, "i"},   {0x00EF, "i"},
    {0x00F0, "d"},   {0x00F1, "n"},   {0x00F2, "o"},   {0x00F3, "o"},   {0x00F4, "o"},
    {0x00F5, "o"},   {0x00F6, "oe"},  {0x00F7, "/"},   {0x00F8, "o"},   {0x00F9, "u"},
    {0x00FA, "u"},   {0x00FB, "u"},   {0x00FC, "ue"},  {0x00FD, "y"},   {0x00FE, "th"},
    {0x00FF, "y"},   {0x0152, "OE"},  {0x0153, "oe"},  {0x0160, "S"},   {0x0161, "s"},
    {0x017D, "Z"},   {0x017E, "z"},   {0x2013, "-"},   {0x2014, "-"},   {0x2018, "'"},
    {0x2019, "'"},   {0x201A, "'"},   {0x201C, "\""},  {0x201D, "\""},  {0x201E, "\""},
    {0x2022, "*"},   {0x2026, "..."}, {0x20AC, "EUR"}, {0x2122, "TM"},  {0x03BC, "u"},
});

}  // namespace

std::string Delimiters::definition() const {
    return {field, repeat, component, escape};
}

std::expected<Delimiters, CodecError> delimiters_from_header(std::string_view record) {
    if (record.size() < 5 || record[0] != 'H') {
        return std::unexpected(
            CodecError{.offset = 0, .text = "header record too short for a delimiter definition"});
    }
    const Delimiters d{
        .field = record[1], .repeat = record[2], .component = record[3], .escape = record[4]};
    const std::array chars{d.field, d.repeat, d.component, d.escape};
    for (std::size_t i = 0; i < chars.size(); ++i) {
        const auto c = static_cast<unsigned char>(chars.at(i));
        if (std::isalnum(c) != 0 || c < 0x20 || c > 0x7E) {
            return std::unexpected(
                CodecError{.offset = i + 1, .text = std::format("invalid delimiter 0x{:02X}", c)});
        }
        if (std::ranges::count(chars, chars.at(i)) != 1) {
            return std::unexpected(
                CodecError{.offset = i + 1, .text = "delimiters must be distinct"});
        }
    }
    return d;
}

std::string_view transliteration(char32_t cp) noexcept {
    const auto* const it =
        std::ranges::find(transliterations, cp, &std::pair<char32_t, std::string_view>::first);
    return it == transliterations.end() ? std::string_view{} : it->second;
}

void utf8_append(std::string& out, char32_t cp) {
    const auto put = [&out](std::uint32_t v) {
        out += static_cast<char>(static_cast<std::uint8_t>(v));
    };
    const auto v = static_cast<std::uint32_t>(cp);
    if (v < 0x80) {
        put(v);
    } else if (v < 0x800) {
        put(0xC0U | (v >> 6U));
        put(0x80U | (v & 0x3FU));
    } else if (v < 0x10000) {
        put(0xE0U | (v >> 12U));
        put(0x80U | ((v >> 6U) & 0x3FU));
        put(0x80U | (v & 0x3FU));
    } else {
        put(0xF0U | (v >> 18U));
        put(0x80U | ((v >> 12U) & 0x3FU));
        put(0x80U | ((v >> 6U) & 0x3FU));
        put(0x80U | (v & 0x3FU));
    }
}

std::expected<std::vector<char32_t>, CodecError> utf8_decode(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto b0 = static_cast<std::uint8_t>(text[i]);
        std::size_t len = 0;
        std::uint32_t cp = 0;
        if (b0 < 0x80) {
            len = 1;
            cp = b0;
        } else if ((b0 & 0xE0U) == 0xC0U) {
            len = 2;
            cp = b0 & 0x1FU;
        } else if ((b0 & 0xF0U) == 0xE0U) {
            len = 3;
            cp = b0 & 0x0FU;
        } else if ((b0 & 0xF8U) == 0xF0U) {
            len = 4;
            cp = b0 & 0x07U;
        } else {
            return std::unexpected(CodecError{.offset = i, .text = "invalid UTF-8 lead byte"});
        }
        if (i + len > text.size()) {
            return std::unexpected(CodecError{.offset = i, .text = "truncated UTF-8 sequence"});
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto b = static_cast<std::uint8_t>(text[i + k]);
            if ((b & 0xC0U) != 0x80U) {
                return std::unexpected(
                    CodecError{.offset = i + k, .text = "invalid UTF-8 continuation byte"});
            }
            cp = (cp << 6U) | (b & 0x3FU);
        }
        constexpr std::array<std::uint32_t, 5> min_for_len{0, 0, 0x80, 0x800, 0x10000};
        if (cp < min_for_len.at(len) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return std::unexpected(CodecError{.offset = i, .text = "invalid UTF-8 code point"});
        }
        out.push_back(static_cast<char32_t>(cp));
        i += len;
    }
    return out;
}

std::expected<Encoded, CodecError> encode(std::string_view utf8, const Delimiters& d,
                                          const TextOptions& options) {
    auto cps = utf8_decode(utf8);
    if (!cps) {
        return std::unexpected(cps.error());
    }
    Encoded result;
    auto& out = result.wire;
    out.reserve(utf8.size());
    for (const auto cp : *cps) {
        // 1. delimiters, 2. printable ASCII, 3. everything else by option.
        if (cp == static_cast<unsigned char>(d.field)) {
            out += {d.escape, 'F', d.escape};
        } else if (cp == static_cast<unsigned char>(d.component)) {
            out += {d.escape, 'S', d.escape};
        } else if (cp == static_cast<unsigned char>(d.repeat)) {
            out += {d.escape, 'R', d.escape};
        } else if (cp == static_cast<unsigned char>(d.escape)) {
            out += {d.escape, 'E', d.escape};
        } else if (is_printable_ascii(cp)) {
            out += static_cast<char>(cp);
        } else if (cp < 0x20 || cp == 0x7F) {
            // Control characters never go raw; CR and LF become &X0D& / &X0A&.
            append_hex_escape(out, {static_cast<std::uint8_t>(cp)}, d.escape);
        } else if (options.non_ascii == NonAscii::transliterate) {
            const auto ascii = transliteration(cp);
            if (ascii.empty()) {
                result.dropped.push_back(cp);
            } else {
                out += ascii;
            }
        } else {
            const auto b = charset_bytes(cp, options.charset);
            if (b.empty()) {
                return std::unexpected(
                    CodecError{.offset = out.size(),
                               .text = std::format("U+{:04X} cannot be represented in Latin-1",
                                                   static_cast<std::uint32_t>(cp))});
            }
            if (options.non_ascii == NonAscii::ascii_hex) {
                append_hex_escape(out, b, d.escape);
            } else {
                for (const auto byte : b) {
                    out += static_cast<char>(byte);
                }
            }
        }
    }
    return result;
}

std::expected<std::string, CodecError> decode(std::string_view wire, const Delimiters& d,
                                              const TextOptions& options) {
    std::string raw;  // bytes in the wire charset
    raw.reserve(wire.size());
    for (std::size_t i = 0; i < wire.size(); ++i) {
        if (wire[i] != d.escape) {
            raw += wire[i];
            continue;
        }
        const auto end = wire.find(d.escape, i + 1);
        if (end == std::string_view::npos) {
            if (options.strict) {
                return std::unexpected(
                    CodecError{.offset = i, .text = "unterminated escape sequence"});
            }
            raw += wire.substr(i);
            break;
        }
        const auto body = wire.substr(i + 1, end - i - 1);
        if (body == "F") {
            raw += d.field;
        } else if (body == "S") {
            raw += d.component;
        } else if (body == "R") {
            raw += d.repeat;
        } else if (body == "E") {
            raw += d.escape;
        } else if (body.size() >= 3 && body.front() == 'X' && (body.size() - 1) % 2 == 0 &&
                   std::ranges::all_of(body.substr(1), [](char c) { return hex_digit(c) >= 0; })) {
            for (std::size_t k = 1; k < body.size(); k += 2) {
                raw += static_cast<char>((hex_digit(body[k]) << 4) | hex_digit(body[k + 1]));
            }
        } else {
            if (options.strict) {
                return std::unexpected(CodecError{
                    .offset = i, .text = std::format("unknown escape sequence '{}'", body)});
            }
            raw += wire.substr(i, end - i + 1);
        }
        i = end;
    }

    if (options.charset == Charset::latin1) {
        std::string out;
        out.reserve(raw.size());
        for (const char c : raw) {
            utf8_append(out, static_cast<char32_t>(static_cast<unsigned char>(c)));
        }
        return out;
    }
    if (utf8_decode(raw)) {
        return raw;
    }
    if (options.strict) {
        return std::unexpected(CodecError{.offset = 0, .text = "wire text is not valid UTF-8"});
    }
    // Lenient: keep valid sequences, replace each byte that starts none.
    const auto valid_length = [&raw](std::size_t at) -> std::size_t {
        for (std::size_t len = 1; len <= 4 && at + len <= raw.size(); ++len) {
            if (utf8_decode(std::string_view{raw}.substr(at, len))) {
                return len;
            }
        }
        return 0;
    };
    std::string out;
    for (std::size_t i = 0; i < raw.size();) {
        if (const auto len = valid_length(i); len > 0) {
            out += raw.substr(i, len);
            i += len;
        } else {
            utf8_append(out, replacement_char);
            ++i;
        }
    }
    return out;
}

}  // namespace lisbench::lis02

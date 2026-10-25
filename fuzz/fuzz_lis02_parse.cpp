// Arbitrary text as records: parse must not crash; whatever parses must
// serialize, and the serialized form must parse to the same message.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "lisbench/lis02_message.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace lisbench::lis02;
    const std::string_view text{reinterpret_cast<const char*>(data), size};  // NOLINT
    const auto message = parse_text(text);
    if (!message) {
        return 0;
    }
    (void)validate(*message);
    const auto lines = serialize(*message);
    if (!lines) {
        return 0;  // e.g. a value outside Latin-1 cannot be put on the wire
    }
    const auto again = parse(*lines);
    if (!again || again->records.size() != message->records.size()) {
        std::abort();
    }
    for (std::size_t i = 0; i < message->records.size(); ++i) {
        auto a = message->records[i];
        auto b = again->records[i];
        a.trim();
        b.trim();
        if (a != b) {
            std::abort();
        }
    }
    return 0;
}

// Arbitrary bytes as a capture: deframe must never crash, and every record it
// returns must survive framing and deframing unchanged.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "lisbench/lis01_decoder.hpp"
#include "lisbench/lis01_frame.hpp"
#include "lisbench/lis01_link.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace lisbench;
    const bytes_view input{reinterpret_cast<const std::byte*>(data), size};  // NOLINT
    const auto capture = lis01::deframe(input);

    lis01::Message m{capture.records};
    bytes stream;
    for (const auto& f : lis01::frames_for(m)) {
        const auto raw = lis01::encode(f);
        stream.insert(stream.end(), raw.begin(), raw.end());
    }
    const auto again = lis01::deframe(stream);
    // Records containing STX/ETX/ETB cannot be framed faithfully; those never
    // come out of deframe, so the round trip must hold.
    if (again.records != capture.records) {
        std::abort();
    }
    return 0;
}

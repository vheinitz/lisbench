#include <catch2/catch_test_macros.hpp>

#include "lisbench/printable.hpp"

using namespace lisbench;

TEST_CASE("printable shows controls by name and 8-bit bytes as hex") {
    CHECK(printable(to_bytes("\x02"
                             "1R|1|Ren\xE9\r\x03"
                             "AB\r\n")) == "<STX>1R|1|Ren<E9><CR><ETX>AB<CR><LF>");
    CHECK(printable(to_bytes("a<b")) == "a<<b");
}

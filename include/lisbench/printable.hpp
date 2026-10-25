#pragma once

// Human-readable rendering of raw link bytes for logs and the CLI.
// Control characters appear as <STX>, <CR> ...; bytes >= 0x80 as <E9>, so a
// logged frame can always be checked against its checksum by hand.

#include <string>

#include "lisbench/bytes.hpp"

namespace lisbench {

[[nodiscard]] std::string printable(bytes_view data);

}  // namespace lisbench

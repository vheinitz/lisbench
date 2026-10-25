#pragma once

// Serial ports present on this computer, for the web UI's port selection.

#include <string>
#include <vector>

namespace lisbench::io {

struct PortInfo {
    std::string path{};         // /dev/ttyUSB0, COM3
    std::string description{};  // "FTDI FT232R USB UART", from the USB device
};

// Linux: USB serial adapters, the Pi's UARTs and /dev/serial0, with the USB
// vendor and product where known. Windows: the COM ports Windows reports.
[[nodiscard]] std::vector<PortInfo> serial_ports();

}  // namespace lisbench::io

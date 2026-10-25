#include "lisbench/io/ports.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace lisbench::io {

namespace fs = std::filesystem;

namespace {

[[maybe_unused]] std::string first_line(const fs::path& file) {
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

#if !defined(_WIN32)
// Walks up from /sys/class/tty/<name>/device to the USB device and reads
// its manufacturer and product strings.
std::string usb_description(std::string_view name) {
    std::error_code ec;
    auto dir = fs::canonical(fs::path{"/sys/class/tty"} / name / "device", ec);
    if (ec) {
        return {};
    }
    for (int depth = 0; depth < 6 && !dir.empty() && dir != dir.root_path(); ++depth) {
        if (fs::exists(dir / "idVendor", ec)) {
            auto text = first_line(dir / "manufacturer");
            const auto product = first_line(dir / "product");
            if (!product.empty()) {
                text += text.empty() ? product : " " + product;
            }
            if (text.empty()) {
                text = "USB " + first_line(dir / "idVendor") + ":" + first_line(dir / "idProduct");
            }
            return text;
        }
        dir = dir.parent_path();
    }
    return {};
}
#endif

}  // namespace

std::vector<PortInfo> serial_ports() {
    std::vector<PortInfo> out;
#if defined(_WIN32)
    // QueryDosDevice lists the COM ports Windows knows of.
    for (int i = 1; i <= 64; ++i) {
        const auto name = "COM" + std::to_string(i);
        std::array<char, 256> target{};
        if (QueryDosDeviceA(name.c_str(), target.data(), static_cast<DWORD>(target.size())) != 0) {
            out.push_back({.path = name, .description = target.data()});
        }
    }
#else
    std::error_code ec;
    for (const auto& e : fs::directory_iterator{"/dev", ec}) {
        const auto name = e.path().filename().string();
        const bool usb = name.starts_with("ttyUSB") || name.starts_with("ttyACM");
        const bool uart = name.starts_with("ttyAMA") || name == "serial0" || name == "serial1";
        if (!usb && !uart) {
            continue;
        }
        PortInfo p{.path = e.path().string()};
        if (usb) {
            p.description = usb_description(name);
        } else if (name.starts_with("serial")) {
            p.description =
                "Raspberry Pi UART (" + fs::read_symlink(e.path(), ec).filename().string() + ")";
        } else {
            p.description = "UART";
        }
        out.push_back(std::move(p));
    }
    std::ranges::sort(out, {}, &PortInfo::path);
#endif
    return out;
}

}  // namespace lisbench::io

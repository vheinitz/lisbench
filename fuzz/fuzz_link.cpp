// Arbitrary bytes and timer expiries fed into a link that is also sending:
// no crash, and every submitted message is reported exactly once after
// cancel_all.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>

#include "lisbench/lis01_link.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace lisbench;
    using namespace lisbench::lis01;
    if (size < 1) {
        return 0;
    }
    Link link{(data[0] & 1U) != 0 ? Role::instrument : Role::computer};
    Events ev;
    std::map<MessageId, int> results;
    TimerId timer = 0;
    const auto drain = [&] {
        for (const auto& e : ev) {
            if (const auto* f = std::get_if<ev::SendFinished>(&e)) {
                ++results[f->id];
            } else if (const auto* t = std::get_if<ev::ArmTimer>(&e)) {
                timer = t->id;
            }
        }
        ev.clear();
    };
    std::vector<MessageId> ids;
    ids.push_back(link.submit(Message{{to_bytes("H|\\^&"), to_bytes("L|1|N")}}, ev));
    drain();
    for (std::size_t i = 1; i < size; ++i) {
        const auto b = data[i];
        if (b == 0xFF) {
            link.on_timer(timer, ev);
        } else if (b == 0xFE) {
            ids.push_back(link.submit(Message{{to_bytes("H|\\^&"), to_bytes("L|1|N")}}, ev));
        } else if (b == 0xFD) {
            link.on_disconnect(ev);
            link.resume(ev);
        } else {
            const auto byte = static_cast<std::byte>(b);
            link.on_bytes(bytes_view{&byte, 1}, ev);
        }
        drain();
    }
    link.cancel_all(ev);
    drain();
    for (const auto id : ids) {
        if (results[id] != 1) {
            std::abort();
        }
    }
    return 0;
}

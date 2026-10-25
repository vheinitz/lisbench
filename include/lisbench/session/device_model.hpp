#pragma once

// The instrument simulator's logic (docs/spec/features.md, 9.6): specimen
// IDs, the query it sends, the results it produces from the catalogue, and
// what it takes from the LIS's answer. Pure; the session runtime sends.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lisbench/lis02_message.hpp"
#include "lisbench/session/catalog.hpp"
#include "lisbench/session/config.hpp"

namespace lisbench::session {

enum class SpecimenState : std::uint8_t {
    queued,
    queried,
    ordered,
    no_order,
    timed_out,
    measuring,
    reported,
    failed,
};

[[nodiscard]] std::string_view to_string(SpecimenState s) noexcept;

struct DeviceSpecimen {
    std::string id{};
    SpecimenState state{SpecimenState::queued};
    std::vector<std::string> tests{};
    std::vector<std::string> values{};  // one per test once measured
    std::string note{};
};

class DeviceModel {
public:
    DeviceModel(DeviceConfig config, Catalog catalog, std::string name);

    // The next specimen to work on; false when a fixed list is used up.
    [[nodiscard]] bool next(DeviceSpecimen& out);

    [[nodiscard]] std::vector<std::string> query(const DeviceSpecimen& s) const;
    // Tests the LIS ordered for this specimen (empty: no order).
    [[nodiscard]] static std::vector<std::string> ordered_tests(const lis02::Message& answer,
                                                                std::string_view specimen);
    // Measures (values from the catalogue, stable per specimen and test) and
    // returns the result message.
    [[nodiscard]] std::vector<std::string> results(DeviceSpecimen& s) const;

    [[nodiscard]] const DeviceConfig& config() const noexcept { return config_; }

private:
    DeviceConfig config_;
    Catalog catalog_;
    std::string name_;
    unsigned counter_;
    std::size_t list_index_{0};
};

}  // namespace lisbench::session

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>

#include "lisbench/lis01_decoder.hpp"
#include "lisbench/lis01_frame.hpp"
#include "lisbench/lis01_link.hpp"
#include "lisbench/lis02_message.hpp"

using namespace lisbench;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> records_of(std::string_view text) {
    std::vector<std::string> out;
    for (auto part : text | std::views::split('\r')) {
        if (!part.empty()) {
            out.emplace_back(part.begin(), part.end());
        }
    }
    return out;
}

}  // namespace

TEST_CASE("corpus: valid files validate, invalid files are caught, all round-trip") {
    int files = 0;
    for (const auto& entry : fs::directory_iterator{LISBENCH_CORPUS_DIR}) {
        if (entry.path().extension() != ".astm") {
            continue;
        }
        ++files;
        const auto name = entry.path().filename().string();
        INFO(name);
        const auto text = read_file(entry.path());
        const auto wire = records_of(text);
        const auto message = lis02::parse(wire);
        REQUIRE(message);

        const auto issues = lis02::validate(*message);
        const bool errors = std::ranges::any_of(
            issues, [](const auto& i) { return i.severity == lis02::Severity::error; });
        CHECK(errors == name.starts_with("invalid_"));

        // Parse -> serialize gives back the same wire text.
        CHECK(lis02::serialize(*message).value() == wire);

        // Frame -> deframe gives back the same records.
        lis01::Message m;
        for (const auto& r : wire) {
            m.records.push_back(to_bytes(r));
        }
        bytes stream{lis01::ctl::enq};
        for (const auto& f : lis01::frames_for(m)) {
            const auto raw = lis01::encode(f);
            stream.insert(stream.end(), raw.begin(), raw.end());
        }
        stream.push_back(lis01::ctl::eot);
        const auto capture = lis01::deframe(stream);
        CHECK(capture.problems.empty());
        CHECK(capture.records == m.records);
    }
    CHECK(files >= 7);
}

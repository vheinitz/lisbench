#pragma once

// Checked reading of YAML nodes for session and catalogue files: unknown keys
// and wrong types are errors with the line, never silently ignored.

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lisbench::session::detail {

struct FileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string where(const YAML::Node& node) {
    return node.Mark().line >= 0 ? std::format("line {}: ", node.Mark().line + 1) : "";
}

[[noreturn]] inline void fail(const YAML::Node& node, std::string_view text) {
    throw FileError{where(node) + std::string{text}};
}

inline void only_keys(const YAML::Node& map, std::string_view what,
                      std::initializer_list<std::string_view> allowed) {
    if (!map.IsMap()) {
        fail(map, std::format("{} must be a mapping", what));
    }
    for (const auto& kv : map) {
        const auto key = kv.first.as<std::string>();
        if (std::ranges::find(allowed, key) == allowed.end()) {
            fail(kv.first, std::format("unknown key '{}' in {}", key, what));
        }
    }
}

template <typename T>
T get(const YAML::Node& map, const char* key, T fallback) {
    const auto n = map[key];
    if (!n || n.IsNull()) {
        return fallback;
    }
    try {
        return n.as<T>();
    } catch (const YAML::Exception&) {
        fail(n, std::format("'{}' has the wrong type", key));
    }
}

template <typename T>
T required(const YAML::Node& map, const char* key) {
    const auto n = map[key];
    if (!n || n.IsNull()) {
        fail(map, std::format("'{}' is missing", key));
    }
    return get<T>(map, key, T{});
}

// One of a fixed set of words.
template <typename E>
E choice(const YAML::Node& map, const char* key, E fallback,
         std::initializer_list<std::pair<std::string_view, E>> words) {
    const auto n = map[key];
    if (!n || n.IsNull()) {
        return fallback;
    }
    const auto v = get<std::string>(map, key, "");
    for (const auto& [word, value] : words) {
        if (v == word) {
            return value;
        }
    }
    std::string list;
    for (const auto& [word, value] : words) {
        list += list.empty() ? "" : ", ";
        list += word;
    }
    fail(n, std::format("'{}' is one of: {}", key, list));
}

}  // namespace lisbench::session::detail

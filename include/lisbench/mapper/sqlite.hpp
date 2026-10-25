#pragma once

// Minimal SQLite access for the mapper: one connection, prepared statements
// kept per SQL text, named parameters only (:name). Values are bound, never
// pasted into SQL.

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lisbench/mapper/template.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace lisbench::mapper {

using Params = std::map<std::string, std::string>;

class Database {
public:
    // ":memory:" opens an in-memory database.
    [[nodiscard]] static std::expected<Database, std::string> open(
        const std::filesystem::path& file);

    [[nodiscard]] std::expected<std::vector<Columns>, std::string> query(const std::string& sql,
                                                                         const Params& params);
    // Statements without result rows; returns the number of changed rows.
    [[nodiscard]] std::expected<int, std::string> execute(const std::string& sql,
                                                          const Params& params);
    // Several statements, no parameters (schemas in tests and examples).
    [[nodiscard]] std::expected<void, std::string> script(const std::string& sql);

private:
    struct Close {
        void operator()(sqlite3* db) const noexcept;
    };
    struct Finalize {
        void operator()(sqlite3_stmt* s) const noexcept;
    };
    using Statement = std::unique_ptr<sqlite3_stmt, Finalize>;

    explicit Database(sqlite3* db) : db_{db} {}
    std::expected<sqlite3_stmt*, std::string> prepare(const std::string& sql, const Params& params);

    std::unique_ptr<sqlite3, Close> db_;
    std::map<std::string, Statement> statements_{};
};

}  // namespace lisbench::mapper

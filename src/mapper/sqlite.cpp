#include "lisbench/mapper/sqlite.hpp"

#include <sqlite3.h>

#include <format>
#include <string_view>

namespace lisbench::mapper {

void Database::Close::operator()(sqlite3* db) const noexcept {
    sqlite3_close_v2(db);
}
void Database::Finalize::operator()(sqlite3_stmt* s) const noexcept {
    sqlite3_finalize(s);
}

std::expected<Database, std::string> Database::open(const std::filesystem::path& file) {
    sqlite3* db = nullptr;
    const auto name = file.string();
    const int rc =
        sqlite3_open_v2(name.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    Database result{db};  // owns db even when opening failed
    if (rc != SQLITE_OK) {
        return std::unexpected(std::format("cannot open {}: {}", name,
                                           db != nullptr ? sqlite3_errmsg(db) : "out of memory"));
    }
    return result;
}

std::expected<sqlite3_stmt*, std::string> Database::prepare(const std::string& sql,
                                                            const Params& params) {
    auto it = statements_.find(sql);
    if (it == statements_.end()) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_.get(), sql.c_str(), static_cast<int>(sql.size()), &raw,
                               nullptr) != SQLITE_OK) {
            return std::unexpected(std::format("SQL error: {}", sqlite3_errmsg(db_.get())));
        }
        it = statements_.emplace(sql, Statement{raw}).first;
    }
    auto* stmt = it->second.get();
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    const int count = sqlite3_bind_parameter_count(stmt);
    for (int i = 1; i <= count; ++i) {
        const char* name = sqlite3_bind_parameter_name(stmt, i);
        if (name == nullptr) {
            return std::unexpected("positional parameters (?) are not supported; use :name");
        }
        // Skip the prefix ':', '@' or '$'.
        const auto key = std::string{std::string_view{name}.substr(1)};
        const auto value = params.find(key);
        if (value == params.end()) {
            return std::unexpected(std::format("no value for parameter :{}", key));
        }
        // SQLITE_TRANSIENT is SQLite's own cast macro.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
        sqlite3_bind_text(stmt, i, value->second.c_str(), static_cast<int>(value->second.size()),
                          SQLITE_TRANSIENT);
    }
    return stmt;
}

std::expected<std::vector<Columns>, std::string> Database::query(const std::string& sql,
                                                                 const Params& params) {
    auto stmt = prepare(sql, params);
    if (!stmt) {
        return std::unexpected(stmt.error());
    }
    std::vector<Columns> rows;
    while (true) {
        const int rc = sqlite3_step(*stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return std::unexpected(std::format("SQL error: {}", sqlite3_errmsg(db_.get())));
        }
        Columns row;
        const int n = sqlite3_column_count(*stmt);
        for (int c = 0; c < n; ++c) {
            const std::string name = sqlite3_column_name(*stmt, c);
            if (sqlite3_column_type(*stmt, c) == SQLITE_NULL) {
                row[name] = std::nullopt;
            } else {
                const auto* text = sqlite3_column_text(*stmt, c);
                const auto size = static_cast<std::size_t>(sqlite3_column_bytes(*stmt, c));
                // SQLite returns text as unsigned char.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                row[name] = std::string(reinterpret_cast<const char*>(text), size);
            }
        }
        rows.push_back(std::move(row));
    }
    sqlite3_reset(*stmt);
    return rows;
}

std::expected<int, std::string> Database::execute(const std::string& sql, const Params& params) {
    auto stmt = prepare(sql, params);
    if (!stmt) {
        return std::unexpected(stmt.error());
    }
    int rc = sqlite3_step(*stmt);
    while (rc == SQLITE_ROW) {
        rc = sqlite3_step(*stmt);
    }
    sqlite3_reset(*stmt);
    if (rc != SQLITE_DONE) {
        return std::unexpected(std::format("SQL error: {}", sqlite3_errmsg(db_.get())));
    }
    return sqlite3_changes(db_.get());
}

std::expected<void, std::string> Database::script(const std::string& sql) {
    char* error = nullptr;
    if (sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        std::string text = error != nullptr ? error : "unknown error";
        sqlite3_free(error);
        return std::unexpected(std::format("SQL error: {}", text));
    }
    return {};
}

}  // namespace lisbench::mapper

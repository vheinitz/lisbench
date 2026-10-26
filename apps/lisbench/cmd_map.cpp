// lisbench map: build messages from a database through a mapping file and
// print them, send them to a LIS or write them to files; then write the
// result back to the database.

#include "cmd_map.hpp"

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>

#include "common.hpp"
#include "lisbench/io/session.hpp"
#include "lisbench/mapper/mapping.hpp"
#include "lisbench/printable.hpp"

namespace lisbench::cli {

namespace {

namespace fs = std::filesystem;

std::expected<std::map<std::string, std::string>, std::string> pairs(
    const std::vector<std::string>& items) {
    std::map<std::string, std::string> out;
    for (const auto& item : items) {
        const auto eq = item.find('=');
        if (eq == std::string::npos || eq == 0) {
            return std::unexpected(std::format("'{}': expected name=value", item));
        }
        out[item.substr(0, eq)] = item.substr(eq + 1);
    }
    return out;
}

struct Job {
    std::map<std::string, std::string> args{};
    mapper::Context ctx{};
    std::vector<std::string> records{};
    std::string error{};
};

// Delivery target: none (print), a file pattern, or an endpoint.
struct FileTarget {
    fs::path dir;
    std::string pattern;  // contains {unique}
};

std::expected<fs::path, std::string> write_file(const FileTarget& target, const Job& job,
                                                int counter) {
    std::string name = target.pattern;
    for (int attempt = 0;; ++attempt) {
        const auto unique =
            std::format("{}_{}_{}", job.ctx.now, job.ctx.message_id, counter + attempt);
        name = target.pattern;
        name.replace(name.find("{unique}"), 8, unique);
        if (!fs::exists(target.dir / name)) {
            break;
        }
    }
    auto final_path = target.dir / name;
    auto tmp = final_path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) {
            return std::unexpected(std::format("cannot write {}", tmp.string()));
        }
        // A file carries the records separated by CR, without framing.
        for (const auto& r : job.records) {
            out << r << '\r';
        }
        if (!out.flush()) {
            return std::unexpected(std::format("cannot write {}", tmp.string()));
        }
    }
    std::error_code ec;
    fs::rename(tmp, final_path, ec);  // the reader never sees half a file
    if (ec) {
        return std::unexpected(std::format("cannot rename {}: {}", tmp.string(), ec.message()));
    }
    return final_path;
}

// Reports one result and writes it back. Returns false when the run must stop.
bool finish(const mapper::Mapping& m, mapper::Database& db, Job& job, std::size_t index, bool ok,
            const std::string& what) {
    println("{} [map] message {} ({}): {}", clock_now(), index + 1, job.ctx.message_id, what);
    if (ok && m.on_success) {
        const auto changed = mapper::write_back(*m.on_success, db, job.ctx);
        if (!changed) {
            // Sent but not marked as sent: it would go out again next time.
            println(stderr, "{} [map] message {} WAS SENT but on_success failed: {}", clock_now(),
                    index + 1, changed.error());
            return false;
        }
        println("{} [map]   on_success: {} row(s) changed", clock_now(), *changed);
    }
    if (!ok && m.on_failure) {
        job.ctx.error = what;
        const auto changed = mapper::write_back(*m.on_failure, db, job.ctx);
        if (!changed) {
            println(stderr, "{} [map]   on_failure failed as well: {}", clock_now(),
                    changed.error());
        } else {
            println("{} [map]   on_failure: {} row(s) changed", clock_now(), *changed);
        }
    }
    return ok;
}

struct SendContext {
    const mapper::Mapping* mapping;
    mapper::Database* db;
    std::vector<Job>* jobs;
    bool keep_going;
    bool all_ok{true};
};

asio::awaitable<void> send_jobs(io::Session* session, SendContext* c) {
    for (std::size_t i = 0; i < c->jobs->size(); ++i) {
        auto& job = (*c->jobs)[i];
        if (!job.error.empty()) {
            c->all_ok = finish(*c->mapping, *c->db, job, i, false, job.error) && c->all_ok;
            if (!c->keep_going) {
                break;
            }
            continue;
        }
        lis01::Message message;
        for (const auto& r : job.records) {
            message.records.push_back(to_bytes(r));
        }
        const auto outcome = co_await session->send(std::move(message));
        const bool go_on =
            finish(*c->mapping, *c->db, job, i, outcome == lis01::Outcome::delivered,
                   std::format("{} records, {}", job.records.size(), lis01::to_string(outcome)));
        c->all_ok = c->all_ok && go_on;
        if (!go_on && !c->keep_going) {
            break;
        }
    }
    session->stop();
}

asio::awaitable<bool> send_all(io::Endpoint endpoint, lis01::Role role, SendContext c) {
    auto stream = co_await io::connect(endpoint);
    io::Session session{
        std::move(stream),
        {.role = role, .timing = {}, .faults = {}, .low_level = io::low_level_of(endpoint)},
        {}};
    using namespace asio::experimental::awaitable_operators;
    co_await (session.run() && send_jobs(&session, &c));
    co_return c.all_ok;
}

}  // namespace

int cmd_map(const MapFlags& flags) {
    auto mapping = mapper::load_mapping(flags.mapping);
    if (!mapping) {
        println(stderr, "{}: {}", flags.mapping, mapping.error());
        return 2;
    }
    auto cfg = pairs(flags.cfg);
    auto args = pairs(flags.args);
    if (!cfg || !args) {
        println(stderr, "map: {}", !cfg ? cfg.error() : args.error());
        return 2;
    }
    for (const auto& [k, v] : *cfg) {
        mapping->cfg[k] = v;
    }

    // Check the target before touching the database.
    std::optional<FileTarget> file;
    std::optional<io::Endpoint> endpoint;
    if (flags.to.starts_with("file:")) {
        const fs::path p{flags.to.substr(5)};
        if (!p.filename().string().contains("{unique}")) {
            println(stderr,
                    "map: the file name pattern must contain {{unique}}, e.g. "
                    "file:out/result_{{unique}}.astm");
            return 2;
        }
        file = FileTarget{.dir = p.parent_path().empty() ? fs::path{"."} : p.parent_path(),
                          .pattern = p.filename().string()};
        if (!fs::is_directory(file->dir)) {
            println(stderr, "map: directory {} does not exist", file->dir.string());
            return 2;
        }
    } else if (!flags.to.empty()) {
        auto e = io::parse_endpoint(flags.to);
        if (!e) {
            println(stderr, "map: {}", e.error());
            return 2;
        }
        endpoint = *e;
    }

    auto db = mapper::Database::open(flags.db);
    if (!db) {
        println(stderr, "map: {}", db.error());
        return 2;
    }

    // One message per argument set: the --arg values, or each row of --each.
    std::vector<std::map<std::string, std::string>> arg_sets;
    if (!flags.each.empty()) {
        auto rows = db->query(flags.each, *args);
        if (!rows) {
            println(stderr, "map: --each: {}", rows.error());
            return 2;
        }
        for (const auto& row : *rows) {
            auto set = *args;
            for (const auto& [col, value] : row) {
                set[col] = value.value_or("");
            }
            arg_sets.push_back(std::move(set));
        }
        if (arg_sets.empty()) {
            println("{} [map] --each found nothing to send", clock_now());
            return 0;
        }
    } else {
        arg_sets.push_back(*args);
    }

    std::vector<Job> jobs;
    for (auto& set : arg_sets) {
        Job job{.args = set,
                .ctx = {.cfg = mapping->cfg,
                        .args = set,
                        .rows = {},
                        .now = mapper::local_timestamp(std::chrono::system_clock::now()),
                        .message_id = mapper::stable_message_id(mapping->source, set),
                        .error = {}}};
        auto message = mapper::build_message(*mapping, *db, job.ctx);
        if (!message) {
            job.error = message.error();
        } else if (auto lines = lis02::serialize(*message, mapping->text); !lines) {
            job.error = std::format("record {}: {}", lines.error().record + 1, lines.error().text);
        } else {
            job.records = std::move(*lines);
        }
        jobs.push_back(std::move(job));
    }

    if (!file && !endpoint) {
        // Dry run: show, do not write back.
        bool ok = true;
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            const auto& job = jobs[i];
            println("# message {} ({})", i + 1, job.ctx.message_id);
            if (!job.error.empty()) {
                println(stderr, "map: message {}: {}", i + 1, job.error);
                ok = false;
                continue;
            }
            for (const auto& r : job.records) {
                println("{}", printable(to_bytes(r)));
            }
        }
        return ok ? 0 : 1;
    }

    if (file) {
        bool ok = true;
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            auto& job = jobs[i];
            if (!job.error.empty()) {
                ok = finish(*mapping, *db, job, i, false, job.error) && ok;
            } else if (auto written = write_file(*file, job, static_cast<int>(i)); !written) {
                ok = finish(*mapping, *db, job, i, false, written.error()) && ok;
            } else {
                ok = finish(*mapping, *db, job, i, true,
                            std::format("written to {}", written->string())) &&
                     ok;
            }
            if (!ok && !flags.keep_going) {
                return 1;
            }
        }
        return ok ? 0 : 1;
    }

    asio::io_context io;
    bool ok = false;
    asio::co_spawn(
        io,
        send_all(
            *endpoint, flags.role == "computer" ? lis01::Role::computer : lis01::Role::instrument,
            SendContext{
                .mapping = &*mapping, .db = &*db, .jobs = &jobs, .keep_going = flags.keep_going}),
        [&](const std::exception_ptr& e, bool result) {
            if (e) {
                try {
                    std::rethrow_exception(e);
                } catch (const std::exception& ex) {
                    println(stderr, "map: {}", ex.what());
                    // Nothing was sent: every message failed.
                    for (std::size_t i = 0; i < jobs.size(); ++i) {
                        (void)finish(*mapping, *db, jobs[i], i, false, ex.what());
                    }
                }
            } else {
                ok = result;
            }
        });
    io.run();
    return ok ? 0 : 1;
}

}  // namespace lisbench::cli

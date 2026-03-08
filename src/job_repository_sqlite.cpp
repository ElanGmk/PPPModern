#include "ppp/core/job_repository.h"

#if PPP_CORE_HAVE_SQLITE

#include "sqlite_support.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ppp::core {

namespace {

namespace fs = std::filesystem;

using sqlite_support::Statement;
using sqlite_support::Transaction;
using sqlite_support::exec;

constexpr int kLatestSchemaVersion = 7;

[[nodiscard]] std::int64_t to_micros(const TimePoint& tp) {
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] TimePoint from_micros(std::int64_t value) {
    return TimePoint{std::chrono::microseconds{value}};
}

[[nodiscard]] std::optional<std::string> optional_text(sqlite3_stmt* stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
    return text ? std::optional<std::string>{text} : std::optional<std::string>{std::string{}};
}

[[nodiscard]] std::optional<TimePoint> optional_time(sqlite3_stmt* stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
        return std::nullopt;
    }
    return from_micros(sqlite3_column_int64(stmt, column));
}

void bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    if (sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("failed to bind text parameter");
    }
}

void bind_optional_text(sqlite3_stmt* stmt, int index, const std::optional<std::string>& value) {
    if (value) {
        bind_text(stmt, index, *value);
    } else if (sqlite3_bind_null(stmt, index) != SQLITE_OK) {
        throw std::runtime_error("failed to bind null parameter");
    }
}

std::vector<std::string> load_attachments(sqlite3* db, std::string_view id) {
    Statement stmt{db, "SELECT path FROM attachments WHERE job_id=?1 ORDER BY idx;"};
    if (sqlite3_bind_text(stmt.get(), 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("failed to bind attachment query parameter");
    }
    std::vector<std::string> attachments;
    while (true) {
        const auto rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to iterate attachment rows");
        }
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        attachments.emplace_back(text ? text : "");
    }
    return attachments;
}

std::vector<std::string> load_tags(sqlite3* db, std::string_view id) {
    Statement stmt{db, "SELECT value FROM tags WHERE job_id=?1 ORDER BY idx;"};
    if (sqlite3_bind_text(stmt.get(), 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("failed to bind tag query parameter");
    }
    std::vector<std::string> tags;
    while (true) {
        const auto rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to iterate tag rows");
        }
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0));
        tags.emplace_back(text ? text : "");
    }
    return tags;
}

JobRecord hydrate_job(sqlite3_stmt* stmt) {
    JobRecord record;
    const auto* id_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto* state_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!id_text || !state_text) {
        throw std::runtime_error("job row missing required columns");
    }
    record.id = id_text;
    const auto state = job_state_from_string(state_text);
    if (!state) {
        throw std::runtime_error("job row contained invalid state value");
    }
    record.state = *state;
    record.created_at = from_micros(sqlite3_column_int64(stmt, 2));
    record.updated_at = optional_time(stmt, 3);

    const auto* source_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    if (!source_text) {
        throw std::runtime_error("job row missing source_path");
    }
    record.payload.source_path = source_text;

    record.payload.profile_name = optional_text(stmt, 5);
    record.correlation_id = optional_text(stmt, 6);
    record.error_message = optional_text(stmt, 7);
    record.priority = static_cast<std::int32_t>(sqlite3_column_int64(stmt, 8));
    record.attempt_count = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 9));
    record.last_attempt_at = optional_time(stmt, 10);
    record.due_at = optional_time(stmt, 11);

    return record;
}

int get_user_version(sqlite3* db) {
    Statement stmt{db, "PRAGMA user_version;"};
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        throw std::runtime_error("failed to query sqlite user_version");
    }
    return sqlite3_column_int(stmt.get(), 0);
}

void set_user_version(sqlite3* db, int version) {
    exec(db, std::string{"PRAGMA user_version = "} + std::to_string(version) + ";");
}

void ensure_schema(sqlite3* db) {
    exec(db, "PRAGMA foreign_keys = ON;");
    exec(db, "PRAGMA journal_mode = WAL;");

    int version = get_user_version(db);
    if (version > kLatestSchemaVersion) {
        throw std::runtime_error("database schema version is newer than this build supports");
    }

    struct Migration {
        int target_version;
        void (*apply)(sqlite3*);
    };

    const Migration migrations[] = {
        {1, [](sqlite3* connection) {
             exec(connection,
                  "CREATE TABLE IF NOT EXISTS jobs ("
                  " id TEXT PRIMARY KEY,"
                  " state TEXT NOT NULL,"
                  " created_at INTEGER NOT NULL,"
                  " updated_at INTEGER,"
                  " source_path TEXT NOT NULL,"
                  " profile_name TEXT,"
                  " correlation_id TEXT,"
                  " error_message TEXT,"
                  " priority INTEGER NOT NULL DEFAULT 0,"
                  " attempt_count INTEGER NOT NULL DEFAULT 0,"
                  " last_attempt_at INTEGER,"
                  " due_at INTEGER"
                  ");");
             exec(connection,
                  "CREATE TABLE IF NOT EXISTS attachments ("
                  " job_id TEXT NOT NULL,"
                  " idx INTEGER NOT NULL,"
                  " path TEXT NOT NULL,"
                  " PRIMARY KEY(job_id, idx),"
                  " FOREIGN KEY(job_id) REFERENCES jobs(id) ON DELETE CASCADE"
                  ");");
         }},
        {2, [](sqlite3* connection) {
             exec(connection,
                  "CREATE INDEX IF NOT EXISTS idx_jobs_state_created ON jobs(state, created_at);");
         }},
        {3, [](sqlite3* connection) {
             Statement pragma{connection, "PRAGMA table_info('jobs');"};
             bool has_column = false;
             while (true) {
                 const auto rc = sqlite3_step(pragma.get());
                 if (rc == SQLITE_DONE) {
                     break;
                 }
                 if (rc != SQLITE_ROW) {
                     throw std::runtime_error("failed to inspect jobs table info");
                 }
                 const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(pragma.get(), 1));
                 if (name && std::string_view{name} == "attempt_count") {
                     has_column = true;
                     break;
                 }
             }
             if (!has_column) {
                 exec(connection, "ALTER TABLE jobs ADD COLUMN attempt_count INTEGER NOT NULL DEFAULT 0;");
             }
         }},
        {4, [](sqlite3* connection) {
             Statement pragma{connection, "PRAGMA table_info('jobs');"};
             bool has_column = false;
             while (true) {
                 const auto rc = sqlite3_step(pragma.get());
                 if (rc == SQLITE_DONE) {
                     break;
                 }
                 if (rc != SQLITE_ROW) {
                     throw std::runtime_error("failed to inspect jobs table info");
                 }
                 const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(pragma.get(), 1));
                 if (name && std::string_view{name} == "last_attempt_at") {
                     has_column = true;
                     break;
                 }
             }
            if (!has_column) {
                exec(connection, "ALTER TABLE jobs ADD COLUMN last_attempt_at INTEGER;");
            }
        }},
        {5, [](sqlite3* connection) {
             bool has_priority = false;
             {
                 Statement pragma{connection, "PRAGMA table_info('jobs');"};
                 while (true) {
                     const auto rc = sqlite3_step(pragma.get());
                     if (rc == SQLITE_DONE) {
                         break;
                     }
                     if (rc != SQLITE_ROW) {
                         throw std::runtime_error("failed to inspect jobs table info");
                     }
                     const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(pragma.get(), 1));
                     if (name && std::string_view{name} == "priority") {
                         has_priority = true;
                         break;
                     }
                 }
             }
             if (!has_priority) {
                 exec(connection, "ALTER TABLE jobs ADD COLUMN priority INTEGER NOT NULL DEFAULT 0;");
             }
             exec(connection, "DROP INDEX IF EXISTS idx_jobs_state_created;");
             exec(connection,
                  "CREATE INDEX IF NOT EXISTS idx_jobs_state_priority_created ON jobs(state, priority DESC, created_at, id);");
         }},
        {6, [](sqlite3* connection) {
             bool has_due = false;
             {
                 Statement pragma{connection, "PRAGMA table_info('jobs');"};
                 while (true) {
                     const auto rc = sqlite3_step(pragma.get());
                     if (rc == SQLITE_DONE) {
                         break;
                     }
                     if (rc != SQLITE_ROW) {
                         throw std::runtime_error("failed to inspect jobs table info");
                     }
                     const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(pragma.get(), 1));
                     if (name && std::string_view{name} == "due_at") {
                         has_due = true;
                         break;
                     }
                 }
             }
             if (!has_due) {
                 exec(connection, "ALTER TABLE jobs ADD COLUMN due_at INTEGER;");
             }
             exec(connection, "DROP INDEX IF EXISTS idx_jobs_state_priority_created;");
             exec(connection,
                  "CREATE INDEX IF NOT EXISTS idx_jobs_state_priority_due_created ON jobs("
                  " state, priority DESC, COALESCE(due_at, 9223372036854775807), created_at, id);");
         }},
        {7, [](sqlite3* connection) {
             exec(connection,
                  "CREATE TABLE IF NOT EXISTS tags ("
                  " job_id TEXT NOT NULL,"
                  " idx INTEGER NOT NULL,"
                  " value TEXT NOT NULL,"
                  " PRIMARY KEY(job_id, idx),"
                  " FOREIGN KEY(job_id) REFERENCES jobs(id) ON DELETE CASCADE"
                  ");");
         }},
    };

    for (const auto& migration : migrations) {
        if (version < migration.target_version) {
            Transaction tx{db};
            migration.apply(db);
            set_user_version(db, migration.target_version);
            tx.commit();
            version = migration.target_version;
        }
    }
}

} // namespace

struct SqliteJobRepository::Impl {
    fs::path database_path;
    sqlite3* db{nullptr};
    mutable std::mutex mutex;

    explicit Impl(fs::path path) : database_path(std::move(path)) {
        if (database_path.empty()) {
            throw std::invalid_argument("database path must not be empty");
        }
        if (database_path.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(database_path.parent_path(), ec);
            if (ec) {
                throw std::runtime_error("failed to create database directory");
            }
        }

        const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(database_path.string().c_str(), &db, flags, nullptr) != SQLITE_OK) {
            std::string message = "failed to open sqlite database: ";
            message += sqlite3_errmsg(db);
            sqlite3_close(db);
            db = nullptr;
            throw std::runtime_error(message);
        }

        sqlite3_busy_timeout(db, 5000);
        ensure_schema(db);
    }

    ~Impl() {
        if (db) {
            sqlite3_close(db);
        }
    }
};

SqliteJobRepository::SqliteJobRepository(std::filesystem::path database_path)
    : impl_(std::make_unique<Impl>(std::move(database_path))) {}

SqliteJobRepository::~SqliteJobRepository() = default;

void SqliteJobRepository::initialize_schema(const std::filesystem::path& database_path) {
    if (database_path.empty()) {
        throw std::invalid_argument("database path must not be empty");
    }
    if (database_path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(database_path.parent_path(), ec);
        if (ec) {
            throw std::runtime_error("failed to create database directory");
        }
    }

    sqlite3* db = nullptr;
    const auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(database_path.string().c_str(), &db, flags, nullptr) != SQLITE_OK) {
        std::string message = "failed to open sqlite database: ";
        message += sqlite3_errmsg(db);
        sqlite3_close(db);
        throw std::runtime_error(message);
    }

    sqlite3_busy_timeout(db, 5000);

    try {
        ensure_schema(db);
    } catch (...) {
        sqlite3_close(db);
        throw;
    }

    sqlite3_close(db);
}

int SqliteJobRepository::latest_schema_version() { return kLatestSchemaVersion; }

std::optional<JobRecord> SqliteJobRepository::fetch(std::string_view id) const {
    std::scoped_lock lock(impl_->mutex);
    Statement stmt{impl_->db,
                   "SELECT id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,"
                   " attempt_count,last_attempt_at,due_at FROM jobs WHERE id=?1;"};
    if (sqlite3_bind_text(stmt.get(), 1, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error("failed to bind fetch parameter");
    }
    if (sqlite3_step(stmt.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    auto record = hydrate_job(stmt.get());
    record.payload.attachments = load_attachments(impl_->db, record.id);
    record.payload.tags = load_tags(impl_->db, record.id);
    return record;
}

std::vector<JobRecord> SqliteJobRepository::list(std::optional<JobState> state_filter) const {
    std::scoped_lock lock(impl_->mutex);
    std::string sql =
        "SELECT id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,attempt_count,last_attempt_at,due_at FROM jobs";
    if (state_filter) {
        sql += " WHERE state=?1";
    }
    sql += " ORDER BY priority DESC, COALESCE(due_at, 9223372036854775807) ASC, created_at ASC, id ASC;";
    Statement stmt{impl_->db, sql};
    if (state_filter) {
        const auto state_text = to_string(*state_filter);
        bind_text(stmt.get(), 1, std::string{state_text});
    }

    std::vector<JobRecord> result;
    while (true) {
        const auto rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("failed to iterate job rows");
        }
        auto record = hydrate_job(stmt.get());
        record.payload.attachments = load_attachments(impl_->db, record.id);
        record.payload.tags = load_tags(impl_->db, record.id);
        result.push_back(std::move(record));
    }
    return result;
}

std::optional<JobRecord> SqliteJobRepository::claim_next_submitted(JobState next_state) {
    std::scoped_lock lock(impl_->mutex);
    Transaction tx{impl_->db};

    Statement select{impl_->db,
                     "SELECT id FROM jobs WHERE state=?1 ORDER BY priority DESC, COALESCE(due_at, 9223372036854775807) ASC, "
                     "created_at ASC, id ASC LIMIT 1;"};
    bind_text(select.get(), 1, std::string{to_string(JobState::Submitted)});
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
        return std::nullopt;
    }

    const auto* id_text = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 0));
    if (!id_text) {
        throw std::runtime_error("failed to read job id while claiming");
    }
    std::string id{id_text};

    const auto now = Clock::now();
    Statement update{impl_->db,
                     "UPDATE jobs SET state=?1, updated_at=?2, last_attempt_at=?2, error_message=NULL,"
                     " attempt_count=attempt_count+1 WHERE id=?3;"};
    bind_text(update.get(), 1, std::string{to_string(next_state)});
    if (sqlite3_bind_int64(update.get(), 2, to_micros(now)) != SQLITE_OK) {
        throw std::runtime_error("failed to bind claim timestamp");
    }
    bind_text(update.get(), 3, id);
    if (sqlite3_step(update.get()) != SQLITE_DONE) {
        throw std::runtime_error("failed to update claimed job state");
    }

    Statement fetch{impl_->db,
                    "SELECT id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,"
                    " attempt_count,last_attempt_at,due_at FROM jobs WHERE id=?1;"};
    bind_text(fetch.get(), 1, id);
    if (sqlite3_step(fetch.get()) != SQLITE_ROW) {
        throw std::runtime_error("failed to reload claimed job");
    }

    auto record = hydrate_job(fetch.get());
    record.payload.attachments = load_attachments(impl_->db, record.id);
    record.payload.tags = load_tags(impl_->db, record.id);

    tx.commit();
    return record;
}

void SqliteJobRepository::upsert(JobRecord record) {
    std::scoped_lock lock(impl_->mutex);
    record.updated_at = Clock::now();
    Transaction tx{impl_->db};

    Statement stmt{impl_->db,
                   "INSERT INTO jobs(id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,attempt_count,last_attempt_at,due_at)"
                   " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)"
                   " ON CONFLICT(id) DO UPDATE SET"
                   " state=excluded.state,"
                   " created_at=excluded.created_at,"
                   " updated_at=excluded.updated_at,"
                   " source_path=excluded.source_path,"
                   " profile_name=excluded.profile_name,"
                   " correlation_id=excluded.correlation_id,"
                   " error_message=excluded.error_message,"
                   " priority=excluded.priority,"
                   " attempt_count=excluded.attempt_count,"
                   " last_attempt_at=excluded.last_attempt_at,"
                   " due_at=excluded.due_at;"};

    bind_text(stmt.get(), 1, record.id);
    bind_text(stmt.get(), 2, std::string{to_string(record.state)});
    if (sqlite3_bind_int64(stmt.get(), 3, to_micros(record.created_at)) != SQLITE_OK) {
        throw std::runtime_error("failed to bind created_at");
    }
    if (record.updated_at) {
        if (sqlite3_bind_int64(stmt.get(), 4, to_micros(*record.updated_at)) != SQLITE_OK) {
            throw std::runtime_error("failed to bind updated_at");
        }
    } else if (sqlite3_bind_null(stmt.get(), 4) != SQLITE_OK) {
        throw std::runtime_error("failed to bind null updated_at");
    }
    bind_text(stmt.get(), 5, record.payload.source_path);
    bind_optional_text(stmt.get(), 6, record.payload.profile_name);
    bind_optional_text(stmt.get(), 7, record.correlation_id);
    bind_optional_text(stmt.get(), 8, record.error_message);
    if (sqlite3_bind_int64(stmt.get(), 9, static_cast<std::int64_t>(record.priority)) != SQLITE_OK) {
        throw std::runtime_error("failed to bind priority");
    }
    if (sqlite3_bind_int64(stmt.get(), 10, static_cast<std::int64_t>(record.attempt_count)) != SQLITE_OK) {
        throw std::runtime_error("failed to bind attempt_count");
    }
    if (record.last_attempt_at) {
        if (sqlite3_bind_int64(stmt.get(), 11, to_micros(*record.last_attempt_at)) != SQLITE_OK) {
            throw std::runtime_error("failed to bind last_attempt_at");
        }
    } else if (sqlite3_bind_null(stmt.get(), 11) != SQLITE_OK) {
        throw std::runtime_error("failed to bind null last_attempt_at");
    }
    if (record.due_at) {
        if (sqlite3_bind_int64(stmt.get(), 12, to_micros(*record.due_at)) != SQLITE_OK) {
            throw std::runtime_error("failed to bind due_at");
        }
    } else if (sqlite3_bind_null(stmt.get(), 12) != SQLITE_OK) {
        throw std::runtime_error("failed to bind null due_at");
    }

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("failed to upsert job");
    }

    Statement delete_stmt{impl_->db, "DELETE FROM attachments WHERE job_id=?1;"};
    bind_text(delete_stmt.get(), 1, record.id);
    if (sqlite3_step(delete_stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("failed to clear attachments");
    }

    Statement insert_stmt{impl_->db, "INSERT INTO attachments(job_id,idx,path) VALUES(?1,?2,?3);"};
    for (std::size_t i = 0; i < record.payload.attachments.size(); ++i) {
        bind_text(insert_stmt.get(), 1, record.id);
        if (sqlite3_bind_int64(insert_stmt.get(), 2, static_cast<std::int64_t>(i)) != SQLITE_OK) {
            throw std::runtime_error("failed to bind attachment index");
        }
        bind_text(insert_stmt.get(), 3, record.payload.attachments[i]);
        if (sqlite3_step(insert_stmt.get()) != SQLITE_DONE) {
            throw std::runtime_error("failed to insert attachment");
        }
        sqlite3_reset(insert_stmt.get());
        sqlite3_clear_bindings(insert_stmt.get());
    }

    Statement delete_tags{impl_->db, "DELETE FROM tags WHERE job_id=?1;"};
    bind_text(delete_tags.get(), 1, record.id);
    if (sqlite3_step(delete_tags.get()) != SQLITE_DONE) {
        throw std::runtime_error("failed to clear tags");
    }

    Statement insert_tag{impl_->db, "INSERT INTO tags(job_id,idx,value) VALUES(?1,?2,?3);"};
    for (std::size_t i = 0; i < record.payload.tags.size(); ++i) {
        bind_text(insert_tag.get(), 1, record.id);
        if (sqlite3_bind_int64(insert_tag.get(), 2, static_cast<std::int64_t>(i)) != SQLITE_OK) {
            throw std::runtime_error("failed to bind tag index");
        }
        bind_text(insert_tag.get(), 3, record.payload.tags[i]);
        if (sqlite3_step(insert_tag.get()) != SQLITE_DONE) {
            throw std::runtime_error("failed to insert tag");
        }
        sqlite3_reset(insert_tag.get());
        sqlite3_clear_bindings(insert_tag.get());
    }

    tx.commit();
}

void SqliteJobRepository::transition(std::string_view id, JobState state, std::optional<std::string> error) {
    std::scoped_lock lock(impl_->mutex);
    Transaction tx{impl_->db};
    const auto now = Clock::now();

    Statement stmt{impl_->db,
                   "UPDATE jobs SET state=?1, updated_at=?2, error_message=?3 WHERE id=?4;"};
    bind_text(stmt.get(), 1, std::string{to_string(state)});
    if (sqlite3_bind_int64(stmt.get(), 2, to_micros(now)) != SQLITE_OK) {
        throw std::runtime_error("failed to bind transition timestamp");
    }
    bind_optional_text(stmt.get(), 3, error);
    bind_text(stmt.get(), 4, std::string{id});

    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("failed to update job state");
    }

    tx.commit();
}

void SqliteJobRepository::remove(std::string_view id) {
    std::scoped_lock lock(impl_->mutex);
    Statement stmt{impl_->db, "DELETE FROM jobs WHERE id=?1;"};
    bind_text(stmt.get(), 1, std::string{id});
    if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
        throw std::runtime_error("failed to remove job");
    }
}

} // namespace ppp::core

#endif // PPP_CORE_HAVE_SQLITE

#include "ppp/core/job_repository.h"

#if PPP_CORE_HAVE_SQLSERVER

#include "ppp/core/job.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <sqlext.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ppp::core {

namespace {

constexpr int kLatestSchemaVersion = 7;

[[nodiscard]] std::int64_t to_micros(const TimePoint& tp) {
    return std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] TimePoint from_micros(std::int64_t value) {
    return TimePoint{std::chrono::microseconds{value}};
}

[[noreturn]] void throw_odbc_error(SQLRETURN rc, SQLSMALLINT handle_type, SQLHANDLE handle, std::string_view context) {
    std::ostringstream oss;
    if (!context.empty()) {
        oss << context;
    } else {
        oss << "ODBC error";
    }

    SQLCHAR sql_state[6]{};
    SQLCHAR message[512]{};
    SQLINTEGER native_error = 0;
    SQLSMALLINT text_length = 0;
    bool first = true;

    const auto append_diag = [&](SQLSMALLINT type, SQLHANDLE diag_handle) {
        SQLSMALLINT record = 1;
        while (true) {
            const SQLRETURN diag =
                SQLGetDiagRec(type, diag_handle, record, sql_state, &native_error, message, sizeof(message), &text_length);
            if (diag == SQL_NO_DATA) {
                break;
            }
            if (diag == SQL_INVALID_HANDLE) {
                break;
            }
            if (!first) {
                oss << " | ";
            } else {
                oss << ": ";
                first = false;
            }
            const auto length = text_length >= 0 ? static_cast<std::size_t>(text_length)
                                                 : std::strlen(reinterpret_cast<const char*>(message));
            oss << '[' << reinterpret_cast<const char*>(sql_state) << "] "
                << std::string(reinterpret_cast<const char*>(message), length);
            ++record;
        }
    };

    append_diag(handle_type, handle);
    if (first) {
        oss << " (rc=" << rc << ")";
    }

    throw std::runtime_error(oss.str());
}

void check_odbc(SQLRETURN rc, SQLSMALLINT handle_type, SQLHANDLE handle, std::string_view context) {
    if (!SQL_SUCCEEDED(rc)) {
        throw_odbc_error(rc, handle_type, handle, context);
    }
}

class Statement {
public:
    explicit Statement(SQLHDBC connection) {
        SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, connection, &handle_);
        check_odbc(rc, SQL_HANDLE_DBC, connection, "failed to allocate ODBC statement handle");
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement(Statement&& other) noexcept : handle_(other.handle_) {
        other.handle_ = SQL_NULL_HSTMT;
    }

    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            if (handle_ != SQL_NULL_HSTMT) {
                SQLFreeHandle(SQL_HANDLE_STMT, handle_);
            }
            handle_ = other.handle_;
            other.handle_ = SQL_NULL_HSTMT;
        }
        return *this;
    }

    ~Statement() {
        if (handle_ != SQL_NULL_HSTMT) {
            SQLFreeHandle(SQL_HANDLE_STMT, handle_);
        }
    }

    void exec(std::string_view sql) {
        std::string sql_copy{sql};
        SQLRETURN rc = SQLExecDirect(handle_, reinterpret_cast<SQLCHAR*>(sql_copy.data()),
                                     static_cast<SQLINTEGER>(sql_copy.size()));
        if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA) {
            std::string context = "failed to execute statement: ";
            context.append(sql);
            throw_odbc_error(rc, SQL_HANDLE_STMT, handle_, context);
        }
    }

    [[nodiscard]] bool fetch() {
        SQLRETURN rc = SQLFetch(handle_);
        if (rc == SQL_NO_DATA) {
            return false;
        }
        check_odbc(rc, SQL_HANDLE_STMT, handle_, "failed to fetch row");
        return true;
    }

    [[nodiscard]] SQLLEN row_count() const {
        SQLLEN count = 0;
        SQLRETURN rc = SQLRowCount(handle_, &count);
        check_odbc(rc, SQL_HANDLE_STMT, handle_, "failed to obtain affected row count");
        return count;
    }

    [[nodiscard]] std::optional<std::string> get_string(SQLUSMALLINT column) {
        SQLLEN indicator = 0;
        std::string result;
        std::array<char, 1024> buffer{};
        bool has_data = false;
        while (true) {
            SQLRETURN rc = SQLGetData(handle_, column, SQL_C_CHAR, buffer.data(), buffer.size(), &indicator);
            if (rc == SQL_NO_DATA) {
                if (!has_data) {
                    return std::nullopt;
                }
                break;
            }
            if (indicator == SQL_NULL_DATA) {
                return std::nullopt;
            }
            if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
                throw_odbc_error(rc, SQL_HANDLE_STMT, handle_, "failed to read string column");
            }
            has_data = true;
            result.append(buffer.data(), std::strlen(buffer.data()));
            if (rc == SQL_SUCCESS) {
                break;
            }
        }
        return result;
    }

    [[nodiscard]] std::string get_required_string(SQLUSMALLINT column) {
        auto value = get_string(column);
        if (!value) {
            throw std::runtime_error("unexpected NULL string column");
        }
        return *value;
    }

    [[nodiscard]] std::optional<std::int64_t> get_optional_bigint(SQLUSMALLINT column) {
        SQLBIGINT value = 0;
        SQLLEN indicator = 0;
        SQLRETURN rc = SQLGetData(handle_, column, SQL_C_SBIGINT, &value, sizeof(value), &indicator);
        if (rc == SQL_NO_DATA) {
            return std::nullopt;
        }
        if (indicator == SQL_NULL_DATA) {
            return std::nullopt;
        }
        check_odbc(rc, SQL_HANDLE_STMT, handle_, "failed to read bigint column");
        return static_cast<std::int64_t>(value);
    }

    [[nodiscard]] std::int32_t get_int(SQLUSMALLINT column) {
        SQLINTEGER value = 0;
        SQLLEN indicator = 0;
        SQLRETURN rc = SQLGetData(handle_, column, SQL_C_SLONG, &value, sizeof(value), &indicator);
        check_odbc(rc, SQL_HANDLE_STMT, handle_, "failed to read integer column");
        if (indicator == SQL_NULL_DATA) {
            throw std::runtime_error("unexpected NULL integer column");
        }
        return static_cast<std::int32_t>(value);
    }

    [[nodiscard]] std::uint32_t get_uint(SQLUSMALLINT column) {
        SQLUINTEGER value = 0;
        SQLLEN indicator = 0;
        SQLRETURN rc = SQLGetData(handle_, column, SQL_C_ULONG, &value, sizeof(value), &indicator);
        check_odbc(rc, SQL_HANDLE_STMT, handle_, "failed to read unsigned integer column");
        if (indicator == SQL_NULL_DATA) {
            throw std::runtime_error("unexpected NULL integer column");
        }
        return static_cast<std::uint32_t>(value);
    }

    SQLHSTMT handle() const { return handle_; }

private:
    SQLHSTMT handle_{SQL_NULL_HSTMT};
};

class Transaction {
public:
    explicit Transaction(SQLHDBC connection) : connection_(connection) {
        SQLRETURN rc = SQLSetConnectAttr(connection_, SQL_ATTR_AUTOCOMMIT,
                                         reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0);
        check_odbc(rc, SQL_HANDLE_DBC, connection_, "failed to disable autocommit");
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Transaction(Transaction&& other) noexcept : connection_(other.connection_), active_(other.active_) {
        other.active_ = false;
    }

    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            if (active_) {
                SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, connection_, SQL_ROLLBACK);
                if (!SQL_SUCCEEDED(rc)) {
                    std::fprintf(stderr, "ppp_core: ODBC ROLLBACK failed (rc=%d)\n", static_cast<int>(rc));
                }
            }
            restore_autocommit();
            connection_ = other.connection_;
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    ~Transaction() {
        if (active_) {
            SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, connection_, SQL_ROLLBACK);
            if (!SQL_SUCCEEDED(rc)) {
                std::fprintf(stderr, "ppp_core: ODBC ROLLBACK failed (rc=%d)\n", static_cast<int>(rc));
            }
        }
        restore_autocommit();
    }

    void commit() {
        check_odbc(SQLEndTran(SQL_HANDLE_DBC, connection_, SQL_COMMIT), SQL_HANDLE_DBC, connection_,
                   "failed to commit transaction");
        active_ = false;
    }

private:
    void restore_autocommit() {
        SQLRETURN rc =
            SQLSetConnectAttr(connection_, SQL_ATTR_AUTOCOMMIT, reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0);
        if (!SQL_SUCCEEDED(rc)) {
            std::fprintf(stderr, "ppp_core: failed to restore ODBC autocommit (rc=%d)\n", static_cast<int>(rc));
        }
    }

    SQLHDBC connection_{SQL_NULL_HDBC};
    bool active_{true};
};

[[nodiscard]] std::string quote(std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 3);
    quoted.append("N'");
    for (char ch : value) {
        if (ch == '\'') {
            quoted.append("''");
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

[[nodiscard]] std::string literal_or_null(const std::optional<std::string>& value) {
    if (!value) {
        return "NULL";
    }
    return quote(*value);
}

[[nodiscard]] std::string literal_or_null(const std::optional<TimePoint>& value) {
    if (!value) {
        return "NULL";
    }
    return std::to_string(to_micros(*value));
}

void exec(SQLHDBC connection, std::string_view sql) {
    Statement stmt{connection};
    stmt.exec(sql);
}

int read_schema_version(SQLHDBC connection) {
    Statement stmt{connection};
    stmt.exec("SELECT TOP (1) version FROM schema_version WITH (HOLDLOCK);");
    if (!stmt.fetch()) {
        return 0;
    }
    const auto value = stmt.get_optional_bigint(1);
    if (!value) {
        return 0;
    }
    return static_cast<int>(*value);
}

void write_schema_version(SQLHDBC connection, int version) {
    Statement stmt{connection};
    stmt.exec("UPDATE schema_version SET version=" + std::to_string(version) + ";");
    if (stmt.row_count() == 0) {
        exec(connection, "INSERT INTO schema_version(version) VALUES(" + std::to_string(version) + ");");
    }
}

bool column_exists(SQLHDBC connection, std::string_view table, std::string_view column) {
    Statement stmt{connection};
    std::string sql =
        "SELECT 1 FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_NAME=" + quote(table) + " AND COLUMN_NAME=" + quote(column) +
        ";";
    stmt.exec(sql);
    return stmt.fetch();
}

bool index_exists(SQLHDBC connection, std::string_view table, std::string_view index) {
    Statement stmt{connection};
    std::string sql =
        "SELECT 1 FROM sys.indexes WHERE name=" + quote(index) + " AND object_id=OBJECT_ID(" + quote(table) + ");";
    stmt.exec(sql);
    return stmt.fetch();
}

void ensure_schema(SQLHDBC connection) {
    exec(connection,
         "IF OBJECT_ID('schema_version','U') IS NULL BEGIN "
         "CREATE TABLE schema_version(version INT NOT NULL);"
         "INSERT INTO schema_version(version) VALUES(0);"
         "END;");

    int version = read_schema_version(connection);
    if (version > kLatestSchemaVersion) {
        throw std::runtime_error("database schema version is newer than this build supports");
    }

    struct Migration {
        int target_version;
        void (*apply)(SQLHDBC);
    };

    const Migration migrations[] = {
        {1, [](SQLHDBC conn) {
             exec(conn,
                  "IF OBJECT_ID('jobs','U') IS NULL BEGIN "
                  "CREATE TABLE jobs ("
                  " id NVARCHAR(64) NOT NULL PRIMARY KEY,"
                  " state NVARCHAR(32) NOT NULL,"
                  " created_at BIGINT NOT NULL,"
                  " updated_at BIGINT NULL,"
                  " source_path NVARCHAR(1024) NOT NULL,"
                  " profile_name NVARCHAR(256) NULL,"
                  " correlation_id NVARCHAR(256) NULL,"
                  " error_message NVARCHAR(MAX) NULL,"
                  " priority INT NOT NULL DEFAULT 0,"
                  " attempt_count INT NOT NULL DEFAULT 0,"
                  " last_attempt_at BIGINT NULL,"
                  " due_at BIGINT NULL"
                  ");"
                  "END;");
             exec(conn,
                  "IF OBJECT_ID('attachments','U') IS NULL BEGIN "
                  "CREATE TABLE attachments ("
                  " job_id NVARCHAR(64) NOT NULL,"
                  " idx INT NOT NULL,"
                  " path NVARCHAR(1024) NOT NULL,"
                  " PRIMARY KEY(job_id, idx),"
                  " CONSTRAINT fk_attachments_job FOREIGN KEY(job_id) REFERENCES jobs(id) ON DELETE CASCADE"
                  ");"
                  "END;");
         }},
        {2, [](SQLHDBC conn) {
             if (!index_exists(conn, "jobs", "idx_jobs_state_priority_created")) {
                 exec(conn,
                      "CREATE INDEX idx_jobs_state_priority_created ON jobs(state, priority DESC, created_at, id);");
             }
         }},
        {3, [](SQLHDBC conn) {
             if (!column_exists(conn, "jobs", "attempt_count")) {
                 exec(conn, "ALTER TABLE jobs ADD attempt_count INT NOT NULL DEFAULT 0;");
             }
         }},
        {4, [](SQLHDBC conn) {
             if (!column_exists(conn, "jobs", "last_attempt_at")) {
                 exec(conn, "ALTER TABLE jobs ADD last_attempt_at BIGINT NULL;");
             }
         }},
        {5, [](SQLHDBC conn) {
             if (!column_exists(conn, "jobs", "priority")) {
                 exec(conn, "ALTER TABLE jobs ADD priority INT NOT NULL DEFAULT 0;");
             }
             exec(conn,
                  "IF EXISTS (SELECT 1 FROM sys.indexes WHERE name='idx_jobs_state_priority_created' "
                  "AND object_id=OBJECT_ID('jobs')) "
                  "DROP INDEX idx_jobs_state_priority_created ON jobs;");
             exec(conn,
                  "CREATE INDEX idx_jobs_state_priority_created ON jobs(state, priority DESC, created_at, id);");
         }},
        {6, [](SQLHDBC conn) {
             if (!column_exists(conn, "jobs", "due_at")) {
                 exec(conn, "ALTER TABLE jobs ADD due_at BIGINT NULL;");
             }
             exec(conn,
                  "IF EXISTS (SELECT 1 FROM sys.indexes WHERE name='idx_jobs_state_priority_created' "
                  "AND object_id=OBJECT_ID('jobs')) "
                  "DROP INDEX idx_jobs_state_priority_created ON jobs;");
             exec(conn,
                  "IF EXISTS (SELECT 1 FROM sys.indexes WHERE name='idx_jobs_state_priority_due_created' "
                  "AND object_id=OBJECT_ID('jobs')) "
                  "DROP INDEX idx_jobs_state_priority_due_created ON jobs;");
             exec(conn,
                  "CREATE INDEX idx_jobs_state_priority_due_created ON jobs("
                  " state ASC, priority DESC, due_at ASC, created_at ASC, id ASC);");
         }},
        {7, [](SQLHDBC conn) {
             exec(conn,
                  "IF OBJECT_ID('tags','U') IS NULL BEGIN "
                  "CREATE TABLE tags ("
                  " job_id NVARCHAR(64) NOT NULL,"
                  " idx INT NOT NULL,"
                  " value NVARCHAR(256) NOT NULL,"
                  " PRIMARY KEY(job_id, idx),"
                  " CONSTRAINT fk_tags_job FOREIGN KEY(job_id) REFERENCES jobs(id) ON DELETE CASCADE"
                  ");"
                  "END;");
         }},
    };

    for (const auto& migration : migrations) {
        if (version < migration.target_version) {
            Transaction tx{connection};
            migration.apply(connection);
            write_schema_version(connection, migration.target_version);
            tx.commit();
            version = migration.target_version;
        }
    }
}

std::vector<std::string> load_collection(SQLHDBC connection, std::string_view table, std::string_view column,
                                         std::string_view id) {
    Statement stmt{connection};
    std::string sql = "SELECT " + std::string{column} + " FROM " + std::string{table} + " WHERE job_id=" + quote(id) +
                      " ORDER BY idx ASC;";
    stmt.exec(sql);
    std::vector<std::string> values;
    while (stmt.fetch()) {
        values.push_back(stmt.get_required_string(1));
    }
    return values;
}

JobRecord hydrate_job(Statement& stmt) {
    JobRecord record;
    record.id = stmt.get_required_string(1);
    const auto state_text = stmt.get_required_string(2);
    const auto state = job_state_from_string(state_text);
    if (!state) {
        throw std::runtime_error("invalid job state stored in SQL Server repository");
    }
    record.state = *state;

    const auto created = stmt.get_optional_bigint(3);
    if (!created) {
        throw std::runtime_error("job row missing created_at");
    }
    record.created_at = from_micros(*created);

    if (const auto updated = stmt.get_optional_bigint(4)) {
        record.updated_at = from_micros(*updated);
    }

    record.payload.source_path = stmt.get_required_string(5);
    record.payload.profile_name = stmt.get_string(6);
    record.correlation_id = stmt.get_string(7);
    record.error_message = stmt.get_string(8);
    record.priority = stmt.get_int(9);
    record.attempt_count = stmt.get_uint(10);
    if (const auto attempt = stmt.get_optional_bigint(11)) {
        record.last_attempt_at = from_micros(*attempt);
    }
    if (const auto due = stmt.get_optional_bigint(12)) {
        record.due_at = from_micros(*due);
    }

    return record;
}

std::optional<JobRecord> load_job(SQLHDBC connection, std::string_view id) {
    Statement stmt{connection};
    std::string sql =
        "SELECT id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,"
        "attempt_count,last_attempt_at,due_at FROM jobs WHERE id=" +
        quote(id) + ";";
    stmt.exec(sql);
    if (!stmt.fetch()) {
        return std::nullopt;
    }
    auto record = hydrate_job(stmt);
    record.payload.attachments = load_collection(connection, "attachments", "path", record.id);
    record.payload.tags = load_collection(connection, "tags", "value", record.id);
    return record;
}

} // namespace

struct SqlServerJobRepository::Impl {
    explicit Impl(std::string connection_string) : connection_string_(std::move(connection_string)) {
        if (connection_string_.empty()) {
            throw std::invalid_argument("connection string must not be empty");
        }
        SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_);
        check_odbc(rc, SQL_HANDLE_ENV, environment_, "failed to allocate ODBC environment");
        rc = SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
        check_odbc(rc, SQL_HANDLE_ENV, environment_, "failed to configure ODBC environment");
        rc = SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_);
        check_odbc(rc, SQL_HANDLE_ENV, environment_, "failed to allocate ODBC connection");

        rc = SQLDriverConnect(connection_, nullptr,
                              reinterpret_cast<SQLCHAR*>(connection_string_.data()),
                              SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
        if (!SQL_SUCCEEDED(rc)) {
            throw_odbc_error(rc, SQL_HANDLE_DBC, connection_, "failed to connect to SQL Server");
        }

        ensure_schema(connection_);
    }

    ~Impl() {
        if (connection_ != SQL_NULL_HDBC) {
            SQLDisconnect(connection_);
            SQLFreeHandle(SQL_HANDLE_DBC, connection_);
        }
        if (environment_ != SQL_NULL_HENV) {
            SQLFreeHandle(SQL_HANDLE_ENV, environment_);
        }
    }

    std::optional<JobRecord> fetch(std::string_view id) const {
        std::scoped_lock lock(mutex_);
        return load_job(connection_, id);
    }

    std::vector<JobRecord> list(std::optional<JobState> state_filter) const {
        std::scoped_lock lock(mutex_);
        Statement stmt{connection_};
        std::string sql =
            "SELECT id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,"
            "attempt_count,last_attempt_at,due_at FROM jobs";
        if (state_filter) {
            sql += " WHERE state=" + quote(std::string{to_string(*state_filter)});
        }
        sql += " ORDER BY created_at ASC, id ASC;";
        stmt.exec(sql);
        std::vector<JobRecord> records;
        while (stmt.fetch()) {
            auto record = hydrate_job(stmt);
            record.payload.attachments = load_collection(connection_, "attachments", "path", record.id);
            record.payload.tags = load_collection(connection_, "tags", "value", record.id);
            records.push_back(std::move(record));
        }
        return records;
    }

    std::optional<JobRecord> claim_next_submitted(JobState next_state) {
        std::scoped_lock lock(mutex_);
        Transaction tx{connection_};

        Statement select{connection_};
        select.exec(
            "SELECT TOP (1) id FROM jobs WITH (UPDLOCK, READPAST, ROWLOCK) WHERE state=" +
            quote(std::string{to_string(JobState::Submitted)}) +
            " ORDER BY priority DESC, CASE WHEN due_at IS NULL THEN 1 ELSE 0 END ASC, due_at ASC, created_at ASC, id ASC;");
        if (!select.fetch()) {
            return std::nullopt;
        }
        const auto id = select.get_required_string(1);

        const auto now = Clock::now();
        const auto micros = to_micros(now);
        Statement update{connection_};
        update.exec("UPDATE jobs SET state=" + quote(std::string{to_string(next_state)}) + ", updated_at=" +
                    std::to_string(micros) + ", last_attempt_at=" + std::to_string(micros) +
                    ", error_message=NULL, attempt_count=attempt_count+1 WHERE id=" + quote(id) + ";");
        if (update.row_count() == 0) {
            throw std::runtime_error("failed to update claimed job state");
        }

        auto record = load_job(connection_, id);
        if (!record) {
            throw std::runtime_error("failed to reload claimed job");
        }

        tx.commit();
        return record;
    }

    void upsert(JobRecord record) {
        std::scoped_lock lock(mutex_);
        record.updated_at = Clock::now();
        Transaction tx{connection_};

        const std::string state_literal = quote(std::string{to_string(record.state)});
        const std::string id_literal = quote(record.id);
        const std::string insert_or_update =
            "IF EXISTS (SELECT 1 FROM jobs WHERE id=" + id_literal + ") "
            "BEGIN "
            "UPDATE jobs SET "
            " state=" + state_literal + ","
            " created_at=" + std::to_string(to_micros(record.created_at)) + ","
            " updated_at=" + literal_or_null(record.updated_at) + ","
            " source_path=" + quote(record.payload.source_path) + ","
            " profile_name=" + literal_or_null(record.payload.profile_name) + ","
            " correlation_id=" + literal_or_null(record.correlation_id) + ","
            " error_message=" + literal_or_null(record.error_message) + ","
            " priority=" + std::to_string(record.priority) + ","
            " attempt_count=" + std::to_string(record.attempt_count) + ","
            " last_attempt_at=" + literal_or_null(record.last_attempt_at) + ","
            " due_at=" + literal_or_null(record.due_at) +
            " WHERE id=" + id_literal + ";"
            "END "
            "ELSE "
            "BEGIN "
            "INSERT INTO jobs(id,state,created_at,updated_at,source_path,profile_name,correlation_id,error_message,priority,attempt_count,last_attempt_at,due_at) "
            "VALUES(" + id_literal + "," + state_literal + "," + std::to_string(to_micros(record.created_at)) + "," +
            literal_or_null(record.updated_at) + "," + quote(record.payload.source_path) + "," +
            literal_or_null(record.payload.profile_name) + "," + literal_or_null(record.correlation_id) + "," +
            literal_or_null(record.error_message) + "," + std::to_string(record.priority) + "," +
            std::to_string(record.attempt_count) + "," + literal_or_null(record.last_attempt_at) + "," +
            literal_or_null(record.due_at) + ");"
            "END;";
        exec(connection_, insert_or_update);

        exec(connection_, "DELETE FROM attachments WHERE job_id=" + id_literal + ";");
        for (std::size_t i = 0; i < record.payload.attachments.size(); ++i) {
            std::string sql =
                "INSERT INTO attachments(job_id,idx,path) VALUES(" + id_literal + "," + std::to_string(i) + "," +
                quote(record.payload.attachments[i]) + ");";
            exec(connection_, sql);
        }

        exec(connection_, "DELETE FROM tags WHERE job_id=" + id_literal + ";");
        for (std::size_t i = 0; i < record.payload.tags.size(); ++i) {
            std::string sql =
                "INSERT INTO tags(job_id,idx,value) VALUES(" + id_literal + "," + std::to_string(i) + "," +
                quote(record.payload.tags[i]) + ");";
            exec(connection_, sql);
        }

        tx.commit();
    }

    void transition(std::string_view id, JobState state, std::optional<std::string> error) {
        std::scoped_lock lock(mutex_);
        Transaction tx{connection_};
        const auto now = Clock::now();
        const auto micros = to_micros(now);
        std::string sql =
            "UPDATE jobs SET state=" + quote(std::string{to_string(state)}) + ", updated_at=" + std::to_string(micros) +
            ", error_message=" + literal_or_null(error) + " WHERE id=" + quote(id) + ";";
        Statement stmt{connection_};
        stmt.exec(sql);
        if (stmt.row_count() == 0) {
            throw std::runtime_error("failed to update job state");
        }
        tx.commit();
    }

    void remove(std::string_view id) {
        std::scoped_lock lock(mutex_);
        Statement stmt{connection_};
        stmt.exec("DELETE FROM jobs WHERE id=" + quote(id) + ";");
        [[maybe_unused]] const auto removed = stmt.row_count();
    }

    std::string connection_string_;
    SQLHENV environment_{SQL_NULL_HENV};
    SQLHDBC connection_{SQL_NULL_HDBC};
    mutable std::mutex mutex_;
};

SqlServerJobRepository::SqlServerJobRepository(std::string connection_string)
    : impl_(std::make_unique<Impl>(std::move(connection_string))) {}

SqlServerJobRepository::~SqlServerJobRepository() = default;

void SqlServerJobRepository::initialize_schema(std::string connection_string) {
    Impl initializer(std::move(connection_string));
}

int SqlServerJobRepository::latest_schema_version() {
    return kLatestSchemaVersion;
}

std::optional<JobRecord> SqlServerJobRepository::fetch(std::string_view id) const {
    return impl_->fetch(id);
}

std::vector<JobRecord> SqlServerJobRepository::list(std::optional<JobState> state_filter) const {
    return impl_->list(std::move(state_filter));
}

std::optional<JobRecord> SqlServerJobRepository::claim_next_submitted(JobState next_state) {
    return impl_->claim_next_submitted(next_state);
}

void SqlServerJobRepository::upsert(JobRecord record) {
    impl_->upsert(std::move(record));
}

void SqlServerJobRepository::transition(std::string_view id, JobState state, std::optional<std::string> error) {
    impl_->transition(id, state, std::move(error));
}

void SqlServerJobRepository::remove(std::string_view id) {
    impl_->remove(id);
}

} // namespace ppp::core

#endif // PPP_CORE_HAVE_SQLSERVER

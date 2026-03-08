#pragma once

#if PPP_CORE_HAVE_SQLITE

#include <sqlite3.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ppp::core::sqlite_support {

inline void exec(sqlite3* db, std::string_view sql) {
    std::string query{sql};
    char* error_message = nullptr;
    const auto rc = sqlite3_exec(db, query.c_str(), nullptr, nullptr, &error_message);
    if (rc != SQLITE_OK) {
        std::string message = "SQLite exec failed";
        if (error_message) {
            message += ": ";
            message += error_message;
        }
        sqlite3_free(error_message);
        throw std::runtime_error(message);
    }
}

class Statement {
public:
    Statement(sqlite3* db, std::string_view sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string{"Failed to prepare statement: "} + sqlite3_errmsg(db_));
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) { other.stmt_ = nullptr; }
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            finalize();
            db_ = other.db_;
            stmt_ = other.stmt_;
            other.stmt_ = nullptr;
        }
        return *this;
    }

    ~Statement() { finalize(); }

    sqlite3_stmt* get() const { return stmt_; }

private:
    void finalize() {
        if (stmt_) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
    }

    sqlite3* db_;
    sqlite3_stmt* stmt_{nullptr};
};

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { exec(db_, "BEGIN IMMEDIATE TRANSACTION;"); }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Transaction(Transaction&& other) noexcept : db_(other.db_), active_(other.active_) { other.active_ = false; }
    Transaction& operator=(Transaction&& other) noexcept {
        if (this != &other) {
            if (active_) {
                char* err = nullptr;
                if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK) {
                    std::fprintf(stderr, "ppp_core: ROLLBACK failed: %s\n", err ? err : "unknown error");
                    sqlite3_free(err);
                }
            }
            db_ = other.db_;
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    ~Transaction() {
        if (active_) {
            char* err = nullptr;
            if (sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, &err) != SQLITE_OK) {
                std::fprintf(stderr, "ppp_core: ROLLBACK failed: %s\n", err ? err : "unknown error");
                sqlite3_free(err);
            }
        }
    }

    void commit() {
        exec(db_, "COMMIT;");
        active_ = false;
    }

private:
    sqlite3* db_;
    bool active_{true};
};

} // namespace ppp::core::sqlite_support

#endif // PPP_CORE_HAVE_SQLITE


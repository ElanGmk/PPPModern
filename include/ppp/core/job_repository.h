#pragma once

#include "ppp/core/job.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ppp::core {

/// Represents durable storage for PPP jobs.
class JobRepository {
public:
    virtual ~JobRepository() = default;

    [[nodiscard]] virtual std::optional<JobRecord> fetch(std::string_view id) const = 0;
    [[nodiscard]] virtual std::vector<JobRecord> list(std::optional<JobState> state_filter = std::nullopt) const = 0;

    /// Claims the oldest submitted job and transitions it to the provided state.
    /// Returns the claimed record (already updated to the new state) or
    /// std::nullopt when no submitted jobs are available.
    [[nodiscard]] virtual std::optional<JobRecord> claim_next_submitted(JobState next_state) = 0;

    virtual void upsert(JobRecord record) = 0;
    virtual void transition(std::string_view id, JobState state,
                            std::optional<std::string> error = std::nullopt) = 0;
    virtual void remove(std::string_view id) = 0;
};

/// In-memory repository useful for prototyping and tests.
class InMemoryJobRepository final : public JobRepository {
public:
    InMemoryJobRepository();
    ~InMemoryJobRepository() override;

    [[nodiscard]] std::optional<JobRecord> fetch(std::string_view id) const override;
    [[nodiscard]] std::vector<JobRecord> list(std::optional<JobState> state_filter) const override;

    [[nodiscard]] std::optional<JobRecord> claim_next_submitted(JobState next_state) override;

    void upsert(JobRecord record) override;
    void transition(std::string_view id, JobState state,
                    std::optional<std::string> error) override;
    void remove(std::string_view id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// File-backed repository that persists job state to disk using a simple
/// human-readable format. Suitable for air-gapped deployments and smoke tests
/// where a full database is unavailable.
class FileJobRepository final : public JobRepository {
public:
    explicit FileJobRepository(std::filesystem::path root_directory);
    ~FileJobRepository() override;

    [[nodiscard]] std::optional<JobRecord> fetch(std::string_view id) const override;
    [[nodiscard]] std::vector<JobRecord> list(std::optional<JobState> state_filter) const override;

    [[nodiscard]] std::optional<JobRecord> claim_next_submitted(JobState next_state) override;

    void upsert(JobRecord record) override;
    void transition(std::string_view id, JobState state,
                    std::optional<std::string> error) override;
    void remove(std::string_view id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#if PPP_CORE_HAVE_SQLITE
/// SQLite-backed repository that stores jobs in a relational database. Requires
/// SQLite support to be enabled at build time.
class SqliteJobRepository final : public JobRepository {
public:
    explicit SqliteJobRepository(std::filesystem::path database_path);
    ~SqliteJobRepository() override;

    /// Ensure the database at `database_path` has the latest schema. This can be
    /// invoked separately by tooling prior to connecting workers in production
    /// environments.
    static void initialize_schema(const std::filesystem::path& database_path);

    /// Returns the most recent schema version understood by the repository.
    static int latest_schema_version();

    [[nodiscard]] std::optional<JobRecord> fetch(std::string_view id) const override;
    [[nodiscard]] std::vector<JobRecord> list(std::optional<JobState> state_filter) const override;

    [[nodiscard]] std::optional<JobRecord> claim_next_submitted(JobState next_state) override;

    void upsert(JobRecord record) override;
    void transition(std::string_view id, JobState state,
                    std::optional<std::string> error) override;
    void remove(std::string_view id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif // PPP_CORE_HAVE_SQLITE

#if PPP_CORE_HAVE_SQLSERVER
/// SQL Server-backed repository that mirrors the SQLite schema using ODBC. Requires
/// PPP_ENABLE_SQLSERVER and an available ODBC driver for Microsoft SQL Server.
class SqlServerJobRepository final : public JobRepository {
public:
    explicit SqlServerJobRepository(std::string connection_string);
    ~SqlServerJobRepository() override;

    /// Ensure the target database has the expected schema version. This connects
    /// using the provided ODBC connection string and applies any pending
    /// migrations.
    static void initialize_schema(std::string connection_string);

    /// Returns the latest schema version supported by this adapter.
    static int latest_schema_version();

    [[nodiscard]] std::optional<JobRecord> fetch(std::string_view id) const override;
    [[nodiscard]] std::vector<JobRecord> list(std::optional<JobState> state_filter) const override;

    [[nodiscard]] std::optional<JobRecord> claim_next_submitted(JobState next_state) override;

    void upsert(JobRecord record) override;
    void transition(std::string_view id, JobState state,
                    std::optional<std::string> error) override;
    void remove(std::string_view id) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif // PPP_CORE_HAVE_SQLSERVER

} // namespace ppp::core

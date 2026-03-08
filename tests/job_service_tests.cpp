#include "ppp/core/job_processor.h"
#include "ppp/core/job_repository.h"
#include "ppp/core/job_serialization.h"
#include "ppp/core/job_service.h"
#include "ppp/core/processing_config.h"
#include "ppp/core/processing_config_io.h"
#include "ppp/core/scheduling_policy.h"
#include "ppp/core/scheduling_policy_io.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if PPP_CORE_HAVE_SQLITE
#include <sqlite3.h>
#endif

namespace {

namespace fs = std::filesystem;

struct TempRepository {
    fs::path path;

    TempRepository() {
        std::error_code ec;
        auto base = fs::temp_directory_path() / "ppp_core_tests";
        fs::create_directories(base, ec);
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto entropy = static_cast<unsigned long long>(std::random_device{}());
        path = base / ("repo-" + std::to_string(timestamp) + "-" + std::to_string(entropy));
        fs::create_directories(path, ec);
        if (ec) {
            throw std::runtime_error("failed to create repository directory");
        }
    }

    ~TempRepository() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool job_records_equal(const ppp::core::JobRecord& lhs, const ppp::core::JobRecord& rhs) {
    return lhs.id == rhs.id && lhs.state == rhs.state && lhs.payload.source_path == rhs.payload.source_path &&
           lhs.payload.profile_name == rhs.payload.profile_name && lhs.payload.attachments == rhs.payload.attachments &&
           lhs.payload.tags == rhs.payload.tags && lhs.created_at == rhs.created_at &&
           lhs.updated_at == rhs.updated_at && lhs.correlation_id == rhs.correlation_id &&
           lhs.error_message == rhs.error_message && lhs.priority == rhs.priority &&
           lhs.attempt_count == rhs.attempt_count && lhs.last_attempt_at == rhs.last_attempt_at && lhs.due_at == rhs.due_at;
}

bool test_job_state_round_trip() {
    using ppp::core::JobState;
    for (JobState state : {JobState::Submitted, JobState::Validating, JobState::Rendering,
                           JobState::Exception, JobState::Completed, JobState::Cancelled}) {
        const auto round_trip = ppp::core::job_state_from_string(ppp::core::to_string(state));
        if (!round_trip || *round_trip != state) {
            std::cerr << "job_state_from_string failed round-trip for state "
                      << static_cast<int>(state) << std::endl;
            return false;
        }
    }
    if (ppp::core::job_state_from_string("bogus")) {
        std::cerr << "job_state_from_string accepted invalid input" << std::endl;
        return false;
    }
    return true;
}

bool test_scheduling_policy_config_parsing() {
    const std::string config =
        R"CFG(# Escalation policy
within-minutes=120 priority=50
within-minutes=60,priority=80
within-minutes=15 priority=95 ; urgent window
overdue=110
escalate-overdue=false
)CFG";

    std::istringstream stream{config};
    const auto policy = ppp::core::read_scheduling_policy(stream);
    if (policy.escalations.size() != 3) {
        std::cerr << "expected 3 escalations, found " << policy.escalations.size() << std::endl;
        return false;
    }
    const auto minutes0 = std::chrono::duration_cast<std::chrono::minutes>(policy.escalations[0].within);
    const auto minutes1 = std::chrono::duration_cast<std::chrono::minutes>(policy.escalations[1].within);
    const auto minutes2 = std::chrono::duration_cast<std::chrono::minutes>(policy.escalations[2].within);
    if (minutes0.count() != 120 || policy.escalations[0].priority != 50) {
        std::cerr << "unexpected first escalation" << std::endl;
        return false;
    }
    if (minutes1.count() != 60 || policy.escalations[1].priority != 80) {
        std::cerr << "unexpected second escalation" << std::endl;
        return false;
    }
    if (minutes2.count() != 15 || policy.escalations[2].priority != 95) {
        std::cerr << "unexpected third escalation" << std::endl;
        return false;
    }
    if (!policy.overdue_priority || *policy.overdue_priority != 110) {
        std::cerr << "expected overdue priority of 110" << std::endl;
        return false;
    }
    if (policy.escalate_overdue) {
        std::cerr << "expected overdue escalation to be disabled" << std::endl;
        return false;
    }

    std::istringstream invalid{"within-minutes=30"};
    bool threw = false;
    try {
        static_cast<void>(ppp::core::read_scheduling_policy(invalid));
    } catch (const std::exception&) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "expected parser to reject missing priority" << std::endl;
        return false;
    }

    return true;
}

bool test_scheduling_policy_directory_layering() {
    TempRepository temp;
    const auto policy_dir = temp.path / "policies";
    fs::create_directories(policy_dir);

    {
        std::ofstream base(policy_dir / "00-base.policy");
        base << "within-minutes=120 priority=40\n";
        base << "overdue=90\n";
    }
    {
        std::ofstream disable(policy_dir / "05-disable.cfg");
        disable << "no-overdue-escalation\n";
    }
    {
        std::ofstream urgent(policy_dir / "10-urgent.policy");
        urgent << "within-minutes=30 priority=80\n";
        urgent << "escalate-overdue=true\n";
    }
    {
        std::ofstream ignored(policy_dir / "readme.md");
        ignored << "within-minutes=1 priority=100\n";
    }

    const auto directory_policy = ppp::core::load_scheduling_policy_directory(policy_dir);
    if (!directory_policy.has_escalations || directory_policy.policy.escalations.size() != 2) {
        std::cerr << "expected two escalations from layered directory policy" << std::endl;
        return false;
    }
    if (!directory_policy.overdue_priority_specified ||
        !directory_policy.policy.overdue_priority || *directory_policy.policy.overdue_priority != 90) {
        std::cerr << "expected overdue priority of 90 from base policy" << std::endl;
        return false;
    }
    if (!directory_policy.escalate_overdue_specified || !directory_policy.policy.escalate_overdue) {
        std::cerr << "expected overdue escalation to be re-enabled by urgent policy" << std::endl;
        return false;
    }
    const auto minutes0 = std::chrono::duration_cast<std::chrono::minutes>(directory_policy.policy.escalations[0].within);
    const auto minutes1 = std::chrono::duration_cast<std::chrono::minutes>(directory_policy.policy.escalations[1].within);
    if (minutes0.count() != 120 || directory_policy.policy.escalations[0].priority != 40) {
        std::cerr << "unexpected first escalation from directory policy" << std::endl;
        return false;
    }
    if (minutes1.count() != 30 || directory_policy.policy.escalations[1].priority != 80) {
        std::cerr << "unexpected second escalation from directory policy" << std::endl;
        return false;
    }

    std::ofstream override_file(temp.path / "override.policy");
    override_file << "overdue=120\n";
    override_file.close();

    const auto stacked_policy =
        ppp::core::load_scheduling_policy_stack({policy_dir, temp.path / "override.policy"});
    if (!stacked_policy.overdue_priority_specified ||
        !stacked_policy.policy.overdue_priority || *stacked_policy.policy.overdue_priority != 120) {
        std::cerr << "expected overdue priority to be overridden by stacked policy" << std::endl;
        return false;
    }
    if (!stacked_policy.has_escalations || stacked_policy.policy.escalations.size() != 2) {
        std::cerr << "expected stacked policy to retain directory escalations" << std::endl;
        return false;
    }
    if (!stacked_policy.escalate_overdue_specified || !stacked_policy.policy.escalate_overdue) {
        std::cerr << "expected stacked policy to preserve overdue escalation directive" << std::endl;
        return false;
    }

    return true;
}

bool test_file_repository_persistence() {
    TempRepository temp;
    ppp::core::FileJobRepository repository{temp.path};
    ppp::core::JobService service{repository};

    ppp::core::JobPayload payload{.source_path = "input.tif"};
    payload.profile_name = "default";
    payload.attachments = {"cover.pdf", "notes.txt"};

    const auto id = service.create_job(payload, "corr-123");
    service.mark_validating(id);
    service.mark_failed(id, "checksum mismatch");

    auto record = service.get(id);
    if (!record) {
        std::cerr << "failed to fetch job after creation" << std::endl;
        return false;
    }
    if (record->attempt_count != 0) {
        std::cerr << "expected initial attempt_count to be zero" << std::endl;
        return false;
    }
    if (record->priority != 0) {
        std::cerr << "expected initial priority to be zero" << std::endl;
        return false;
    }
    if (record->last_attempt_at) {
        std::cerr << "last_attempt_at should be empty before any claims" << std::endl;
        return false;
    }
    if (record->state != ppp::core::JobState::Exception) {
        std::cerr << "expected job state to be exception" << std::endl;
        return false;
    }
    if (!record->error_message || *record->error_message != "checksum mismatch") {
        std::cerr << "expected error message to persist" << std::endl;
        return false;
    }
    if (record->payload.attachments.size() != 2) {
        std::cerr << "unexpected attachment count" << std::endl;
        return false;
    }
    if (record->due_at) {
        std::cerr << "due_at should be empty before being scheduled" << std::endl;
        return false;
    }

    ppp::core::FileJobRepository reopened{temp.path};
    auto persisted = reopened.fetch(id);
    if (!persisted) {
        std::cerr << "failed to reload job from disk" << std::endl;
        return false;
    }
    if (persisted->attempt_count != 0) {
        std::cerr << "expected persisted attempt_count to remain zero" << std::endl;
        return false;
    }
    if (persisted->priority != 0) {
        std::cerr << "expected persisted priority to remain zero" << std::endl;
        return false;
    }
    if (persisted->last_attempt_at) {
        std::cerr << "persisted last_attempt_at should remain empty before claims" << std::endl;
        return false;
    }
    if (!persisted->payload.profile_name || *persisted->payload.profile_name != "default") {
        std::cerr << "profile name did not persist" << std::endl;
        return false;
    }
    if (persisted->payload.attachments.size() != 2) {
        std::cerr << "attachments did not persist" << std::endl;
        return false;
    }
    if (persisted->due_at) {
        std::cerr << "persisted due_at should remain empty before scheduling" << std::endl;
        return false;
    }

    ppp::core::JobService reopened_service{reopened};
    (void)reopened_service.update_priority(id, 5);
    auto reprioritized = reopened_service.get(id);
    if (!reprioritized || reprioritized->priority != 5) {
        std::cerr << "failed to update persisted priority" << std::endl;
        return false;
    }

    auto filtered = reopened_service.list(ppp::core::JobState::Exception);
    if (filtered.size() != 1) {
        std::cerr << "state-filtered listing incorrect (size=" << filtered.size() << ")" << std::endl;
        return false;
    }
    if (filtered.front().id != id) {
        std::cerr << "state-filtered listing returned unexpected job" << std::endl;
        return false;
    }

    ppp::core::JobPayload payload2{.source_path = "second.tif"};
    const auto second_id = reopened_service.create_job(payload2);
    (void)reopened_service.retry(id);
    reopened_service.mark_completed(second_id);

    auto jobs = reopened_service.list();
    if (jobs.size() != 2) {
        std::cerr << "unexpected job count after second insert" << std::endl;
        return false;
    }
    auto completed = reopened_service.get(second_id);
    if (!completed || completed->state != ppp::core::JobState::Completed) {
        std::cerr << "failed to persist completed job" << std::endl;
        return false;
    }

    auto claimed = reopened_service.claim_next_submitted();
    if (!claimed || claimed->id != id || claimed->priority != 5) {
        std::cerr << "expected reprioritized job to be claimed first" << std::endl;
        return false;
    }

    return true;
}

bool test_priority_ordering_in_memory() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    ppp::core::JobPayload payload{.source_path = "priority.tif"};
    const auto low_id = service.create_job(payload, std::nullopt, 0);
    const auto high_id = service.create_job(payload, std::nullopt, 5);
    const auto mid_id = service.create_job(payload, std::nullopt, 3);

    auto first = service.claim_next_submitted();
    if (!first || first->id != high_id) {
        std::cerr << "expected highest priority job to be claimed first" << std::endl;
        return false;
    }

    (void)service.retry(first->id);
    (void)service.update_priority(low_id, 10);

    auto second = service.claim_next_submitted();
    if (!second || second->id != low_id || second->priority != 10) {
        std::cerr << "expected reprioritized job to win subsequent claim" << std::endl;
        return false;
    }

    auto third = service.claim_next_submitted();
    if (!third || third->id != high_id) {
        std::cerr << "expected original high priority job next" << std::endl;
        return false;
    }

    service.mark_completed(third->id);

    auto fourth = service.claim_next_submitted();
    if (!fourth || fourth->id != mid_id) {
        std::cerr << "expected remaining job to be claimed last" << std::endl;
        return false;
    }

    return true;
}

bool test_due_ordering_in_memory() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    ppp::core::JobPayload payload{.source_path = "due.tif"};
    const auto base = ppp::core::Clock::now() + std::chrono::minutes(10);
    const auto early_due = base - std::chrono::minutes(5);
    const auto late_due = base + std::chrono::minutes(5);

    const auto later_id = service.create_job(payload, std::nullopt, 1, late_due);
    const auto early_id = service.create_job(payload, std::nullopt, 1, early_due);
    const auto none_id = service.create_job(payload, std::nullopt, 1);

    auto first = service.claim_next_submitted();
    if (!first || first->id != early_id) {
        std::cerr << "expected earliest due job to be claimed first" << std::endl;
        return false;
    }

    auto second = service.claim_next_submitted();
    if (!second || second->id != later_id) {
        std::cerr << "expected later due job to be claimed second" << std::endl;
        return false;
    }

    auto third = service.claim_next_submitted();
    if (!third || third->id != none_id) {
        std::cerr << "expected unscheduled job to be claimed last" << std::endl;
        return false;
    }

    return true;
}

bool test_scheduling_policy_escalation() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    const auto now = ppp::core::Clock::now();
    ppp::core::JobPayload payload{.source_path = "policy.tif"};

    const auto far_id = service.create_job(payload, std::nullopt, 5, now + std::chrono::minutes(90));
    const auto near_id = service.create_job(payload, std::nullopt, 0, now + std::chrono::minutes(20));
    const auto imminent_id = service.create_job(payload, std::nullopt, 0, now + std::chrono::minutes(3));
    const auto overdue_id = service.create_job(payload, std::nullopt, 0, now - std::chrono::minutes(1));
    const auto high_id = service.create_job(payload, std::nullopt, 80, now + std::chrono::minutes(2));

    ppp::core::SchedulingPolicy policy;
    policy.escalations.push_back(ppp::core::SchedulingEscalation{std::chrono::minutes(30), 25});
    policy.escalations.push_back(ppp::core::SchedulingEscalation{std::chrono::minutes(5), 75});
    policy.overdue_priority = 100;

    const auto updated = service.apply_scheduling_policy(policy, now);
    if (updated != 3) {
        std::cerr << "expected three jobs to be escalated but saw " << updated << std::endl;
        return false;
    }

    const auto far = service.get(far_id);
    const auto near = service.get(near_id);
    const auto imminent = service.get(imminent_id);
    const auto overdue = service.get(overdue_id);
    const auto high = service.get(high_id);

    if (!far || far->priority != 5) {
        std::cerr << "far job priority changed unexpectedly" << std::endl;
        return false;
    }
    if (!near || near->priority != 25) {
        std::cerr << "near job priority did not escalate to 25" << std::endl;
        return false;
    }
    if (!imminent || imminent->priority != 75) {
        std::cerr << "imminent job priority did not escalate to 75" << std::endl;
        return false;
    }
    if (!overdue || overdue->priority != 100) {
        std::cerr << "overdue job priority did not escalate to 100" << std::endl;
        return false;
    }
    if (!high || high->priority != 80) {
        std::cerr << "pre-escalated job priority should remain highest" << std::endl;
        return false;
    }

    return true;
}

#if PPP_CORE_HAVE_SQLITE
bool test_sqlite_repository_persistence() {
    TempRepository temp;
    const auto db_path = temp.path / "jobs.db";
    std::string first_id;

    {
        ppp::core::SqliteJobRepository repository{db_path};
        ppp::core::JobService service{repository};

        ppp::core::JobPayload payload{.source_path = "input-sqlite.tif"};
        payload.profile_name = "default";
        payload.attachments = {"cover.pdf", "notes.txt"};

        first_id = service.create_job(payload, "corr-123");
        service.mark_validating(first_id);
        service.mark_failed(first_id, "checksum mismatch");

        auto record = service.get(first_id);
        if (!record) {
            std::cerr << "failed to fetch sqlite job after creation" << std::endl;
            return false;
        }
        if (record->attempt_count != 0) {
            std::cerr << "expected sqlite attempt_count to start at zero" << std::endl;
            return false;
        }
        if (record->priority != 0) {
            std::cerr << "expected sqlite priority to start at zero" << std::endl;
            return false;
        }
        if (record->last_attempt_at) {
            std::cerr << "sqlite last_attempt_at should be empty before claims" << std::endl;
            return false;
        }
        if (record->state != ppp::core::JobState::Exception) {
            std::cerr << "expected sqlite job state to be exception" << std::endl;
            return false;
        }
        if (!record->error_message || *record->error_message != "checksum mismatch") {
            std::cerr << "expected sqlite error message to persist" << std::endl;
            return false;
        }
        if (!record->payload.profile_name || *record->payload.profile_name != "default") {
            std::cerr << "expected sqlite profile name" << std::endl;
            return false;
        }
        if (record->payload.attachments.size() != 2) {
            std::cerr << "unexpected sqlite attachment count" << std::endl;
            return false;
        }
        if (record->due_at) {
            std::cerr << "sqlite due_at should be empty before scheduling" << std::endl;
            return false;
        }
    }

    ppp::core::SqliteJobRepository reopened{db_path};
    ppp::core::JobService reopened_service{reopened};

    auto persisted = reopened_service.get(first_id);
    if (!persisted) {
        std::cerr << "failed to reload sqlite job" << std::endl;
        return false;
    }
    if (persisted->attempt_count != 0) {
        std::cerr << "sqlite attempt_count should persist" << std::endl;
        return false;
    }
    if (persisted->priority != 0) {
        std::cerr << "sqlite priority should persist" << std::endl;
        return false;
    }
    if (persisted->last_attempt_at) {
        std::cerr << "sqlite last_attempt_at should remain empty before claims" << std::endl;
        return false;
    }
    if (!persisted->error_message || *persisted->error_message != "checksum mismatch") {
        std::cerr << "sqlite error message missing after reload" << std::endl;
        return false;
    }
    if (persisted->payload.attachments.size() != 2) {
        std::cerr << "sqlite attachments missing after reload" << std::endl;
        return false;
    }
    if (persisted->due_at) {
        std::cerr << "sqlite due_at should remain empty before scheduling" << std::endl;
        return false;
    }

    (void)reopened_service.update_priority(first_id, 8);
    auto reprioritized = reopened_service.get(first_id);
    if (!reprioritized || reprioritized->priority != 8) {
        std::cerr << "sqlite priority update failed" << std::endl;
        return false;
    }

    ppp::core::JobPayload payload2{.source_path = "second-sqlite.tif"};
    const auto second_id = reopened_service.create_job(payload2);

    auto filtered = reopened_service.list(ppp::core::JobState::Exception);
    if (filtered.empty() || filtered.front().id != first_id) {
        std::cerr << "sqlite state-filtered listing incorrect" << std::endl;
        return false;
    }

    (void)reopened_service.retry(first_id);
    reopened_service.mark_completed(second_id);

    auto jobs = reopened_service.list();
    if (jobs.size() != 2) {
        std::cerr << "unexpected sqlite job count" << std::endl;
        return false;
    }

    auto completed = reopened_service.get(second_id);
    if (!completed || completed->state != ppp::core::JobState::Completed) {
        std::cerr << "sqlite completed job missing" << std::endl;
        return false;
    }

    auto claimed = reopened_service.claim_next_submitted();
    if (!claimed || claimed->id != first_id || claimed->priority != 8) {
        std::cerr << "sqlite priority claim ordering incorrect" << std::endl;
        return false;
    }

    return true;
}

bool test_sqlite_schema_migration() {
    TempRepository temp;
    const auto db_path = temp.path / "jobs-migrate.db";

    {
        sqlite3* db = nullptr;
        if (sqlite3_open(db_path.string().c_str(), &db) != SQLITE_OK) {
            std::cerr << "failed to open sqlite database for migration setup" << std::endl;
            return false;
        }
        auto close_db = [&]() {
            if (db) {
                sqlite3_close(db);
                db = nullptr;
            }
        };

        char* error = nullptr;
        if (sqlite3_exec(db,
                         "CREATE TABLE jobs ("
                         " id TEXT PRIMARY KEY,"
                         " state TEXT NOT NULL,"
                         " created_at INTEGER NOT NULL,"
                         " updated_at INTEGER,"
                         " source_path TEXT NOT NULL,"
                         " profile_name TEXT,"
                         " correlation_id TEXT,"
                         " error_message TEXT"
                         ");",
                         nullptr, nullptr, &error) != SQLITE_OK) {
            std::cerr << "failed to create legacy jobs table: " << (error ? error : "unknown") << std::endl;
            sqlite3_free(error);
            close_db();
            return false;
        }
        if (sqlite3_exec(db,
                         "CREATE TABLE attachments ("
                         " job_id TEXT NOT NULL,"
                         " idx INTEGER NOT NULL,"
                         " path TEXT NOT NULL,"
                         " PRIMARY KEY(job_id, idx)"
                         ");",
                         nullptr, nullptr, &error) != SQLITE_OK) {
            std::cerr << "failed to create legacy attachments table: " << (error ? error : "unknown")
                      << std::endl;
            sqlite3_free(error);
            close_db();
            return false;
        }
        if (sqlite3_exec(db, "PRAGMA user_version = 1;", nullptr, nullptr, &error) != SQLITE_OK) {
            std::cerr << "failed to set legacy user_version: " << (error ? error : "unknown") << std::endl;
            sqlite3_free(error);
            close_db();
            return false;
        }
        close_db();
    }

    try {
        ppp::core::SqliteJobRepository::initialize_schema(db_path);
        ppp::core::SqliteJobRepository::initialize_schema(db_path);
    } catch (const std::exception& ex) {
        std::cerr << "schema migration threw: " << ex.what() << std::endl;
        return false;
    }

    sqlite3* verify = nullptr;
    if (sqlite3_open(db_path.string().c_str(), &verify) != SQLITE_OK) {
        std::cerr << "failed to reopen sqlite database for verification" << std::endl;
        return false;
    }

    auto close_verify = [&]() {
        if (verify) {
            sqlite3_close(verify);
            verify = nullptr;
        }
    };

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(verify, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "failed to query user_version after migration" << std::endl;
        close_verify();
        return false;
    }
    bool version_ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version_ok = sqlite3_column_int(stmt, 0) == ppp::core::SqliteJobRepository::latest_schema_version();
    }
    sqlite3_finalize(stmt);
    if (!version_ok) {
        std::cerr << "unexpected user_version after migration" << std::endl;
        close_verify();
        return false;
    }

    if (sqlite3_prepare_v2(verify, "PRAGMA index_list('jobs');", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "failed to enumerate job indexes" << std::endl;
        close_verify();
        return false;
    }
    bool index_found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (text && std::string{text} == "idx_jobs_state_priority_due_created") {
            index_found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (!index_found) {
        std::cerr << "expected priority index after migration" << std::endl;
        close_verify();
        return false;
    }

    if (sqlite3_prepare_v2(verify, "PRAGMA table_info('jobs');", -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "failed to inspect jobs table after migration" << std::endl;
        close_verify();
        return false;
    }
    bool attempt_column_found = false;
    bool last_attempt_column_found = false;
    bool priority_column_found = false;
    bool due_column_found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* column_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if (!column_name) {
            continue;
        }
        const std::string name{column_name};
        if (name == "attempt_count") {
            attempt_column_found = true;
        }
        if (name == "last_attempt_at") {
            last_attempt_column_found = true;
        }
        if (name == "priority") {
            priority_column_found = true;
        }
        if (name == "due_at") {
            due_column_found = true;
        }
    }
    sqlite3_finalize(stmt);
    if (!attempt_column_found) {
        std::cerr << "expected attempt_count column after migration" << std::endl;
        close_verify();
        return false;
    }
    if (!last_attempt_column_found) {
        std::cerr << "expected last_attempt_at column after migration" << std::endl;
        close_verify();
        return false;
    }
    if (!priority_column_found) {
        std::cerr << "expected priority column after migration" << std::endl;
        close_verify();
        return false;
    }
    if (!due_column_found) {
        std::cerr << "expected due_at column after migration" << std::endl;
        close_verify();
        return false;
    }

    close_verify();

    try {
        ppp::core::SqliteJobRepository repository{db_path};
        ppp::core::JobService service{repository};
        auto jobs = service.list();
        if (!jobs.empty()) {
            std::cerr << "expected empty job list after migration" << std::endl;
            return false;
        }
    } catch (const std::exception& ex) {
        std::cerr << "failed to open repository after migration: " << ex.what() << std::endl;
        return false;
    }

    return true;
}
#endif

bool test_job_processor_completes_job() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    bool handler_called = false;
    ppp::core::JobProcessor processor{
        service, [&handler_called](const ppp::core::JobRecord& record) {
            handler_called = true;
            if (record.state != ppp::core::JobState::Rendering) {
                throw std::runtime_error("processor saw unexpected state");
            }
            return ppp::core::JobExecutionResult::completed();
        }};

    ppp::core::JobPayload payload{.source_path = "processor-input.tif"};
    const auto id = service.create_job(payload);

    auto processed = processor.process_next();
    if (!processed) {
        std::cerr << "processor failed to claim a job" << std::endl;
        return false;
    }
    if (processed->id != id) {
        std::cerr << "processor returned unexpected job id" << std::endl;
        return false;
    }
    if (processed->result.outcome != ppp::core::JobExecutionResult::Outcome::Completed) {
        std::cerr << "processor should have completed job" << std::endl;
        return false;
    }
    if (!handler_called) {
        std::cerr << "processor handler was not invoked" << std::endl;
        return false;
    }

    auto stored = service.get(id);
    if (!stored || stored->state != ppp::core::JobState::Completed) {
        std::cerr << "job state not marked completed" << std::endl;
        return false;
    }
    if (stored->attempt_count != 1) {
        std::cerr << "completed job attempt_count not incremented" << std::endl;
        return false;
    }
    if (!stored->last_attempt_at) {
        std::cerr << "completed job missing last_attempt_at" << std::endl;
        return false;
    }

    return true;
}

bool test_job_processor_handles_failure_result() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    const auto id = service.create_job(ppp::core::JobPayload{.source_path = "processor-failure.tif"});

    ppp::core::JobProcessor processor{
        service, [](const ppp::core::JobRecord&) {
            return ppp::core::JobExecutionResult::failed("failed during render");
        }};

    auto processed = processor.process_next();
    if (!processed) {
        std::cerr << "processor failed to claim job for failure scenario" << std::endl;
        return false;
    }
    if (processed->result.outcome != ppp::core::JobExecutionResult::Outcome::Failed) {
        std::cerr << "processor outcome mismatch for failure" << std::endl;
        return false;
    }
    if (!processed->result.message || *processed->result.message != "failed during render") {
        std::cerr << "processor did not propagate failure message" << std::endl;
        return false;
    }

    auto stored = service.get(id);
    if (!stored || stored->state != ppp::core::JobState::Exception) {
        std::cerr << "repository did not capture exception state" << std::endl;
        return false;
    }
    if (stored->attempt_count != 1) {
        std::cerr << "failed job attempt_count not incremented" << std::endl;
        return false;
    }
    if (!stored->last_attempt_at) {
        std::cerr << "failed job missing last_attempt_at" << std::endl;
        return false;
    }
    if (!stored->error_message || *stored->error_message != "failed during render") {
        std::cerr << "repository did not persist failure reason" << std::endl;
        return false;
    }

    return true;
}

bool test_job_processor_catches_exception() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    const auto id = service.create_job(ppp::core::JobPayload{.source_path = "processor-exception.tif"});

    ppp::core::JobProcessor processor{
        service, [](const ppp::core::JobRecord&) -> ppp::core::JobExecutionResult {
            throw std::runtime_error("unexpected handler error");
        }};

    auto processed = processor.process_next();
    if (!processed) {
        std::cerr << "processor failed to claim job for exception scenario" << std::endl;
        return false;
    }
    if (processed->result.outcome != ppp::core::JobExecutionResult::Outcome::Failed) {
        std::cerr << "exception should be reported as failure" << std::endl;
        return false;
    }
    if (!processed->result.message || *processed->result.message != "unexpected handler error") {
        std::cerr << "exception message not propagated" << std::endl;
        return false;
    }

    auto stored = service.get(id);
    if (!stored || stored->state != ppp::core::JobState::Exception) {
        std::cerr << "job state not set to exception after handler throw" << std::endl;
        return false;
    }
    if (stored->attempt_count != 1) {
        std::cerr << "exception job attempt_count not incremented" << std::endl;
        return false;
    }
    if (!stored->last_attempt_at) {
        std::cerr << "exception job missing last_attempt_at" << std::endl;
        return false;
    }
    if (!stored->error_message || *stored->error_message != "unexpected handler error") {
        std::cerr << "repository did not persist exception reason" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_claims_submitted_jobs() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    std::vector<ppp::core::JobEvent> events;
    service.on_event([&events](const ppp::core::JobEvent& event) { events.push_back(event); });

    const auto first_id = service.create_job(ppp::core::JobPayload{.source_path = "first-claim.tif"});
    const auto second_id = service.create_job(ppp::core::JobPayload{.source_path = "second-claim.tif"});

    auto claimed_first = service.claim_next_submitted();
    if (!claimed_first) {
        std::cerr << "failed to claim first submitted job" << std::endl;
        return false;
    }
    if (claimed_first->state != ppp::core::JobState::Validating) {
        std::cerr << "claimed job not transitioned to validating" << std::endl;
        return false;
    }
    if (claimed_first->attempt_count != 1) {
        std::cerr << "expected attempt_count to increment on first claim" << std::endl;
        return false;
    }
    if (!claimed_first->last_attempt_at) {
        std::cerr << "expected last_attempt_at to be populated after claim" << std::endl;
        return false;
    }
    const auto first_attempt_time = *claimed_first->last_attempt_at;
    auto stored_first = service.get(claimed_first->id);
    if (!stored_first || stored_first->state != ppp::core::JobState::Validating) {
        std::cerr << "repository did not persist validating state" << std::endl;
        return false;
    }
    if (!stored_first || stored_first->attempt_count != 1) {
        std::cerr << "stored attempt_count mismatch after first claim" << std::endl;
        return false;
    }
    if (!stored_first->last_attempt_at) {
        std::cerr << "stored last_attempt_at missing after claim" << std::endl;
        return false;
    }
    if (*stored_first->last_attempt_at != first_attempt_time) {
        std::cerr << "stored last_attempt_at does not match claim timestamp" << std::endl;
        return false;
    }

    auto claimed_second = service.claim_next_submitted();
    if (!claimed_second) {
        std::cerr << "failed to claim second submitted job" << std::endl;
        return false;
    }
    if (claimed_second->attempt_count != 1) {
        std::cerr << "expected attempt_count to increment on second claim" << std::endl;
        return false;
    }
    if (!claimed_second->last_attempt_at) {
        std::cerr << "expected second claim to populate last_attempt_at" << std::endl;
        return false;
    }

    std::vector<std::string> claimed_ids = {claimed_first->id, claimed_second->id};
    std::sort(claimed_ids.begin(), claimed_ids.end());
    std::vector<std::string> expected_ids = {first_id, second_id};
    std::sort(expected_ids.begin(), expected_ids.end());
    if (claimed_ids != expected_ids) {
        std::cerr << "claims did not cover all submitted jobs" << std::endl;
        return false;
    }

    if (service.claim_next_submitted()) {
        std::cerr << "claim should return nullopt when queue exhausted" << std::endl;
        return false;
    }

    service.mark_failed(first_id, "previous failure");
    (void)service.retry(first_id, false, "manual retry");

    auto claimed_retry = service.claim_next_submitted();
    if (!claimed_retry || claimed_retry->id != first_id) {
        std::cerr << "failed to claim resubmitted job" << std::endl;
        return false;
    }
    if (claimed_retry->state != ppp::core::JobState::Validating) {
        std::cerr << "resubmitted job not transitioned to validating" << std::endl;
        return false;
    }
    if (claimed_retry->attempt_count != 2) {
        std::cerr << "expected attempt_count to increment on retry" << std::endl;
        return false;
    }
    if (!claimed_retry->last_attempt_at) {
        std::cerr << "expected retry claim to populate last_attempt_at" << std::endl;
        return false;
    }
    if (*claimed_retry->last_attempt_at < first_attempt_time) {
        std::cerr << "retry claim last_attempt_at should not regress" << std::endl;
        return false;
    }
    if (claimed_retry->error_message) {
        std::cerr << "claim should clear stale error message" << std::endl;
        return false;
    }

    auto stored_retry = service.get(first_id);
    if (!stored_retry || stored_retry->attempt_count != 2) {
        std::cerr << "stored attempt_count mismatch after retry" << std::endl;
        return false;
    }
    if (stored_retry->error_message) {
        std::cerr << "stored error message should be cleared after retry claim" << std::endl;
        return false;
    }
    if (!stored_retry->last_attempt_at) {
        std::cerr << "stored retry last_attempt_at missing" << std::endl;
        return false;
    }
    if (*stored_retry->last_attempt_at < first_attempt_time) {
        std::cerr << "stored retry last_attempt_at should not regress" << std::endl;
        return false;
    }
    if (*stored_retry->last_attempt_at != *claimed_retry->last_attempt_at) {
        std::cerr << "stored retry last_attempt_at does not match claimed timestamp" << std::endl;
        return false;
    }

    bool saw_validating_event = false;
    bool saw_retry_event = false;
    for (const auto& event : events) {
        if (event.job_id == first_id && event.state == ppp::core::JobState::Validating) {
            saw_validating_event = true;
        }
        if (event.job_id == first_id && event.state == ppp::core::JobState::Submitted && event.message &&
            *event.message == "manual retry") {
            saw_retry_event = true;
        }
    }
    if (!saw_validating_event) {
        std::cerr << "expected validating event for claimed job" << std::endl;
        return false;
    }
    if (!saw_retry_event) {
        std::cerr << "expected retry event with note" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_retry_resubmits() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    std::vector<ppp::core::JobEvent> events;
    service.on_event([&events](const ppp::core::JobEvent& event) { events.push_back(event); });

    const auto id = service.create_job(ppp::core::JobPayload{.source_path = "retry-input.tif"});
    service.mark_failed(id, "initial failure");

    (void)service.retry(id);
    auto queued = service.get(id);
    if (!queued || queued->state != ppp::core::JobState::Submitted) {
        std::cerr << "retry did not resubmit job" << std::endl;
        return false;
    }
    if (queued->error_message) {
        std::cerr << "retry should clear error by default" << std::endl;
        return false;
    }
    if (queued->last_attempt_at) {
        std::cerr << "retry should not set last_attempt_at before a claim" << std::endl;
        return false;
    }

    auto first_claim = service.claim_next_submitted();
    if (!first_claim || first_claim->attempt_count != 1) {
        std::cerr << "first claim after retry missing or wrong attempt_count" << std::endl;
        return false;
    }
    if (!first_claim->last_attempt_at) {
        std::cerr << "first claim after retry missing last_attempt_at" << std::endl;
        return false;
    }

    service.mark_failed(id, "second failure");
    (void)service.retry(id, false, "second chance");

    auto second_ready = service.get(id);
    if (!second_ready || second_ready->state != ppp::core::JobState::Submitted) {
        std::cerr << "retry with keep-error did not submit job" << std::endl;
        return false;
    }
    if (!second_ready->error_message || *second_ready->error_message != "second failure") {
        std::cerr << "retry with keep-error should preserve error message" << std::endl;
        return false;
    }
    if (!second_ready->last_attempt_at) {
        std::cerr << "retry with keep-error should preserve last_attempt_at" << std::endl;
        return false;
    }

    auto second_claim = service.claim_next_submitted();
    if (!second_claim || second_claim->attempt_count != 2) {
        std::cerr << "second claim attempt_count mismatch" << std::endl;
        return false;
    }
    if (!second_claim->last_attempt_at) {
        std::cerr << "second claim missing last_attempt_at" << std::endl;
        return false;
    }
    if (second_claim->error_message) {
        std::cerr << "second claim should clear preserved error" << std::endl;
        return false;
    }

    bool saw_second_note = false;
    for (const auto& event : events) {
        if (event.job_id == id && event.state == ppp::core::JobState::Submitted && event.message &&
            *event.message == "second chance") {
            saw_second_note = true;
        }
    }
    if (!saw_second_note) {
        std::cerr << "expected retry event note to be emitted" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_attachment_mutation() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    std::vector<ppp::core::JobEvent> events;
    service.on_event([&events](const ppp::core::JobEvent& event) { events.push_back(event); });

    ppp::core::JobPayload payload{.source_path = "attachments-input.tif"};
    payload.attachments = {"initial.pdf"};
    const auto id = service.create_job(std::move(payload));

    auto initial_record = service.get(id);
    if (!initial_record || initial_record->payload.attachments.size() != 1) {
        std::cerr << "expected job to start with one attachment" << std::endl;
        return false;
    }
    const auto initial_updated = initial_record->updated_at;

    (void)service.add_attachment(id, "note.txt");
    (void)service.add_attachment(id, "initial.pdf");

    auto with_additions = service.get(id);
    if (!with_additions || with_additions->payload.attachments.size() != 2) {
        std::cerr << "expected job to contain two distinct attachments" << std::endl;
        return false;
    }
    if (std::find(with_additions->payload.attachments.begin(), with_additions->payload.attachments.end(), "note.txt") ==
        with_additions->payload.attachments.end()) {
        std::cerr << "missing newly added attachment" << std::endl;
        return false;
    }
    if (std::find(with_additions->payload.attachments.begin(), with_additions->payload.attachments.end(), "initial.pdf") ==
        with_additions->payload.attachments.end()) {
        std::cerr << "original attachment lost after mutation" << std::endl;
        return false;
    }
    if (!with_additions->updated_at || (initial_updated && *with_additions->updated_at == *initial_updated)) {
        std::cerr << "attachment addition should update updated_at" << std::endl;
        return false;
    }

    (void)service.remove_attachment(id, "initial.pdf");
    (void)service.remove_attachment(id, "nonexistent.txt");

    auto after_removal = service.get(id);
    if (!after_removal || after_removal->payload.attachments.size() != 1) {
        std::cerr << "expected removal to leave one attachment" << std::endl;
        return false;
    }
    if (after_removal->payload.attachments.front() != "note.txt") {
        std::cerr << "unexpected attachment remains after removal" << std::endl;
        return false;
    }

    (void)service.clear_attachments(id);

    auto cleared = service.get(id);
    if (!cleared || !cleared->payload.attachments.empty()) {
        std::cerr << "clear_attachments should remove all attachments" << std::endl;
        return false;
    }

    (void)service.clear_attachments(id);

    bool saw_added = false;
    bool saw_removed = false;
    bool saw_cleared = false;
    for (const auto& event : events) {
        if (event.job_id != id || !event.message) {
            continue;
        }
        if (*event.message == "attachment added: note.txt") {
            saw_added = true;
        }
        if (*event.message == "attachment removed: initial.pdf") {
            saw_removed = true;
        }
        if (*event.message == "attachments cleared") {
            saw_cleared = true;
        }
    }

    if (!saw_added || !saw_removed || !saw_cleared) {
        std::cerr << "attachment mutation events missing" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_tag_mutation() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    std::vector<ppp::core::JobEvent> events;
    service.on_event([&events](const ppp::core::JobEvent& event) { events.push_back(event); });

    ppp::core::JobPayload payload{.source_path = "tagged-input.tif"};
    payload.tags = {"ingest"};
    const auto id = service.create_job(std::move(payload));

    auto initial = service.get(id);
    if (!initial || initial->payload.tags.size() != 1 || initial->payload.tags.front() != "ingest") {
        std::cerr << "expected job to start with an ingest tag" << std::endl;
        return false;
    }
    const auto initial_updated = initial->updated_at;

    (void)service.add_tag(id, "urgent");
    (void)service.add_tag(id, "ingest");

    auto after_add = service.get(id);
    if (!after_add || after_add->payload.tags.size() != 2) {
        std::cerr << "expected job to contain two distinct tags" << std::endl;
        return false;
    }
    if (std::find(after_add->payload.tags.begin(), after_add->payload.tags.end(), "urgent") ==
        after_add->payload.tags.end()) {
        std::cerr << "missing newly added tag" << std::endl;
        return false;
    }
    if (!after_add->updated_at || (initial_updated && *after_add->updated_at == *initial_updated)) {
        std::cerr << "tag addition should update updated_at" << std::endl;
        return false;
    }

    (void)service.remove_tag(id, "ingest");
    (void)service.remove_tag(id, "nonexistent");

    auto after_remove = service.get(id);
    if (!after_remove || after_remove->payload.tags.size() != 1) {
        std::cerr << "expected tag removal to leave one tag" << std::endl;
        return false;
    }
    if (after_remove->payload.tags.front() != "urgent") {
        std::cerr << "unexpected tag remains after removal" << std::endl;
        return false;
    }

    (void)service.clear_tags(id);

    auto cleared = service.get(id);
    if (!cleared || !cleared->payload.tags.empty()) {
        std::cerr << "clear_tags should remove all tags" << std::endl;
        return false;
    }

    (void)service.clear_tags(id);

    bool saw_added = false;
    bool saw_removed = false;
    bool saw_cleared = false;
    for (const auto& event : events) {
        if (event.job_id != id || !event.message) {
            continue;
        }
        if (*event.message == "tag added: urgent") {
            saw_added = true;
        }
        if (*event.message == "tag removed: ingest") {
            saw_removed = true;
        }
        if (*event.message == "tags cleared") {
            saw_cleared = true;
        }
    }

    if (!saw_added || !saw_removed || !saw_cleared) {
        std::cerr << "tag mutation events missing" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_tag_filtering() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    ppp::core::JobPayload first_payload{.source_path = "first.tif"};
    first_payload.tags = {"alpha", "beta"};
    const auto first_id = service.create_job(std::move(first_payload), std::string{"batch-1"});

    ppp::core::JobPayload second_payload{.source_path = "second.tif"};
    second_payload.tags = {"alpha"};
    const auto second_id = service.create_job(std::move(second_payload), std::string{"batch-1"});

    ppp::core::JobPayload third_payload{.source_path = "third.tif"};
    third_payload.tags = {"gamma"};
    const auto third_id = service.create_job(std::move(third_payload), std::string{"batch-2"});

    service.mark_completed(second_id);

    const auto contains_id = [](const std::vector<ppp::core::JobRecord>& records,
                                const std::string& id) {
        return std::any_of(records.begin(), records.end(),
                           [&id](const ppp::core::JobRecord& record) { return record.id == id; });
    };

    auto alpha_records = service.list_with_tags(std::nullopt, {"alpha"});
    if (alpha_records.size() != 2 || !contains_id(alpha_records, first_id) ||
        !contains_id(alpha_records, second_id)) {
        std::cerr << "expected alpha filter to return first and second jobs" << std::endl;
        return false;
    }

    auto duplicate_filter = service.list_with_tags(std::nullopt, {"alpha", "alpha"});
    if (duplicate_filter.size() != alpha_records.size()) {
        std::cerr << "duplicate tag filters should be coalesced" << std::endl;
        return false;
    }

    auto beta_records = service.list_with_tags(std::nullopt, {"beta"});
    if (beta_records.size() != 1 || !contains_id(beta_records, first_id)) {
        std::cerr << "expected beta filter to return only the first job" << std::endl;
        return false;
    }

    auto alpha_beta_records = service.list_with_tags(std::nullopt, {"alpha", "beta"});
    if (alpha_beta_records.size() != 1 || !contains_id(alpha_beta_records, first_id)) {
        std::cerr << "expected combined filter to return only the first job" << std::endl;
        return false;
    }

    auto completed_alpha = service.list_with_tags(ppp::core::JobState::Completed, {"alpha"});
    if (completed_alpha.size() != 1 || completed_alpha.front().id != second_id) {
        std::cerr << "expected state-aware filter to return completed alpha job" << std::endl;
        return false;
    }

    auto gamma_records = service.list_with_tags(std::nullopt, {"gamma"});
    if (gamma_records.size() != 1 || gamma_records.front().id != third_id) {
        std::cerr << "expected gamma filter to return only the third job" << std::endl;
        return false;
    }

    auto unmatched = service.list_with_tags(std::nullopt, {"delta"});
    if (!unmatched.empty()) {
        std::cerr << "unexpected jobs returned for unmatched filter" << std::endl;
        return false;
    }

    auto batch_one_records =
        service.list_with_tags(std::nullopt, std::vector<std::string>{}, std::string{"batch-1"});
    if (batch_one_records.size() != 2 || !contains_id(batch_one_records, first_id) ||
        !contains_id(batch_one_records, second_id)) {
        std::cerr << "expected correlation filter to return both batch-1 jobs" << std::endl;
        return false;
    }

    auto batch_one_beta = service.list_with_tags(std::nullopt, {"beta"}, std::string{"batch-1"});
    if (batch_one_beta.size() != 1 || batch_one_beta.front().id != first_id) {
        std::cerr << "expected combined tag and correlation filter to return first job" << std::endl;
        return false;
    }

    auto empty_batch =
        service.list_with_tags(std::nullopt, std::vector<std::string>{}, std::string{"batch-3"});
    if (!empty_batch.empty()) {
        std::cerr << "unexpected jobs returned for unmatched correlation filter" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_correlation_updates() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    std::vector<ppp::core::JobEvent> events;
    service.on_event([&events](const ppp::core::JobEvent& event) { events.push_back(event); });

    const auto id = service.create_job(ppp::core::JobPayload{.source_path = "corr-input.tif"});

    auto initial = service.get(id);
    if (!initial || initial->correlation_id) {
        std::cerr << "expected job to start without a correlation id" << std::endl;
        return false;
    }
    const auto initial_updated = initial->updated_at;

    (void)service.update_correlation(id, std::string{"batch-42"});

    auto after_set = service.get(id);
    if (!after_set || !after_set->correlation_id || *after_set->correlation_id != "batch-42") {
        std::cerr << "expected correlation id to be applied" << std::endl;
        return false;
    }
    if (!after_set->updated_at || (initial_updated && *after_set->updated_at == *initial_updated)) {
        std::cerr << "correlation update should refresh updated_at" << std::endl;
        return false;
    }

    const auto events_after_set = events.size();
    (void)service.update_correlation(id, std::string{"batch-42"});
    if (events.size() != events_after_set) {
        std::cerr << "expected redundant correlation update to avoid emitting events" << std::endl;
        return false;
    }

    (void)service.update_correlation(id, std::nullopt);

    auto after_clear = service.get(id);
    if (!after_clear || after_clear->correlation_id) {
        std::cerr << "expected correlation id to be cleared" << std::endl;
        return false;
    }
    if (!after_clear->updated_at || (after_set->updated_at && *after_clear->updated_at == *after_set->updated_at)) {
        std::cerr << "clearing correlation should refresh updated_at" << std::endl;
        return false;
    }

    bool saw_set = false;
    bool saw_cleared = false;
    for (const auto& event : events) {
        if (event.job_id != id || !event.message) {
            continue;
        }
        if (*event.message == "correlation set to batch-42") {
            saw_set = true;
        }
        if (*event.message == "correlation cleared") {
            saw_cleared = true;
        }
    }

    if (!saw_set || !saw_cleared) {
        std::cerr << "correlation events missing" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_resume_active_jobs() {
    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    std::vector<ppp::core::JobEvent> events;
    service.on_event([&events](const ppp::core::JobEvent& event) { events.push_back(event); });

    const auto first_id = service.create_job(ppp::core::JobPayload{.source_path = "resume-1.tif"});
    const auto second_id = service.create_job(ppp::core::JobPayload{.source_path = "resume-2.tif"});
    const auto untouched_id = service.create_job(ppp::core::JobPayload{.source_path = "resume-3.tif"});

    auto rendering_claim = service.claim_next_submitted(ppp::core::JobState::Rendering);
    if (!rendering_claim || rendering_claim->id != first_id) {
        std::cerr << "expected to claim first job for rendering" << std::endl;
        return false;
    }
    auto validating_claim = service.claim_next_submitted();
    if (!validating_claim || validating_claim->id != second_id) {
        std::cerr << "expected to claim second job for validating" << std::endl;
        return false;
    }

    const auto events_before_resume = events.size();
    const auto resumed = service.resume_active_jobs();
    if (resumed != 2) {
        std::cerr << "resume_active_jobs should transition both active jobs" << std::endl;
        return false;
    }

    auto rendering_record = service.get(first_id);
    if (!rendering_record || rendering_record->state != ppp::core::JobState::Submitted) {
        std::cerr << "rendering job was not returned to submitted state" << std::endl;
        return false;
    }
    if (rendering_record->attempt_count != rendering_claim->attempt_count) {
        std::cerr << "resume should not change attempt_count for rendering job" << std::endl;
        return false;
    }

    auto validating_record = service.get(second_id);
    if (!validating_record || validating_record->state != ppp::core::JobState::Submitted) {
        std::cerr << "validating job was not returned to submitted state" << std::endl;
        return false;
    }
    if (validating_record->attempt_count != validating_claim->attempt_count) {
        std::cerr << "resume should not change attempt_count for validating job" << std::endl;
        return false;
    }

    auto untouched_record = service.get(untouched_id);
    if (!untouched_record || untouched_record->state != ppp::core::JobState::Submitted) {
        std::cerr << "resume should not affect untouched submitted job" << std::endl;
        return false;
    }

    if (events.size() != events_before_resume + resumed) {
        std::cerr << "expected resume to emit events for transitioned jobs" << std::endl;
        return false;
    }

    const auto resumed_again = service.resume_active_jobs();
    if (resumed_again != 0) {
        std::cerr << "second resume call should not transition additional jobs" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_summary() {
    using namespace std::chrono_literals;

    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    const auto now = ppp::core::Clock::now();
    const auto due_later = now + 30s;
    const auto due_soon = now + 10s;
    const auto due_latest = now + 60s;

    const auto submitted_id = service.create_job(ppp::core::JobPayload{.source_path = "summary-submitted.tif"}, std::nullopt,
                                                1, due_later);
    const auto validating_id = service.create_job(ppp::core::JobPayload{.source_path = "summary-validating.tif"},
                                                 std::nullopt, 2, due_soon);
    service.mark_validating(validating_id);

    const auto rendering_id = service.create_job(ppp::core::JobPayload{.source_path = "summary-rendering.tif"});
    (void)service.update_due_at(rendering_id, due_latest);
    service.mark_rendering(rendering_id);

    const auto exception_id = service.create_job(ppp::core::JobPayload{.source_path = "summary-exception.tif"});
    service.mark_failed(exception_id, "transient failure");

    const auto completed_id = service.create_job(ppp::core::JobPayload{.source_path = "summary-completed.tif"});
    service.mark_completed(completed_id);

    const auto cancelled_id = service.create_job(ppp::core::JobPayload{.source_path = "summary-cancelled.tif"});
    service.mark_cancelled(cancelled_id, "operator request");

    const auto summary = service.summarize();

    if (summary.total != 6 || summary.submitted != 1 || summary.validating != 1 || summary.rendering != 1 ||
        summary.exception != 1 || summary.completed != 1 || summary.cancelled != 1) {
        std::cerr << "summary counts incorrect" << std::endl;
        return false;
    }
    if (summary.outstanding != 4) {
        std::cerr << "unexpected outstanding count" << std::endl;
        return false;
    }

    auto submitted = service.get(submitted_id);
    auto validating = service.get(validating_id);
    auto rendering = service.get(rendering_id);
    auto exception = service.get(exception_id);
    auto completed = service.get(completed_id);
    auto cancelled = service.get(cancelled_id);

    if (!submitted || !validating || !rendering || !exception || !completed || !cancelled) {
        std::cerr << "failed to reload records for summary validation" << std::endl;
        return false;
    }

    std::vector<ppp::core::JobRecord> outstanding_records = {*submitted, *validating, *rendering, *exception};
    auto expected_oldest_outstanding = outstanding_records.front().created_at;
    for (const auto& record : outstanding_records) {
        if (record.created_at < expected_oldest_outstanding) {
            expected_oldest_outstanding = record.created_at;
        }
    }

    if (!summary.oldest_outstanding || *summary.oldest_outstanding != expected_oldest_outstanding) {
        std::cerr << "oldest outstanding timestamp mismatch" << std::endl;
        return false;
    }

    std::vector<ppp::core::JobRecord> all_records = outstanding_records;
    all_records.push_back(*completed);
    all_records.push_back(*cancelled);

    auto expected_oldest_created = all_records.front().created_at;
    for (const auto& record : all_records) {
        if (record.created_at < expected_oldest_created) {
            expected_oldest_created = record.created_at;
        }
    }

    if (!summary.oldest_created || *summary.oldest_created != expected_oldest_created) {
        std::cerr << "oldest created timestamp mismatch" << std::endl;
        return false;
    }

    std::optional<ppp::core::TimePoint> expected_next_due;
    for (const auto& record : outstanding_records) {
        if (record.due_at) {
            if (!expected_next_due || *record.due_at < *expected_next_due) {
                expected_next_due = record.due_at;
            }
        }
    }

    if (expected_next_due) {
        if (!summary.next_due || *summary.next_due != *expected_next_due) {
            std::cerr << "next due timestamp mismatch" << std::endl;
            return false;
        }
    } else if (summary.next_due) {
        std::cerr << "summary reported next due when none expected" << std::endl;
        return false;
    }

    std::optional<ppp::core::TimePoint> expected_latest_activity;
    for (const auto& record : all_records) {
        const auto activity = record.updated_at.value_or(record.created_at);
        if (!expected_latest_activity || activity > *expected_latest_activity) {
            expected_latest_activity = activity;
        }
    }

    if (!summary.latest_update || *summary.latest_update != *expected_latest_activity) {
        std::cerr << "latest activity timestamp mismatch" << std::endl;
        return false;
    }

    return true;
}

bool test_job_service_purge_in_memory() {
    using namespace std::chrono_literals;

    ppp::core::InMemoryJobRepository repository;
    ppp::core::JobService service{repository};

    const auto retain_id = service.create_job(ppp::core::JobPayload{.source_path = "retain.tif"});
    const auto purge_id = service.create_job(ppp::core::JobPayload{.source_path = "purge.tif"});

    service.mark_completed(purge_id);
    auto stale_record = service.get(purge_id);
    if (!stale_record || !stale_record->updated_at) {
        std::cerr << "expected purge candidate to have updated_at" << std::endl;
        return false;
    }

    std::this_thread::sleep_for(2ms);

    service.mark_completed(retain_id);
    auto retained = service.get(retain_id);
    if (!retained || !retained->updated_at) {
        std::cerr << "expected retained job to have updated_at" << std::endl;
        return false;
    }

    const auto cutoff = *stale_record->updated_at + 1us;
    const auto removed = service.purge(ppp::core::JobState::Completed, cutoff);
    if (removed != 1) {
        std::cerr << "expected exactly one job to be purged" << std::endl;
        return false;
    }

    if (service.get(purge_id)) {
        std::cerr << "purged job should not be retrievable" << std::endl;
        return false;
    }

    auto still_there = service.get(retain_id);
    if (!still_there) {
        std::cerr << "retained job missing after purge" << std::endl;
        return false;
    }
    if (still_there->state != ppp::core::JobState::Completed) {
        std::cerr << "retained job state changed unexpectedly" << std::endl;
        return false;
    }

    return true;
}

bool test_job_summary_json() {
    using namespace std::chrono_literals;

    ppp::core::JobSummary summary;
    summary.total = 5;
    summary.submitted = 1;
    summary.validating = 2;
    summary.rendering = 0;
    summary.exception = 1;
    summary.completed = 1;
    summary.cancelled = 0;
    summary.outstanding = 4;
    summary.oldest_created = ppp::core::TimePoint{std::chrono::seconds{1700000000}};
    summary.oldest_outstanding = ppp::core::TimePoint{std::chrono::seconds{1700000005}} + 123456us;
    summary.latest_update = ppp::core::TimePoint{std::chrono::seconds{1700000100}};
    summary.next_due = ppp::core::TimePoint{std::chrono::seconds{1700000200}} + 999000us;

    const std::string expected =
        R"JSON({"total":5,"submitted":1,"validating":2,"rendering":0,"exception":1,"completed":1,"cancelled":0,"outstanding":4,"oldest_created":"2023-11-14T22:13:20.000000Z","oldest_outstanding":"2023-11-14T22:13:25.123456Z","latest_update":"2023-11-14T22:15:00.000000Z","next_due":"2023-11-14T22:16:40.999000Z"})JSON";

    const auto json = ppp::core::job_summary_to_json(summary);
    if (json != expected) {
        std::cerr << "unexpected job summary json output\nexpected: " << expected << "\nactual:   " << json << std::endl;
        return false;
    }

    return true;
}

bool test_job_json_serialization() {
    using namespace std::chrono_literals;

    ppp::core::JobRecord record;
    record.id = "job-1\"";
    record.state = ppp::core::JobState::Cancelled;
    record.created_at = ppp::core::TimePoint{std::chrono::seconds{1700000000}} + 123456us;
    record.updated_at = ppp::core::TimePoint{std::chrono::seconds{1700001234}};
    record.correlation_id = "corr\\id";
    record.error_message = "failed \"bad\" input";
    record.priority = 9;
    record.attempt_count = 7;
    record.last_attempt_at = ppp::core::TimePoint{std::chrono::seconds{1700002000}} + 654321us;
    record.due_at = ppp::core::TimePoint{std::chrono::seconds{1700003000}} + 111222us;
    record.payload.source_path = "C:/input/file.tif";
    record.payload.profile_name = "default\nprofile";
    record.payload.attachments = {"note.txt", "cover \"sheet\""};
    record.payload.tags = {"alpha", "beta"};

    const std::string expected =
        R"JSON({"id":"job-1\"","state":"cancelled","created_at":"2023-11-14T22:13:20.123456Z",)JSON";
    const std::string expected_tail =
        R"JSON("updated_at":"2023-11-14T22:33:54.000000Z","correlation_id":"corr\\id",)JSON";
    const std::string expected_tail2 =
        R"JSON("error_message":"failed \"bad\" input","priority":9,"attempt_count":7,"last_attempt_at":"2023-11-14T22:46:40.654321Z","due_at":"2023-11-14T23:03:20.111222Z","payload":{)JSON";
    const std::string expected_payload =
        R"JSON("source_path":"C:/input/file.tif","profile_name":"default\nprofile",)JSON";
    const std::string expected_payload_tail =
        R"JSON("attachments":["note.txt","cover \"sheet\""],"tags":["alpha","beta"]}})JSON";

    const auto json = ppp::core::job_record_to_json(record);
    const auto expected_full = expected + expected_tail + expected_tail2 + expected_payload + expected_payload_tail;
    if (json != expected_full) {
        std::cerr << "unexpected job json output\nexpected: " << expected_full << "\nactual:   " << json << std::endl;
        return false;
    }

    const auto array_json = ppp::core::job_records_to_json_array({record, record});
    const auto expected_array = '[' + expected_full + ',' + expected_full + ']';
    if (array_json != expected_array) {
        std::cerr << "unexpected job array json output\nexpected: " << expected_array
                  << "\nactual:   " << array_json << std::endl;
        return false;
    }

    return true;
}

bool test_job_json_parse_roundtrip() {
    using namespace std::chrono_literals;

    ppp::core::JobRecord original;
    original.id = "parse-job";
    original.state = ppp::core::JobState::Exception;
    original.payload.source_path = "C:/input/image.tif";
    original.payload.profile_name = std::string{"profile-A"};
    original.payload.attachments = {"context.txt", "diagram.png"};
    original.payload.tags = {"alpha", "beta"};
    original.created_at = ppp::core::TimePoint{std::chrono::seconds{1701000000}} + 123456us;
    original.updated_at = original.created_at + 5s;
    original.correlation_id = std::string{"batch-42"};
    original.error_message = std::string{"failure"};
    original.priority = -5;
    original.attempt_count = 3;
    original.last_attempt_at = original.created_at + 6s;
    original.due_at = original.created_at + 1h;

    const auto json = ppp::core::job_record_to_json(original);
    const auto parsed_record = ppp::core::job_record_from_json(json);
    if (!parsed_record) {
        std::cerr << "expected job_record_from_json to succeed" << std::endl;
        return false;
    }
    if (!job_records_equal(*parsed_record, original)) {
        std::cerr << "job_record_from_json altered record contents" << std::endl;
        return false;
    }

    const auto array_json = ppp::core::job_records_to_json_array({original});
    const auto parsed_array = ppp::core::job_records_from_json_array(array_json);
    if (!parsed_array) {
        std::cerr << "expected job_records_from_json_array to succeed" << std::endl;
        return false;
    }
    if (parsed_array->size() != 1 || !job_records_equal(parsed_array->front(), original)) {
        std::cerr << "job_records_from_json_array failed to preserve record" << std::endl;
        return false;
    }

    return true;
}

bool test_job_json_file_export() {
    using namespace std::chrono_literals;

    ppp::core::JobRecord record;
    record.id = "export-job";
    record.created_at = ppp::core::TimePoint{std::chrono::seconds{1700004000}} + 500000us;
    record.updated_at = record.created_at;
    record.payload.source_path = "/data/input.tif";
    record.payload.attachments = {"note.txt"};
    record.payload.tags = {"batch"};

    std::vector<ppp::core::JobRecord> records;
    records.push_back(record);

    TempRepository temp_dir;
    const auto export_path = temp_dir.path / "jobs.json";

    if (!ppp::core::write_job_records_to_json_file(records, export_path)) {
        std::cerr << "expected JSON export helper to write file" << std::endl;
        return false;
    }

    std::ifstream exported{export_path, std::ios::binary};
    if (!exported) {
        std::cerr << "unable to open exported json" << std::endl;
        return false;
    }

    const std::string expected = ppp::core::job_records_to_json_array(records) + '\n';
    const std::string contents{std::istreambuf_iterator<char>{exported}, std::istreambuf_iterator<char>{}};
    if (contents != expected) {
        std::cerr << "unexpected exported json contents\nexpected: " << expected << "\nactual:   " << contents
                  << std::endl;
        return false;
    }

    const auto parsed = ppp::core::read_job_records_from_json_file(export_path);
    if (!parsed) {
        std::cerr << "expected read_job_records_from_json_file to succeed" << std::endl;
        return false;
    }
    if (parsed->size() != records.size() || !job_records_equal(parsed->front(), records.front())) {
        std::cerr << "parsed JSON file did not match original records" << std::endl;
        return false;
    }

    return true;
}

bool test_event_sink_invocation() {
    using namespace ppp::core;
    InMemoryJobRepository repo;
    JobService service{repo};

    std::vector<JobEvent> events;
    service.on_event([&events](const JobEvent& event) { events.push_back(event); });

    const auto id = service.create_job(JobPayload{"test.pdf", std::nullopt, {}, {}});
    if (events.size() != 1 || events.back().job_id != id || events.back().state != JobState::Submitted) {
        std::cerr << "expected create event" << std::endl;
        return false;
    }

    service.mark_validating(id);
    if (events.size() != 2 || events.back().state != JobState::Validating) {
        std::cerr << "expected validating event" << std::endl;
        return false;
    }

    service.mark_completed(id);
    if (events.size() != 3 || events.back().state != JobState::Completed) {
        std::cerr << "expected completed event" << std::endl;
        return false;
    }

    (void)service.add_tag(id, "release");
    if (events.size() != 4 || !events.back().message || events.back().message->find("tag added") == std::string::npos) {
        std::cerr << "expected tag-added event" << std::endl;
        return false;
    }

    return true;
}

bool test_mutation_returns_false_for_missing_job() {
    using namespace ppp::core;
    InMemoryJobRepository repo;
    JobService service{repo};

    const std::string missing = "nonexistent";
    if (service.retry(missing)) {
        std::cerr << "retry should return false for missing job" << std::endl;
        return false;
    }
    if (service.update_priority(missing, 10)) {
        std::cerr << "update_priority should return false for missing job" << std::endl;
        return false;
    }
    if (service.update_due_at(missing, std::nullopt)) {
        std::cerr << "update_due_at should return false for missing job" << std::endl;
        return false;
    }
    if (service.update_correlation(missing, std::nullopt)) {
        std::cerr << "update_correlation should return false for missing job" << std::endl;
        return false;
    }
    if (service.add_attachment(missing, "file.txt")) {
        std::cerr << "add_attachment should return false for missing job" << std::endl;
        return false;
    }
    if (service.remove_attachment(missing, "file.txt")) {
        std::cerr << "remove_attachment should return false for missing job" << std::endl;
        return false;
    }
    if (service.clear_attachments(missing)) {
        std::cerr << "clear_attachments should return false for missing job" << std::endl;
        return false;
    }
    if (service.add_tag(missing, "tag")) {
        std::cerr << "add_tag should return false for missing job" << std::endl;
        return false;
    }
    if (service.remove_tag(missing, "tag")) {
        std::cerr << "remove_tag should return false for missing job" << std::endl;
        return false;
    }
    if (service.clear_tags(missing)) {
        std::cerr << "clear_tags should return false for missing job" << std::endl;
        return false;
    }

    // Verify existing job returns true
    const auto id = service.create_job(JobPayload{"test.pdf", std::nullopt, {}, {}});
    if (!service.update_priority(id, 5)) {
        std::cerr << "update_priority should return true for existing job" << std::endl;
        return false;
    }
    if (!service.add_tag(id, "label")) {
        std::cerr << "add_tag should return true for existing job" << std::endl;
        return false;
    }

    return true;
}

bool test_job_processor_cancellation() {
    using namespace ppp::core;
    InMemoryJobRepository repo;
    JobService service{repo};

    service.create_job(JobPayload{"cancel_me.pdf", std::nullopt, {}, {}});

    JobProcessor processor{service, [](const JobRecord&) {
                               return JobExecutionResult::cancelled("user requested");
                           }};

    auto result = processor.process_next();
    if (!result) {
        std::cerr << "expected a processed job" << std::endl;
        return false;
    }
    if (result->result.outcome != JobExecutionResult::Outcome::Cancelled) {
        std::cerr << "expected cancelled outcome" << std::endl;
        return false;
    }

    auto record = service.get(result->id);
    if (!record || record->state != JobState::Cancelled) {
        std::cerr << "expected job in cancelled state" << std::endl;
        return false;
    }
    if (!record->error_message || record->error_message->find("user requested") == std::string::npos) {
        std::cerr << "expected cancellation reason in error_message" << std::endl;
        return false;
    }

    return true;
}

bool test_processing_config_enum_round_trip() {
    using namespace ppp::core;

    // MeasurementUnit
    for (auto u : {MeasurementUnit::Inches, MeasurementUnit::Pixels, MeasurementUnit::Millimeters}) {
        auto parsed = measurement_unit_from_string(to_string(u));
        if (!parsed || *parsed != u) {
            std::cerr << "MeasurementUnit round-trip failed" << std::endl;
            return false;
        }
    }

    // CanvasPreset
    for (auto cp : {CanvasPreset::Autodetect, CanvasPreset::Letter, CanvasPreset::Legal,
                    CanvasPreset::Tabloid, CanvasPreset::A4, CanvasPreset::A3, CanvasPreset::Custom}) {
        auto parsed = canvas_preset_from_string(to_string(cp));
        if (!parsed || *parsed != cp) {
            std::cerr << "CanvasPreset round-trip failed" << std::endl;
            return false;
        }
    }

    // RasterFormat
    for (auto rf : {RasterFormat::Raw, RasterFormat::Group4, RasterFormat::LZW, RasterFormat::JPEG}) {
        auto parsed = raster_format_from_string(to_string(rf));
        if (!parsed || *parsed != rf) {
            std::cerr << "RasterFormat round-trip failed" << std::endl;
            return false;
        }
    }

    // Short aliases
    if (measurement_unit_from_string("mm") != MeasurementUnit::Millimeters) {
        std::cerr << "mm alias failed" << std::endl;
        return false;
    }
    if (raster_format_from_string("jpg") != RasterFormat::JPEG) {
        std::cerr << "jpg alias failed" << std::endl;
        return false;
    }

    return true;
}

bool test_processing_profile_json_round_trip() {
    using namespace ppp::core;

    ProcessingProfile profile;
    profile.name = "test-profile";
    profile.working_unit = MeasurementUnit::Millimeters;
    profile.detected_dpi = 300;
    profile.detected_width = 2550;
    profile.detected_height = 3300;
    profile.position_image = true;
    profile.keep_outside_subimage = true;
    profile.keep_original_size = false;
    profile.odd_even_mode = true;
    profile.rotation = Rotation::CW90;

    profile.canvas.preset = CanvasPreset::A4;
    profile.canvas.width = {210.0, MeasurementUnit::Millimeters};
    profile.canvas.height = {297.0, MeasurementUnit::Millimeters};
    profile.canvas.orientation = Orientation::Portrait;

    // Margins: set up odd page margins with non-default values
    profile.margins[0].top.distance = {25.4, MeasurementUnit::Millimeters};
    profile.margins[0].top.mode = MarginMode::Set;
    profile.margins[0].left.distance = {12.7, MeasurementUnit::Millimeters};
    profile.margins[0].left.mode = MarginMode::Check;
    profile.margins[0].center_horizontal = true;
    profile.margins[0].keep_horizontal = true;
    profile.margins[0].keep_x = {5.0, MeasurementUnit::Millimeters};

    // Offsets
    profile.offsets[0].x = {1.5, MeasurementUnit::Inches};
    profile.offsets[0].y = {2.0, MeasurementUnit::Inches};

    // Deskew
    profile.deskew.enabled = true;
    profile.deskew.detect_mode = 3;
    profile.deskew.min_angle = 0.1;
    profile.deskew.max_angle = 5.0;
    profile.deskew.fast = true;
    profile.deskew.interpolate = true;
    profile.deskew.character_protect = true;
    profile.deskew.char_protect_below = 0.5;

    // Despeckle
    profile.despeckle.mode = DespeckleMode::Object;
    profile.despeckle.object_min = 2;
    profile.despeckle.object_max = 8;

    // Edge cleanup
    profile.edge_cleanup.enabled = true;
    profile.edge_cleanup.order = EdgeCleanupOrder::AfterDeskew;
    profile.edge_cleanup.set1.top = {0.5, MeasurementUnit::Inches};
    profile.edge_cleanup.set1.left = {0.25, MeasurementUnit::Inches};

    // Hole cleanup
    profile.hole_cleanup.enabled = true;
    profile.hole_cleanup.set1.top = {1.0, MeasurementUnit::Inches};

    // Subimage
    profile.subimage.max_width = {8.0, MeasurementUnit::Inches};
    profile.subimage.max_height = {10.5, MeasurementUnit::Inches};
    profile.subimage.report_small = true;
    profile.subimage.min_width_px = 50;
    profile.subimage.min_height_px = 50;

    // Movement limit
    profile.movement_limit.enabled = true;
    profile.movement_limit.max_vertical = {3.0, MeasurementUnit::Inches};
    profile.movement_limit.max_horizontal = {2.0, MeasurementUnit::Inches};

    // Resize
    profile.resize.enabled = true;
    profile.resize.source = ResizeFrom::Smart;
    profile.resize.anti_alias = true;
    profile.resize.keep_size = false;
    profile.resize.canvas.preset = CanvasPreset::Letter;
    profile.resize.v_alignment = VAlignment::Top;
    profile.resize.h_alignment = HAlignment::Proportional;
    profile.resize.allow_shrink = true;
    profile.resize.allow_enlarge = false;
    profile.resize.output_path = "/output/resized";
    profile.resize.merge_tiff = true;
    profile.resize.merge_tiff_name = "merged.tif";

    // Output
    profile.output.tiff_output = true;
    profile.output.pdf_output = true;
    profile.output.raster_format = RasterFormat::Group4;
    profile.output.jpeg_quality = 90;
    profile.output.new_extension = "out.tif";
    profile.output.save_to_different_dir = true;
    profile.output.output_directory = "/output/dir";
    profile.output.conflict_policy = ConflictPolicy::Report;
    profile.output.path_mode = PathMode::Absolute;

    // Scan
    profile.scan.prefix = "SCAN_";
    profile.scan.start_at = 10;
    profile.scan.increment = 2;
    profile.scan.extension = "png";
    profile.scan.crop = true;
    profile.scan.verso_recto = true;
    profile.scan.force_resolution = 600;

    // Print
    profile.print.page_use_mode = 1;
    profile.print.scale_mode = 2;
    profile.print.scale_x = 150.0;
    profile.print.scale_y = 75.0;

    profile.page_detection_style_sheet = "Custom Sheet";

    // Serialize
    const auto json = processing_profile_to_json(profile);
    if (json.empty()) {
        std::cerr << "profile serialization produced empty string" << std::endl;
        return false;
    }

    // Deserialize
    auto parsed = processing_profile_from_json(json);
    if (!parsed) {
        std::cerr << "profile deserialization failed" << std::endl;
        return false;
    }

    const auto& p = *parsed;

    // Verify top-level fields
    if (p.name != "test-profile") { std::cerr << "name mismatch" << std::endl; return false; }
    if (p.working_unit != MeasurementUnit::Millimeters) { std::cerr << "working_unit mismatch" << std::endl; return false; }
    if (p.detected_dpi != 300) { std::cerr << "detected_dpi mismatch" << std::endl; return false; }
    if (p.detected_width != 2550) { std::cerr << "detected_width mismatch" << std::endl; return false; }
    if (p.detected_height != 3300) { std::cerr << "detected_height mismatch" << std::endl; return false; }
    if (!p.position_image) { std::cerr << "position_image mismatch" << std::endl; return false; }
    if (!p.keep_outside_subimage) { std::cerr << "keep_outside_subimage mismatch" << std::endl; return false; }
    if (p.keep_original_size) { std::cerr << "keep_original_size mismatch" << std::endl; return false; }
    if (!p.odd_even_mode) { std::cerr << "odd_even_mode mismatch" << std::endl; return false; }
    if (p.rotation != Rotation::CW90) { std::cerr << "rotation mismatch" << std::endl; return false; }

    // Canvas
    if (p.canvas.preset != CanvasPreset::A4) { std::cerr << "canvas preset mismatch" << std::endl; return false; }
    if (p.canvas.width.value != 210.0) { std::cerr << "canvas width mismatch" << std::endl; return false; }
    if (p.canvas.orientation != Orientation::Portrait) { std::cerr << "canvas orientation mismatch" << std::endl; return false; }

    // Margins
    if (p.margins[0].top.distance.value != 25.4) { std::cerr << "margin top value mismatch" << std::endl; return false; }
    if (p.margins[0].top.mode != MarginMode::Set) { std::cerr << "margin top mode mismatch" << std::endl; return false; }
    if (p.margins[0].left.mode != MarginMode::Check) { std::cerr << "margin left mode mismatch" << std::endl; return false; }
    if (!p.margins[0].center_horizontal) { std::cerr << "margin center_horizontal mismatch" << std::endl; return false; }
    if (!p.margins[0].keep_horizontal) { std::cerr << "margin keep_horizontal mismatch" << std::endl; return false; }

    // Offsets
    if (p.offsets[0].x.value != 1.5) { std::cerr << "offset x mismatch" << std::endl; return false; }

    // Deskew
    if (!p.deskew.enabled) { std::cerr << "deskew enabled mismatch" << std::endl; return false; }
    if (p.deskew.detect_mode != 3) { std::cerr << "deskew detect_mode mismatch" << std::endl; return false; }
    if (p.deskew.max_angle != 5.0) { std::cerr << "deskew max_angle mismatch" << std::endl; return false; }
    if (!p.deskew.fast) { std::cerr << "deskew fast mismatch" << std::endl; return false; }
    if (!p.deskew.interpolate) { std::cerr << "deskew interpolate mismatch" << std::endl; return false; }

    // Despeckle
    if (p.despeckle.mode != DespeckleMode::Object) { std::cerr << "despeckle mode mismatch" << std::endl; return false; }
    if (p.despeckle.object_min != 2) { std::cerr << "despeckle object_min mismatch" << std::endl; return false; }

    // Edge cleanup
    if (!p.edge_cleanup.enabled) { std::cerr << "edge_cleanup enabled mismatch" << std::endl; return false; }
    if (p.edge_cleanup.order != EdgeCleanupOrder::AfterDeskew) { std::cerr << "edge_cleanup order mismatch" << std::endl; return false; }

    // Resize
    if (!p.resize.enabled) { std::cerr << "resize enabled mismatch" << std::endl; return false; }
    if (p.resize.source != ResizeFrom::Smart) { std::cerr << "resize source mismatch" << std::endl; return false; }
    if (p.resize.v_alignment != VAlignment::Top) { std::cerr << "resize v_alignment mismatch" << std::endl; return false; }
    if (p.resize.h_alignment != HAlignment::Proportional) { std::cerr << "resize h_alignment mismatch" << std::endl; return false; }
    if (p.resize.merge_tiff_name != "merged.tif") { std::cerr << "resize merge_tiff_name mismatch" << std::endl; return false; }

    // Output
    if (p.output.raster_format != RasterFormat::Group4) { std::cerr << "output raster_format mismatch" << std::endl; return false; }
    if (p.output.jpeg_quality != 90) { std::cerr << "output jpeg_quality mismatch" << std::endl; return false; }
    if (p.output.conflict_policy != ConflictPolicy::Report) { std::cerr << "output conflict_policy mismatch" << std::endl; return false; }
    if (p.output.path_mode != PathMode::Absolute) { std::cerr << "output path_mode mismatch" << std::endl; return false; }

    // Scan
    if (p.scan.prefix != "SCAN_") { std::cerr << "scan prefix mismatch" << std::endl; return false; }
    if (p.scan.start_at != 10) { std::cerr << "scan start_at mismatch" << std::endl; return false; }
    if (p.scan.force_resolution != 600) { std::cerr << "scan force_resolution mismatch" << std::endl; return false; }

    // Print
    if (p.print.scale_mode != 2) { std::cerr << "print scale_mode mismatch" << std::endl; return false; }
    if (p.print.scale_x != 150.0) { std::cerr << "print scale_x mismatch" << std::endl; return false; }

    if (p.page_detection_style_sheet != "Custom Sheet") { std::cerr << "page_detection_style_sheet mismatch" << std::endl; return false; }

    return true;
}

bool test_processing_profile_file_round_trip() {
    using namespace ppp::core;

    ProcessingProfile profile;
    profile.name = "file-test";
    profile.deskew.enabled = true;
    profile.deskew.max_angle = 7.5;
    profile.output.pdf_output = true;
    profile.output.jpeg_quality = 85;

    auto temp = fs::temp_directory_path() / "ppp_profile_test.json";
    if (!write_processing_profile(profile, temp)) {
        std::cerr << "failed to write processing profile" << std::endl;
        return false;
    }

    auto loaded = read_processing_profile(temp);
    fs::remove(temp);

    if (!loaded) {
        std::cerr << "failed to read processing profile" << std::endl;
        return false;
    }

    if (loaded->name != "file-test") { std::cerr << "file round-trip name mismatch" << std::endl; return false; }
    if (!loaded->deskew.enabled) { std::cerr << "file round-trip deskew mismatch" << std::endl; return false; }
    if (loaded->deskew.max_angle != 7.5) { std::cerr << "file round-trip max_angle mismatch" << std::endl; return false; }
    if (!loaded->output.pdf_output) { std::cerr << "file round-trip pdf_output mismatch" << std::endl; return false; }
    if (loaded->output.jpeg_quality != 85) { std::cerr << "file round-trip jpeg_quality mismatch" << std::endl; return false; }

    return true;
}

bool test_processing_profile_default_values() {
    using namespace ppp::core;

    // Default-constructed profile should round-trip cleanly
    ProcessingProfile profile;
    const auto json = processing_profile_to_json(profile);
    auto parsed = processing_profile_from_json(json);
    if (!parsed) {
        std::cerr << "default profile round-trip failed" << std::endl;
        return false;
    }

    // Verify key defaults survived
    if (parsed->working_unit != MeasurementUnit::Inches) { std::cerr << "default working_unit wrong" << std::endl; return false; }
    if (parsed->canvas.preset != CanvasPreset::Letter) { std::cerr << "default canvas preset wrong" << std::endl; return false; }
    if (parsed->rotation != Rotation::None) { std::cerr << "default rotation wrong" << std::endl; return false; }
    if (parsed->deskew.enabled) { std::cerr << "default deskew should be disabled" << std::endl; return false; }
    if (parsed->despeckle.mode != DespeckleMode::None) { std::cerr << "default despeckle wrong" << std::endl; return false; }
    if (parsed->output.raster_format != RasterFormat::JPEG) { std::cerr << "default raster_format wrong" << std::endl; return false; }
    if (parsed->output.jpeg_quality != 75) { std::cerr << "default jpeg_quality wrong" << std::endl; return false; }
    if (!parsed->resize.keep_size) { std::cerr << "default resize keep_size wrong" << std::endl; return false; }
    if (parsed->page_detection_style_sheet != "Factory defaults") { std::cerr << "default style sheet wrong" << std::endl; return false; }

    return true;
}

} // namespace

int main() {
    std::vector<std::pair<std::string, bool (*)()>> tests = {
        {"job_state_round_trip", test_job_state_round_trip},
        {"scheduling_policy_config_parsing", test_scheduling_policy_config_parsing},
        {"scheduling_policy_directory_layering", test_scheduling_policy_directory_layering},
        {"file_repository_persistence", test_file_repository_persistence},
        {"priority_ordering_in_memory", test_priority_ordering_in_memory},
        {"due_ordering_in_memory", test_due_ordering_in_memory},
        {"scheduling_policy_escalation", test_scheduling_policy_escalation},
        {"job_processor_completes_job", test_job_processor_completes_job},
        {"job_processor_handles_failure", test_job_processor_handles_failure_result},
        {"job_processor_catches_exception", test_job_processor_catches_exception},
        {"job_service_claims_submitted", test_job_service_claims_submitted_jobs},
        {"job_service_retry_resubmits", test_job_service_retry_resubmits},
        {"job_service_attachment_mutation", test_job_service_attachment_mutation},
        {"job_service_tag_mutation", test_job_service_tag_mutation},
        {"job_service_tag_filtering", test_job_service_tag_filtering},
        {"job_service_correlation_updates", test_job_service_correlation_updates},
        {"job_service_resume_active_jobs", test_job_service_resume_active_jobs},
        {"job_service_summary", test_job_service_summary},
        {"job_service_purge_in_memory", test_job_service_purge_in_memory},
        {"job_summary_json", test_job_summary_json},
        {"job_json_serialization", test_job_json_serialization},
        {"job_json_parse_roundtrip", test_job_json_parse_roundtrip},
        {"job_json_file_export", test_job_json_file_export},
        {"event_sink_invocation", test_event_sink_invocation},
        {"mutation_returns_false_for_missing_job", test_mutation_returns_false_for_missing_job},
        {"job_processor_cancellation", test_job_processor_cancellation},
        {"processing_config_enum_round_trip", test_processing_config_enum_round_trip},
        {"processing_profile_json_round_trip", test_processing_profile_json_round_trip},
        {"processing_profile_file_round_trip", test_processing_profile_file_round_trip},
        {"processing_profile_default_values", test_processing_profile_default_values},
    };
#if PPP_CORE_HAVE_SQLITE
    tests.emplace_back("sqlite_repository_persistence", test_sqlite_repository_persistence);
    tests.emplace_back("sqlite_schema_migration", test_sqlite_schema_migration);
#endif

    bool all_passed = true;
    for (const auto& [name, fn] : tests) {
        if (!fn()) {
            std::cerr << "Test failed: " << name << std::endl;
            all_passed = false;
        }
    }

    if (!all_passed) {
        return 1;
    }

    std::cout << "All tests passed" << std::endl;
    return 0;
}

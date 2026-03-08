#include "ppp/core/job_processor.h"
#include "ppp/core/job_repository.h"
#include "ppp/core/job_serialization.h"
#include "ppp/core/job_service.h"
#include "ppp/core/processing_config.h"
#include "ppp/core/processing_config_io.h"
#include "ppp/core/scheduling_policy.h"
#include "ppp/core/scheduling_policy_io.h"
#include "ppp/core/tiff.h"
#include "ppp/core/geometry.h"
#include "ppp/core/image.h"
#include "ppp/core/image_ops.h"
#include "ppp/core/processing_pipeline.h"
#include "ppp/core/tiff_writer.h"
#include "ppp/core/bmp.h"
#include "ppp/core/pdf_writer.h"
#include "ppp/core/output_writer.h"

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

// Helper: build a minimal TIFF in memory (little-endian, 1 IFD)
std::vector<std::uint8_t> build_test_tiff() {
    // Build a minimal valid TIFF with:
    //   ImageWidth=100 (SHORT), ImageLength=200 (SHORT),
    //   BitsPerSample=1 (SHORT), Compression=4 (Group4, SHORT),
    //   PhotometricInterpretation=0 (WhiteIsZero, SHORT),
    //   XResolution=300/1 (RATIONAL), YResolution=300/1 (RATIONAL)
    //
    // Layout: header(8) + IFD(2 + 7*12 + 4) = 8 + 90 = 98
    //   then rational data at offset 98: 2 rationals * 8 bytes = 16
    //   total = 114 bytes

    std::vector<std::uint8_t> buf(114, 0);
    auto put16 = [&](std::size_t off, std::uint16_t v) {
        buf[off] = static_cast<std::uint8_t>(v & 0xFF);
        buf[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    auto put32 = [&](std::size_t off, std::uint32_t v) {
        buf[off] = static_cast<std::uint8_t>(v & 0xFF);
        buf[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        buf[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        buf[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    };

    // Header: "II" + magic 42 + first IFD offset 8
    buf[0] = 'I'; buf[1] = 'I';
    put16(2, 42);
    put32(4, 8);

    // IFD at offset 8
    constexpr std::size_t ifd_off = 8;
    constexpr std::uint16_t num_entries = 7;
    put16(ifd_off, num_entries);

    auto write_entry = [&](int idx, std::uint16_t tag, std::uint16_t type,
                           std::uint32_t count, std::uint32_t value) {
        std::size_t off = ifd_off + 2 + idx * 12;
        put16(off, tag);
        put16(off + 2, type);
        put32(off + 4, count);
        put32(off + 8, value);
    };

    // SHORT type = 3
    write_entry(0, 256, 3, 1, 100);   // ImageWidth = 100
    write_entry(1, 257, 3, 1, 200);   // ImageLength = 200
    write_entry(2, 258, 3, 1, 1);     // BitsPerSample = 1
    write_entry(3, 259, 3, 1, 4);     // Compression = Group4
    write_entry(4, 262, 3, 1, 0);     // Photometric = WhiteIsZero

    // RATIONAL type = 5, data at offset 98
    write_entry(5, 282, 5, 1, 98);    // XResolution -> offset 98
    write_entry(6, 283, 5, 1, 106);   // YResolution -> offset 106

    // Next IFD = 0 (no more IFDs)
    put32(ifd_off + 2 + num_entries * 12, 0);

    // Rational data at offset 98: 300/1
    put32(98, 300);  put32(102, 1);   // XResolution = 300/1
    put32(106, 300); put32(110, 1);   // YResolution = 300/1

    return buf;
}

bool test_tiff_parse_synthetic() {
    using namespace ppp::core::tiff;

    auto data = build_test_tiff();
    auto tiff = Structure::read(data);
    if (!tiff) {
        std::cerr << "failed to parse synthetic TIFF" << std::endl;
        return false;
    }

    if (tiff->page_count() != 1) {
        std::cerr << "expected 1 page, got " << tiff->page_count() << std::endl;
        return false;
    }
    if (tiff->byte_order() != ByteOrder::LittleEndian) {
        std::cerr << "expected little-endian" << std::endl;
        return false;
    }

    auto w = tiff->image_width();
    if (!w || *w != 100) { std::cerr << "ImageWidth mismatch" << std::endl; return false; }

    auto h = tiff->image_length();
    if (!h || *h != 200) { std::cerr << "ImageLength mismatch" << std::endl; return false; }

    auto xr = tiff->x_resolution();
    if (!xr || *xr != 300.0) { std::cerr << "XResolution mismatch" << std::endl; return false; }

    auto yr = tiff->y_resolution();
    if (!yr || *yr != 300.0) { std::cerr << "YResolution mismatch" << std::endl; return false; }

    auto comp = tiff->compression();
    if (!comp || *comp != Compression::Group4Fax) { std::cerr << "Compression mismatch" << std::endl; return false; }

    auto photo = tiff->photometric();
    if (!photo || *photo != Photometric::WhiteIsZero) { std::cerr << "Photometric mismatch" << std::endl; return false; }

    auto bps = tiff->bits_per_sample();
    if (!bps || *bps != 1) { std::cerr << "BitsPerSample mismatch" << std::endl; return false; }

    return true;
}

bool test_tiff_ifd_accessors() {
    using namespace ppp::core::tiff;

    Ifd ifd;

    // Set an integer tag
    IfdEntry width_entry;
    width_entry.type = FieldType::Short;
    width_entry.value = std::vector<std::uint16_t>{1024};
    ifd.set(Tag::ImageWidth, width_entry);

    // Set a string tag
    IfdEntry software_entry;
    software_entry.type = FieldType::Ascii;
    software_entry.value = std::string{"PPP Modern"};
    ifd.set(Tag::Software, software_entry);

    // Set a rational tag
    IfdEntry xres_entry;
    xres_entry.type = FieldType::Rational;
    xres_entry.value = std::vector<Rational>{{600, 1}};
    ifd.set(Tag::XResolution, xres_entry);

    // Test lookups
    auto w = ifd.get_int(Tag::ImageWidth);
    if (!w || *w != 1024) { std::cerr << "IFD ImageWidth mismatch" << std::endl; return false; }

    auto sw = ifd.get_string(Tag::Software);
    if (!sw || *sw != "PPP Modern") { std::cerr << "IFD Software mismatch" << std::endl; return false; }

    auto xr = ifd.get_double(Tag::XResolution);
    if (!xr || *xr != 600.0) { std::cerr << "IFD XResolution mismatch" << std::endl; return false; }

    // Missing tag
    if (ifd.find(Tag::YResolution) != nullptr) {
        std::cerr << "IFD should not find YResolution" << std::endl;
        return false;
    }
    if (ifd.get_int(Tag::YResolution).has_value()) {
        std::cerr << "IFD missing tag should return nullopt" << std::endl;
        return false;
    }

    return true;
}

bool test_tiff_invalid_data() {
    using namespace ppp::core::tiff;

    // Too short
    if (Structure::read(nullptr, 0).has_value()) { std::cerr << "should reject empty" << std::endl; return false; }

    std::vector<std::uint8_t> short_data = {'I', 'I', 42, 0};
    if (Structure::read(short_data).has_value()) { std::cerr << "should reject too short" << std::endl; return false; }

    // Bad magic
    std::vector<std::uint8_t> bad_magic = {'I', 'I', 0, 0, 0, 0, 0, 0};
    if (Structure::read(bad_magic).has_value()) { std::cerr << "should reject bad magic" << std::endl; return false; }

    // Bad byte order
    std::vector<std::uint8_t> bad_order = {'X', 'X', 42, 0, 0, 0, 0, 0};
    if (Structure::read(bad_order).has_value()) { std::cerr << "should reject bad byte order" << std::endl; return false; }

    return true;
}

bool test_tiff_big_endian() {
    using namespace ppp::core::tiff;

    // Build a big-endian TIFF with just ImageWidth=512 (SHORT)
    std::vector<std::uint8_t> buf(8 + 2 + 12 + 4, 0);
    auto put16be = [&](std::size_t off, std::uint16_t v) {
        buf[off] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        buf[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
    };
    auto put32be = [&](std::size_t off, std::uint32_t v) {
        buf[off] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
        buf[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        buf[off + 2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        buf[off + 3] = static_cast<std::uint8_t>(v & 0xFF);
    };

    buf[0] = 'M'; buf[1] = 'M';
    put16be(2, 42);
    put32be(4, 8);

    put16be(8, 1);  // 1 entry
    put16be(10, 256);  // ImageWidth
    put16be(12, 3);    // SHORT
    put32be(14, 1);    // count = 1
    put16be(18, 512);  // value = 512 (in first 2 bytes of value field for BE SHORT)
    put32be(22, 0);    // next IFD = 0

    auto tiff = Structure::read(buf);
    if (!tiff) { std::cerr << "failed to parse big-endian TIFF" << std::endl; return false; }
    if (tiff->byte_order() != ByteOrder::BigEndian) { std::cerr << "expected big-endian" << std::endl; return false; }

    auto w = tiff->image_width();
    if (!w || *w != 512) { std::cerr << "BE ImageWidth mismatch: got " << (w ? *w : -1) << std::endl; return false; }

    return true;
}

// ---------------------------------------------------------------------------
// Geometry tests
// ---------------------------------------------------------------------------

bool test_geometry_rect_basics() {
    using namespace ppp::core::geometry;

    Rect r{10, 20, 110, 70};
    if (r.width() != 100) return false;
    if (r.height() != 50) return false;
    if (r.area() != 5000) return false;
    if (r.empty()) return false;

    // Empty rect.
    Rect e{5, 5, 5, 10};
    if (!e.empty()) return false;
    if (e.area() != 0) return false;

    // Contains point.
    if (!r.contains(10, 20)) return false;
    if (r.contains(110, 70)) return false;  // Exclusive boundary.
    if (r.contains(9, 20)) return false;

    // Contains rect.
    Rect inner{20, 30, 50, 50};
    if (!r.contains(inner)) return false;
    if (inner.contains(r)) return false;

    // Intersection.
    Rect a{0, 0, 50, 50};
    Rect b{25, 25, 75, 75};
    auto isect = a.intersection(b);
    if (isect != Rect{25, 25, 50, 50}) return false;

    // Non-intersecting.
    Rect c{100, 100, 200, 200};
    if (a.intersects(c)) return false;
    if (!a.intersection(c).empty()) return false;

    // Union.
    auto u = a.united(b);
    if (u != Rect{0, 0, 75, 75}) return false;

    // Inflate / offset.
    Rect m{10, 10, 20, 20};
    m.inflate(5, 5);
    if (m != Rect{5, 5, 25, 25}) return false;
    m.offset(10, 10);
    if (m != Rect{15, 15, 35, 35}) return false;

    return true;
}

bool test_geometry_combinable() {
    using namespace ppp::core::geometry;

    // Two adjacent non-overlapping rects with no gap are combinable with 0 tolerance.
    Rect a{0, 0, 10, 10};
    Rect b{10, 0, 20, 10};
    // Union area = 200, sum = 100 + 100 = 200.
    if (!combinable(a, b, 0)) return false;

    // With a gap — not combinable without tolerance.
    Rect c{15, 0, 25, 10};
    // Union = {0,0,25,10} = 250, sum = 200. 250 > 200.
    if (combinable(a, c, 0)) return false;
    // But with enough tolerance.
    if (!combinable(a, c, 50)) return false;

    return true;
}

bool test_geometry_sort() {
    using namespace ppp::core::geometry;

    std::vector<Rect> rects = {
        {30, 10, 40, 20},
        {10, 30, 20, 40},
        {50, 50, 60, 60},
        {10, 10, 20, 20},
    };

    // Top-to-bottom.
    auto v = rects;
    sort_rects(v, SortAxis::TopToBottom);
    if (v[0].top != 10 || v[0].left != 10) return false;  // {10,10} and {30,10} tied, left wins
    if (v[1].left != 30) return false;
    if (v[2].top != 30) return false;
    if (v[3].top != 50) return false;

    // Left-to-right.
    v = rects;
    sort_rects(v, SortAxis::LeftToRight);
    if (v[0].left != 10) return false;
    if (v[2].left != 30) return false;
    if (v[3].left != 50) return false;

    // Bottom-to-top.
    v = rects;
    sort_rects(v, SortAxis::BottomToTop);
    if (v[0].bottom != 60) return false;
    if (v[3].bottom != 20) return false;

    return true;
}

bool test_geometry_banding() {
    using namespace ppp::core::geometry;

    // Three rects: two in the same horizontal band, one below.
    std::vector<Rect> rects = {
        {0, 0, 10, 10},
        {20, 2, 30, 12},
        {5, 50, 15, 60},
    };

    auto bands = band_rects(rects, BandDirection::Horizontal);
    if (bands.size() != 2) { std::cerr << "expected 2 bands, got " << bands.size() << std::endl; return false; }
    if (bands[0].size() != 2) return false;  // First band has 2 rects.
    if (bands[1].size() != 1) return false;  // Second band has 1 rect.
    // Within band, sorted by left.
    if (bands[0][0].left != 0) return false;
    if (bands[0][1].left != 20) return false;

    // Vertical banding.
    std::vector<Rect> rects2 = {
        {0, 0, 10, 10},
        {2, 20, 12, 30},
        {50, 5, 60, 15},
    };
    auto vbands = band_rects(rects2, BandDirection::Vertical);
    if (vbands.size() != 2) return false;
    if (vbands[0].size() != 2) return false;
    if (vbands[1].size() != 1) return false;

    return true;
}

bool test_geometry_spans_from_bitmap() {
    using namespace ppp::core::geometry;

    // 16x2 bitmap, 2 bytes per row, no padding.
    // Row 0: 0xFF 0x00 → bits 0..7 foreground, 8..15 background.
    // Row 1: 0x00 0xF0 → bits 0..7 background, 8..11 foreground, 12..15 background.
    std::uint8_t data[] = {
        0xFF, 0x00,
        0x00, 0xF0,
    };

    auto spans = spans_from_bitmap(data, 16, 2, 2, 1);
    if (spans.size() != 2) { std::cerr << "expected 2 spans, got " << spans.size() << std::endl; return false; }

    // Row 0: cols 0..8.
    if (spans[0].row != 0 || spans[0].col_start != 0 || spans[0].col_end != 8) {
        std::cerr << "span[0]: row=" << spans[0].row << " start=" << spans[0].col_start << " end=" << spans[0].col_end << std::endl;
        return false;
    }

    // Row 1: cols 8..12.
    if (spans[1].row != 1 || spans[1].col_start != 8 || spans[1].col_end != 12) {
        std::cerr << "span[1]: row=" << spans[1].row << " start=" << spans[1].col_start << " end=" << spans[1].col_end << std::endl;
        return false;
    }

    return true;
}

bool test_geometry_connected_components() {
    using namespace ppp::core::geometry;

    // Build a simple 8x4 binary image with two separate blobs:
    //   Row 0: XX......
    //   Row 1: XX......
    //   Row 2: ......XX
    //   Row 3: ......XX
    std::vector<Span> spans = {
        {0, 0, 2},
        {1, 0, 2},
        {2, 6, 8},
        {3, 6, 8},
    };

    Rect roi{0, 0, 8, 4};
    auto comps = find_components(spans, roi, 4);
    if (comps.size() != 2) { std::cerr << "expected 2 components, got " << comps.size() << std::endl; return false; }

    // Sort by left edge for deterministic checking.
    std::sort(comps.begin(), comps.end(),
              [](const Component& a, const Component& b) { return a.bounds.left < b.bounds.left; });

    if (comps[0].bounds != Rect{0, 0, 2, 2}) {
        std::cerr << "comp[0] bounds mismatch" << std::endl; return false;
    }
    if (comps[0].pixel_count != 4) return false;

    if (comps[1].bounds != Rect{6, 2, 8, 4}) {
        std::cerr << "comp[1] bounds mismatch" << std::endl; return false;
    }
    if (comps[1].pixel_count != 4) return false;

    // Test 8-connectivity: diagonal spans should merge.
    std::vector<Span> diag_spans = {
        {0, 0, 2},
        {1, 2, 4},  // Diagonally adjacent to row 0 span.
    };
    auto comps4 = find_components(diag_spans, {0, 0, 10, 10}, 4);
    auto comps8 = find_components(diag_spans, {0, 0, 10, 10}, 8);
    if (comps4.size() != 2) { std::cerr << "4-conn: expected 2 components for diagonal" << std::endl; return false; }
    if (comps8.size() != 1) { std::cerr << "8-conn: expected 1 component for diagonal" << std::endl; return false; }

    return true;
}

bool test_geometry_connected_components_roi() {
    using namespace ppp::core::geometry;

    // Single blob spanning full width, but ROI clips it.
    std::vector<Span> spans = {
        {0, 0, 100},
        {1, 0, 100},
    };

    // ROI only covers left half.
    Rect roi{0, 0, 50, 2};
    auto comps = find_components(spans, roi, 4);
    if (comps.size() != 1) return false;
    if (comps[0].bounds.right != 50) return false;
    if (comps[0].pixel_count != 100) return false;  // 50 * 2 rows.

    return true;
}

// ---------------------------------------------------------------------------
// Image tests
// ---------------------------------------------------------------------------

bool test_image_construction() {
    using namespace ppp::core;

    // Default construction.
    Image empty;
    if (!empty.empty()) return false;
    if (empty.width() != 0 || empty.height() != 0) return false;

    // Gray8 image.
    Image gray(100, 50, PixelFormat::Gray8);
    if (gray.width() != 100 || gray.height() != 50) return false;
    if (gray.format() != PixelFormat::Gray8) return false;
    if (gray.stride() != 100) return false;  // 100 bytes, already 4-aligned.
    if (gray.empty()) return false;

    // RGB24 image — stride should be padded.
    Image rgb(10, 10, PixelFormat::RGB24);
    // 10 * 3 = 30 bytes per row → padded to 32.
    if (rgb.stride() != 32) return false;

    // BW1 image.
    Image bw(100, 10, PixelFormat::BW1);
    // 100 bits = 13 bytes → padded to 16.
    if (bw.stride() != 16) {
        std::cerr << "bw stride: " << bw.stride() << " expected 16" << std::endl;
        return false;
    }

    // DPI constructor.
    Image dpi_img(100, 100, PixelFormat::Gray8, 300.0, 600.0);
    if (dpi_img.dpi_x() != 300.0 || dpi_img.dpi_y() != 600.0) return false;

    return true;
}

bool test_image_bw_pixel_access() {
    using namespace ppp::core;

    Image bw(16, 4, PixelFormat::BW1);

    // All pixels should start as 0.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 16; ++x)
            if (bw.get_bw_pixel(x, y) != 0) return false;

    // Set some pixels.
    bw.set_bw_pixel(0, 0, 1);
    bw.set_bw_pixel(7, 0, 1);
    bw.set_bw_pixel(8, 0, 1);
    bw.set_bw_pixel(15, 3, 1);

    if (bw.get_bw_pixel(0, 0) != 1) return false;
    if (bw.get_bw_pixel(1, 0) != 0) return false;
    if (bw.get_bw_pixel(7, 0) != 1) return false;
    if (bw.get_bw_pixel(8, 0) != 1) return false;
    if (bw.get_bw_pixel(15, 3) != 1) return false;

    // Clear a pixel.
    bw.set_bw_pixel(0, 0, 0);
    if (bw.get_bw_pixel(0, 0) != 0) return false;

    return true;
}

bool test_image_fill_and_invert() {
    using namespace ppp::core;

    Image gray(4, 4, PixelFormat::Gray8);
    gray.fill(0x80);
    if (gray.row(0)[0] != 0x80 || gray.row(3)[3] != 0x80) return false;

    gray.invert();
    if (gray.row(0)[0] != 0x7F || gray.row(3)[3] != 0x7F) return false;

    return true;
}

bool test_image_crop() {
    using namespace ppp::core;

    // Create a 10x10 gray image with row = y value.
    Image img(10, 10, PixelFormat::Gray8);
    for (int y = 0; y < 10; ++y)
        std::memset(img.row(y), static_cast<std::uint8_t>(y * 10), 10);

    auto cropped = img.crop(2, 3, 5, 4);
    if (cropped.width() != 5 || cropped.height() != 4) return false;
    // Row 0 of cropped = row 3 of original → value 30.
    if (cropped.row(0)[0] != 30) return false;
    // Row 3 of cropped = row 6 of original → value 60.
    if (cropped.row(3)[0] != 60) return false;

    // Crop with clamping.
    auto clamped = img.crop(8, 8, 10, 10);
    if (clamped.width() != 2 || clamped.height() != 2) return false;

    // BW1 crop.
    Image bw(16, 8, PixelFormat::BW1);
    bw.set_bw_pixel(5, 3, 1);
    auto bw_crop = bw.crop(4, 2, 4, 4);
    if (bw_crop.get_bw_pixel(1, 1) != 1) return false;  // (5-4, 3-2)
    if (bw_crop.get_bw_pixel(0, 0) != 0) return false;

    return true;
}

bool test_image_rotate_cw90() {
    using namespace ppp::core;

    // 3x2 gray image:
    //   [1, 2, 3]
    //   [4, 5, 6]
    Image img(3, 2, PixelFormat::Gray8);
    img.row(0)[0] = 1; img.row(0)[1] = 2; img.row(0)[2] = 3;
    img.row(1)[0] = 4; img.row(1)[1] = 5; img.row(1)[2] = 6;

    auto rot = img.rotate_cw90();
    // CW90: 3x2 → 2x3
    //   [4, 1]
    //   [5, 2]
    //   [6, 3]
    if (rot.width() != 2 || rot.height() != 3) return false;
    if (rot.row(0)[0] != 4 || rot.row(0)[1] != 1) return false;
    if (rot.row(1)[0] != 5 || rot.row(1)[1] != 2) return false;
    if (rot.row(2)[0] != 6 || rot.row(2)[1] != 3) return false;

    return true;
}

bool test_image_rotate_ccw90() {
    using namespace ppp::core;

    Image img(3, 2, PixelFormat::Gray8);
    img.row(0)[0] = 1; img.row(0)[1] = 2; img.row(0)[2] = 3;
    img.row(1)[0] = 4; img.row(1)[1] = 5; img.row(1)[2] = 6;

    auto rot = img.rotate_ccw90();
    // CCW90: 3x2 → 2x3
    //   [3, 6]
    //   [2, 5]
    //   [1, 4]
    if (rot.width() != 2 || rot.height() != 3) return false;
    if (rot.row(0)[0] != 3 || rot.row(0)[1] != 6) return false;
    if (rot.row(1)[0] != 2 || rot.row(1)[1] != 5) return false;
    if (rot.row(2)[0] != 1 || rot.row(2)[1] != 4) return false;

    return true;
}

bool test_image_rotate_180() {
    using namespace ppp::core;

    Image img(3, 2, PixelFormat::Gray8);
    img.row(0)[0] = 1; img.row(0)[1] = 2; img.row(0)[2] = 3;
    img.row(1)[0] = 4; img.row(1)[1] = 5; img.row(1)[2] = 6;

    auto rot = img.rotate_180();
    // 180: [6,5,4], [3,2,1]
    if (rot.width() != 3 || rot.height() != 2) return false;
    if (rot.row(0)[0] != 6 || rot.row(0)[1] != 5 || rot.row(0)[2] != 4) return false;
    if (rot.row(1)[0] != 3 || rot.row(1)[1] != 2 || rot.row(1)[2] != 1) return false;

    return true;
}

bool test_image_pad_and_blit() {
    using namespace ppp::core;

    Image img(4, 4, PixelFormat::Gray8);
    img.fill(100);

    auto padded = img.pad(2, 3, 3, 2, 0);
    // New size: (4+3+3) x (4+2+2) = 10 x 8.
    if (padded.width() != 10 || padded.height() != 8) return false;
    // Padding area should be 0.
    if (padded.row(0)[0] != 0) return false;
    // Content area at (3,2).
    if (padded.row(2)[3] != 100) return false;
    if (padded.row(5)[6] != 100) return false;

    return true;
}

bool test_image_convert_gray_to_bw() {
    using namespace ppp::core;

    Image gray(8, 1, PixelFormat::Gray8);
    // Pixels: 0, 64, 127, 128, 200, 255, 50, 130
    gray.row(0)[0] = 0;
    gray.row(0)[1] = 64;
    gray.row(0)[2] = 127;
    gray.row(0)[3] = 128;
    gray.row(0)[4] = 200;
    gray.row(0)[5] = 255;
    gray.row(0)[6] = 50;
    gray.row(0)[7] = 130;

    auto bw = gray.convert(PixelFormat::BW1, 128);
    // < 128 → 1 (black), >= 128 → 0 (white).
    if (bw.get_bw_pixel(0, 0) != 1) return false;   // 0 < 128
    if (bw.get_bw_pixel(1, 0) != 1) return false;   // 64 < 128
    if (bw.get_bw_pixel(2, 0) != 1) return false;   // 127 < 128
    if (bw.get_bw_pixel(3, 0) != 0) return false;   // 128 >= 128
    if (bw.get_bw_pixel(4, 0) != 0) return false;   // 200 >= 128
    if (bw.get_bw_pixel(5, 0) != 0) return false;   // 255 >= 128
    if (bw.get_bw_pixel(6, 0) != 1) return false;   // 50 < 128
    if (bw.get_bw_pixel(7, 0) != 0) return false;   // 130 >= 128

    return true;
}

bool test_image_convert_bw_to_gray() {
    using namespace ppp::core;

    Image bw(8, 1, PixelFormat::BW1);
    bw.set_bw_pixel(0, 0, 1);  // black
    bw.set_bw_pixel(1, 0, 0);  // white

    auto gray = bw.convert(PixelFormat::Gray8);
    if (gray.row(0)[0] != 0) return false;     // black → 0
    if (gray.row(0)[1] != 255) return false;   // white → 255

    return true;
}

bool test_image_convert_rgb_to_gray() {
    using namespace ppp::core;

    Image rgb(2, 1, PixelFormat::RGB24);
    // Pure white.
    rgb.row(0)[0] = 255; rgb.row(0)[1] = 255; rgb.row(0)[2] = 255;
    // Pure black.
    rgb.row(0)[3] = 0; rgb.row(0)[4] = 0; rgb.row(0)[5] = 0;

    auto gray = rgb.convert(PixelFormat::Gray8);
    if (gray.row(0)[0] != 255) return false;  // White stays white.
    if (gray.row(0)[1] != 0) return false;    // Black stays black.

    return true;
}

bool test_image_deep_copy() {
    using namespace ppp::core;

    Image orig(10, 10, PixelFormat::Gray8);
    orig.fill(42);

    Image copy = orig;
    copy.fill(99);

    // Original should be unchanged.
    if (orig.row(0)[0] != 42) return false;
    if (copy.row(0)[0] != 99) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Image operations tests
// ---------------------------------------------------------------------------

bool test_ops_unit_conversion() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Inches at 300 DPI.
    Measurement m_in{1.0, MeasurementUnit::Inches};
    if (std::abs(to_pixels(m_in, 300.0) - 300.0) > 0.01) return false;

    // Pixels (identity).
    Measurement m_px{150.0, MeasurementUnit::Pixels};
    if (std::abs(to_pixels(m_px, 300.0) - 150.0) > 0.01) return false;

    // Millimeters at 300 DPI: 25.4mm = 1 inch = 300px.
    Measurement m_mm{25.4, MeasurementUnit::Millimeters};
    if (std::abs(to_pixels(m_mm, 300.0) - 300.0) > 0.01) return false;

    // Round-trip.
    auto back = from_pixels(300.0, 300.0, MeasurementUnit::Inches);
    if (std::abs(back.value - 1.0) > 0.01) return false;

    return true;
}

bool test_ops_subimage_detection() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Create a 200x100 BW1 image with a 50x30 block of foreground.
    Image bw(200, 100, PixelFormat::BW1, 300.0, 300.0);
    for (int y = 20; y < 50; ++y)
        for (int x = 40; x < 90; ++x)
            bw.set_bw_pixel(x, y, 1);

    SubimageConfig config;
    config.min_width_px = 5;
    config.min_height_px = 5;

    auto result = detect_subimage(bw, config, 300.0, 300.0);
    if (result.too_small) return false;
    if (result.bounds.left != 40 || result.bounds.top != 20) {
        std::cerr << "subimage bounds: left=" << result.bounds.left
                  << " top=" << result.bounds.top << std::endl;
        return false;
    }
    if (result.bounds.right != 90 || result.bounds.bottom != 50) {
        std::cerr << "subimage bounds: right=" << result.bounds.right
                  << " bottom=" << result.bounds.bottom << std::endl;
        return false;
    }

    return true;
}

bool test_ops_subimage_filters_small() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Image with one tiny speck and one real component.
    Image bw(100, 100, PixelFormat::BW1, 300.0, 300.0);
    // Speck: 2x2 at (5,5).
    bw.set_bw_pixel(5, 5, 1);
    bw.set_bw_pixel(6, 5, 1);
    bw.set_bw_pixel(5, 6, 1);
    bw.set_bw_pixel(6, 6, 1);
    // Real content: 30x30 at (50,50).
    for (int y = 50; y < 80; ++y)
        for (int x = 50; x < 80; ++x)
            bw.set_bw_pixel(x, y, 1);

    SubimageConfig config;
    config.min_width_px = 10;
    config.min_height_px = 10;

    auto result = detect_subimage(bw, config, 300.0, 300.0);
    // Should only detect the 30x30 block, not the speck.
    if (result.bounds.left != 50 || result.bounds.top != 50) return false;
    if (result.bounds.right != 80 || result.bounds.bottom != 80) return false;
    if (result.components.size() != 1) return false;

    return true;
}

bool test_ops_edge_cleanup() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Fill entire 100x100 BW1 image with foreground.
    Image bw(100, 100, PixelFormat::BW1, 300.0, 300.0);
    bw.fill(0xFF);

    // Clean 10px from each edge.
    EdgeValues edges;
    edges.top = {10.0, MeasurementUnit::Pixels};
    edges.bottom = {10.0, MeasurementUnit::Pixels};
    edges.left = {10.0, MeasurementUnit::Pixels};
    edges.right = {10.0, MeasurementUnit::Pixels};

    edge_cleanup(bw, edges, 300.0, 300.0);

    // Border pixels should be cleared.
    if (bw.get_bw_pixel(0, 0) != 0) return false;
    if (bw.get_bw_pixel(5, 5) != 0) return false;
    if (bw.get_bw_pixel(99, 99) != 0) return false;
    if (bw.get_bw_pixel(50, 0) != 0) return false;

    // Interior pixels should remain.
    if (bw.get_bw_pixel(50, 50) != 1) return false;
    if (bw.get_bw_pixel(10, 10) != 1) return false;
    if (bw.get_bw_pixel(89, 89) != 1) return false;

    return true;
}

bool test_ops_despeckle_single_pixel() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    Image bw(100, 100, PixelFormat::BW1, 300.0, 300.0);

    // Add single-pixel specks.
    bw.set_bw_pixel(10, 10, 1);
    bw.set_bw_pixel(50, 50, 1);

    // Add a real 5x5 block.
    for (int y = 70; y < 75; ++y)
        for (int x = 70; x < 75; ++x)
            bw.set_bw_pixel(x, y, 1);

    DespeckleConfig config;
    config.mode = DespeckleMode::SinglePixel;

    despeckle(bw, config);

    // Single pixels should be removed.
    if (bw.get_bw_pixel(10, 10) != 0) return false;
    if (bw.get_bw_pixel(50, 50) != 0) return false;

    // Block should remain.
    if (bw.get_bw_pixel(72, 72) != 1) return false;

    return true;
}

bool test_ops_despeckle_object() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    Image bw(100, 100, PixelFormat::BW1, 300.0, 300.0);

    // Small 3x3 object.
    for (int y = 10; y < 13; ++y)
        for (int x = 10; x < 13; ++x)
            bw.set_bw_pixel(x, y, 1);

    // Larger 20x20 object.
    for (int y = 50; y < 70; ++y)
        for (int x = 50; x < 70; ++x)
            bw.set_bw_pixel(x, y, 1);

    DespeckleConfig config;
    config.mode = DespeckleMode::Object;
    config.object_min = 1;
    config.object_max = 5;  // Remove objects up to 5px max dimension.

    despeckle(bw, config);

    // 3x3 object (max dim = 3) should be removed.
    if (bw.get_bw_pixel(11, 11) != 0) return false;

    // 20x20 object should remain.
    if (bw.get_bw_pixel(60, 60) != 1) return false;

    return true;
}

bool test_ops_canvas_resolution() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Letter at 300 DPI.
    CanvasConfig letter;
    letter.preset = CanvasPreset::Letter;
    letter.orientation = Orientation::Portrait;

    auto dims = resolve_canvas(letter, 300.0, 300.0, 0, 0);
    if (dims.width != 2550) { std::cerr << "letter width: " << dims.width << std::endl; return false; }
    if (dims.height != 3300) { std::cerr << "letter height: " << dims.height << std::endl; return false; }

    // Landscape.
    letter.orientation = Orientation::Landscape;
    dims = resolve_canvas(letter, 300.0, 300.0, 0, 0);
    if (dims.width != 3300 || dims.height != 2550) return false;

    // Autodetect.
    CanvasConfig autodet;
    autodet.preset = CanvasPreset::Autodetect;
    dims = resolve_canvas(autodet, 300.0, 300.0, 1000, 2000);
    if (dims.width != 1000 || dims.height != 2000) return false;

    // A4 at 300 DPI.
    CanvasConfig a4;
    a4.preset = CanvasPreset::A4;
    a4.orientation = Orientation::Portrait;
    dims = resolve_canvas(a4, 300.0, 300.0, 0, 0);
    // A4 = 210mm x 297mm = 8.267" x 11.692"
    // At 300 DPI: ~2480 x 3508
    if (dims.width < 2470 || dims.width > 2490) { std::cerr << "A4 width: " << dims.width << std::endl; return false; }
    if (dims.height < 3498 || dims.height > 3518) { std::cerr << "A4 height: " << dims.height << std::endl; return false; }

    return true;
}

bool test_ops_apply_margins() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // 50x30 content at (20,10) in a 100x80 source.
    Image src(100, 80, PixelFormat::Gray8, 300.0, 300.0);
    src.fill(0xFF);  // White background.
    for (int y = 10; y < 40; ++y)
        for (int x = 20; x < 70; ++x)
            src.row(y)[x] = 0;  // Black content.

    geometry::Rect subimage{20, 10, 70, 40};

    MarginConfig margins;
    margins.top.distance = {30.0, MeasurementUnit::Pixels};
    margins.left.distance = {25.0, MeasurementUnit::Pixels};

    // Place on a 200x150 canvas.
    auto result = apply_margins(src, subimage, margins, 200, 150,
                                false, 300.0, 300.0);

    if (result.image.width() != 200 || result.image.height() != 150) return false;

    // Content should be at (25, 30).
    if (result.content_rect.left != 25 || result.content_rect.top != 30) {
        std::cerr << "content at: " << result.content_rect.left
                  << "," << result.content_rect.top << std::endl;
        return false;
    }

    // Check that a content pixel is black.
    if (result.image.row(35)[30] != 0) return false;  // Inside content.
    // Check that border is white.
    if (result.image.row(0)[0] != 0xFF) return false;

    return true;
}

bool test_ops_apply_margins_centered() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // 20x20 content centered in 100x100 canvas.
    Image src(50, 50, PixelFormat::Gray8, 300.0, 300.0);
    src.fill(0xFF);
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 30; ++x)
            src.row(y)[x] = 0;

    geometry::Rect subimage{10, 10, 30, 30};

    MarginConfig margins;
    margins.center_horizontal = true;
    margins.center_vertical = true;

    auto result = apply_margins(src, subimage, margins, 100, 100,
                                false, 300.0, 300.0);

    // Content (20x20) centered in 100x100 → at (40, 40).
    if (result.content_rect.left != 40 || result.content_rect.top != 40) {
        std::cerr << "centered at: " << result.content_rect.left
                  << "," << result.content_rect.top << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Processing pipeline tests
// ---------------------------------------------------------------------------

bool test_pipeline_empty_image() {
    using namespace ppp::core;

    ProcessingProfile profile;
    Image empty;
    auto result = run_pipeline(empty, profile);
    if (result.success) return false;
    if (result.error != "empty source image") return false;

    return true;
}

bool test_pipeline_passthrough() {
    using namespace ppp::core;

    // Default profile with nothing enabled → image passes through unchanged.
    ProcessingProfile profile;
    profile.position_image = false;  // Skip margin positioning.

    Image img(100, 80, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0x80);

    auto result = run_pipeline(img, profile);
    if (!result.success) { std::cerr << "pipeline failed: " << result.error << std::endl; return false; }
    if (result.image.width() != 100 || result.image.height() != 80) return false;
    if (result.image.row(0)[0] != 0x80) return false;

    // Check that steps were logged.
    bool found_rotate = false;
    for (const auto& s : result.steps) {
        if (s.name == "rotate") found_rotate = true;
    }
    if (!found_rotate) return false;

    return true;
}

bool test_pipeline_rotation() {
    using namespace ppp::core;

    ProcessingProfile profile;
    profile.rotation = Rotation::CW90;
    profile.position_image = false;

    Image img(100, 50, PixelFormat::Gray8, 300.0, 300.0);

    auto result = run_pipeline(img, profile);
    if (!result.success) return false;
    // CW90: 100x50 → 50x100.
    if (result.image.width() != 50 || result.image.height() != 100) return false;

    // Check step was logged as applied.
    bool rotate_applied = false;
    for (const auto& s : result.steps) {
        if (s.name == "rotate" && s.applied) rotate_applied = true;
    }
    if (!rotate_applied) return false;

    return true;
}

bool test_pipeline_edge_cleanup_and_despeckle() {
    using namespace ppp::core;

    // Create BW1 image filled with foreground.
    Image img(100, 100, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0xFF);

    ProcessingProfile profile;
    profile.position_image = false;

    // Enable edge cleanup (10px from each edge, before deskew).
    profile.edge_cleanup.enabled = true;
    profile.edge_cleanup.order = EdgeCleanupOrder::BeforeDeskew;
    profile.edge_cleanup.set1.top = {10.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set1.bottom = {10.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set1.left = {10.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set1.right = {10.0, MeasurementUnit::Pixels};

    // Enable single-pixel despeckle.
    profile.despeckle.mode = DespeckleMode::SinglePixel;

    auto result = run_pipeline(img, profile);
    if (!result.success) return false;

    // Border should be cleared.
    if (result.image.get_bw_pixel(0, 0) != 0) return false;
    if (result.image.get_bw_pixel(5, 5) != 0) return false;
    // Interior should remain.
    if (result.image.get_bw_pixel(50, 50) != 1) return false;

    // Check steps were applied.
    int applied_count = 0;
    for (const auto& s : result.steps) {
        if (s.applied) ++applied_count;
    }
    if (applied_count < 2) return false;  // At least edge_cleanup and despeckle.

    return true;
}

bool test_pipeline_full_with_margins() {
    using namespace ppp::core;

    // Create a BW1 image with a block of content.
    Image img(200, 150, PixelFormat::BW1, 300.0, 300.0);
    for (int y = 30; y < 100; ++y)
        for (int x = 40; x < 160; ++x)
            img.set_bw_pixel(x, y, 1);

    ProcessingProfile profile;
    profile.position_image = true;
    profile.canvas.preset = CanvasPreset::Custom;
    profile.canvas.width = {300.0, MeasurementUnit::Pixels};
    profile.canvas.height = {200.0, MeasurementUnit::Pixels};
    profile.canvas.orientation = Orientation::Landscape;
    profile.margins[0].center_horizontal = true;
    profile.margins[0].center_vertical = true;

    auto result = run_pipeline(img, profile);
    if (!result.success) { std::cerr << "pipeline failed: " << result.error << std::endl; return false; }

    // Output should be 300x200 (the custom canvas).
    if (result.image.width() != 300 || result.image.height() != 200) {
        std::cerr << "output size: " << result.image.width() << "x" << result.image.height() << std::endl;
        return false;
    }

    // Subimage should have been detected.
    if (result.subimage_bounds.empty()) return false;

    // Check that margins step was applied.
    bool margins_applied = false;
    for (const auto& s : result.steps) {
        if (s.name == "margins" && s.applied) margins_applied = true;
    }
    if (!margins_applied) return false;

    return true;
}

bool test_pipeline_odd_even_pages() {
    using namespace ppp::core;

    Image img(100, 100, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0xFF);  // All foreground.

    ProcessingProfile profile;
    profile.position_image = false;
    profile.odd_even_mode = true;

    // Set 1 (odd pages): clean 5px from edges.
    profile.edge_cleanup.enabled = true;
    profile.edge_cleanup.set1.top = {5.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set1.bottom = {5.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set1.left = {5.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set1.right = {5.0, MeasurementUnit::Pixels};

    // Set 2 (even pages): clean 20px from edges.
    profile.edge_cleanup.set2.top = {20.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set2.bottom = {20.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set2.left = {20.0, MeasurementUnit::Pixels};
    profile.edge_cleanup.set2.right = {20.0, MeasurementUnit::Pixels};

    // Page 0 (odd) — 5px cleanup.
    auto r0 = run_pipeline(img, profile, 0);
    if (!r0.success) return false;
    if (r0.image.get_bw_pixel(3, 3) != 0) return false;    // Inside 5px border.
    if (r0.image.get_bw_pixel(10, 10) != 1) return false;   // Outside 5px border.

    // Page 1 (even) — 20px cleanup.
    Image img2(100, 100, PixelFormat::BW1, 300.0, 300.0);
    img2.fill(0xFF);
    auto r1 = run_pipeline(img2, profile, 1);
    if (!r1.success) return false;
    if (r1.image.get_bw_pixel(15, 15) != 0) return false;   // Inside 20px border.
    if (r1.image.get_bw_pixel(50, 50) != 1) return false;   // Outside 20px border.

    return true;
}

bool test_pipeline_run_step() {
    using namespace ppp::core;

    Image img(100, 50, PixelFormat::Gray8, 300.0, 300.0);

    ProcessingProfile profile;
    profile.rotation = Rotation::R180;

    auto result = run_step(img, profile, "rotate");
    if (!result.success) return false;
    if (result.image.width() != 100 || result.image.height() != 50) return false;
    if (result.steps.size() != 1) return false;
    if (result.steps[0].name != "rotate" || !result.steps[0].applied) return false;

    // Unknown step.
    auto bad = run_step(img, profile, "nonexistent");
    if (bad.success) return false;

    return true;
}

bool test_pipeline_step_log_detail() {
    using namespace ppp::core;

    Image img(100, 100, PixelFormat::BW1, 300.0, 300.0);
    for (int y = 30; y < 70; ++y)
        for (int x = 20; x < 80; ++x)
            img.set_bw_pixel(x, y, 1);

    ProcessingProfile profile;
    profile.position_image = false;
    profile.despeckle.mode = DespeckleMode::Object;
    profile.despeckle.object_min = 1;
    profile.despeckle.object_max = 3;

    auto result = run_pipeline(img, profile);
    if (!result.success) return false;

    // Check that step details contain useful text.
    for (const auto& s : result.steps) {
        if (s.name == "detect_subimage") {
            if (s.detail.find("content at") == std::string::npos) {
                std::cerr << "detect_subimage detail: " << s.detail << std::endl;
                return false;
            }
        }
        if (s.name == "despeckle" && s.applied) {
            if (s.detail.find("removed objects") == std::string::npos) {
                std::cerr << "despeckle detail: " << s.detail << std::endl;
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// TIFF writer tests
// ---------------------------------------------------------------------------

bool test_tiff_write_gray8_uncompressed() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    // Create a 10x10 gray image with a gradient.
    Image img(10, 10, PixelFormat::Gray8, 300.0, 300.0);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 10; ++x)
            img.row(y)[x] = static_cast<std::uint8_t>(y * 25);

    auto data = write_tiff_to_memory(img);
    if (data.empty()) return false;

    // Verify it's a valid TIFF by parsing it.
    auto structure = Structure::read(data);
    if (!structure) { std::cerr << "failed to parse written TIFF" << std::endl; return false; }
    if (structure->page_count() != 1) return false;

    auto w = structure->image_width();
    auto h = structure->image_length();
    if (!w || *w != 10) return false;
    if (!h || *h != 10) return false;

    auto xres = structure->x_resolution();
    if (!xres || std::abs(*xres - 300.0) > 0.01) return false;

    return true;
}

bool test_tiff_write_bw1_uncompressed() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image img(32, 16, PixelFormat::BW1, 600.0, 600.0);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 16; ++x)
            img.set_bw_pixel(x, y, 1);

    auto data = write_tiff_to_memory(img);
    if (data.empty()) return false;

    auto structure = Structure::read(data);
    if (!structure) return false;
    if (*structure->image_width() != 32) return false;
    if (*structure->bits_per_sample() != 1) return false;

    return true;
}

bool test_tiff_write_rgb24_uncompressed() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image img(8, 8, PixelFormat::RGB24, 150.0, 150.0);
    for (int y = 0; y < 8; ++y) {
        auto* row = img.row(y);
        for (int x = 0; x < 8; ++x) {
            row[x * 3] = 255;      // Red.
            row[x * 3 + 1] = 0;
            row[x * 3 + 2] = 0;
        }
    }

    auto data = write_tiff_to_memory(img);
    if (data.empty()) return false;

    auto structure = Structure::read(data);
    if (!structure) return false;
    if (structure->photometric() != Photometric::RGB) return false;

    return true;
}

bool test_tiff_write_packbits() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    // Create an image with lots of repeated data (good for PackBits).
    Image img(100, 100, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0x80);

    WriteOptions opts;
    opts.compression = Compression::PackBits;

    auto data = write_tiff_to_memory(img, opts);
    if (data.empty()) return false;

    // PackBits should compress repeated data significantly.
    auto uncompressed = write_tiff_to_memory(img);
    if (data.size() >= uncompressed.size()) {
        std::cerr << "PackBits didn't compress: " << data.size()
                  << " vs " << uncompressed.size() << std::endl;
        return false;
    }

    // Verify header is still valid TIFF.
    auto structure = Structure::read(data);
    if (!structure) return false;
    if (structure->compression() != Compression::PackBits) return false;

    return true;
}

bool test_tiff_write_lzw() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image img(50, 50, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0x42);

    WriteOptions opts;
    opts.compression = Compression::LZW;

    auto data = write_tiff_to_memory(img, opts);
    if (data.empty()) return false;

    auto structure = Structure::read(data);
    if (!structure) return false;
    if (structure->compression() != Compression::LZW) return false;

    return true;
}

bool test_tiff_roundtrip_gray8() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    // Create image with specific pixel values.
    Image orig(20, 15, PixelFormat::Gray8, 300.0, 300.0);
    for (int y = 0; y < 15; ++y)
        for (int x = 0; x < 20; ++x)
            orig.row(y)[x] = static_cast<std::uint8_t>((x + y * 20) & 0xFF);

    // Write to memory and read back.
    auto data = write_tiff_to_memory(orig);
    auto loaded = read_tiff_image(data.data(), data.size());

    if (loaded.empty()) { std::cerr << "failed to read back TIFF" << std::endl; return false; }
    if (loaded.width() != 20 || loaded.height() != 15) return false;
    if (loaded.format() != PixelFormat::Gray8) return false;

    // Verify pixel values.
    for (int y = 0; y < 15; ++y)
        for (int x = 0; x < 20; ++x)
            if (loaded.row(y)[x] != orig.row(y)[x]) {
                std::cerr << "pixel mismatch at (" << x << "," << y << ")" << std::endl;
                return false;
            }

    return true;
}

bool test_tiff_roundtrip_bw1() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image orig(24, 10, PixelFormat::BW1, 600.0, 600.0);
    // Set a pattern.
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 24; ++x)
            if ((x + y) % 3 == 0) orig.set_bw_pixel(x, y, 1);

    auto data = write_tiff_to_memory(orig);
    auto loaded = read_tiff_image(data.data(), data.size());

    if (loaded.empty()) return false;
    if (loaded.width() != 24 || loaded.height() != 10) return false;
    if (loaded.format() != PixelFormat::BW1) return false;

    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 24; ++x)
            if (loaded.get_bw_pixel(x, y) != orig.get_bw_pixel(x, y)) {
                std::cerr << "bw pixel mismatch at (" << x << "," << y << ")" << std::endl;
                return false;
            }

    return true;
}

bool test_tiff_roundtrip_rgb24() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image orig(8, 6, PixelFormat::RGB24, 72.0, 72.0);
    for (int y = 0; y < 6; ++y) {
        auto* row = orig.row(y);
        for (int x = 0; x < 8; ++x) {
            row[x * 3] = static_cast<std::uint8_t>(x * 30);
            row[x * 3 + 1] = static_cast<std::uint8_t>(y * 40);
            row[x * 3 + 2] = static_cast<std::uint8_t>((x + y) * 20);
        }
    }

    auto data = write_tiff_to_memory(orig);
    auto loaded = read_tiff_image(data.data(), data.size());

    if (loaded.empty()) return false;
    if (loaded.format() != PixelFormat::RGB24) return false;

    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x)
            for (int c = 0; c < 3; ++c)
                if (loaded.row(y)[x * 3 + c] != orig.row(y)[x * 3 + c]) return false;

    return true;
}

bool test_tiff_file_roundtrip() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image orig(30, 20, PixelFormat::Gray8, 300.0, 300.0);
    orig.fill(0x55);

    auto path = fs::temp_directory_path() / "ppp_test_tiff_roundtrip.tif";

    if (!write_tiff(orig, path)) { std::cerr << "write_tiff failed" << std::endl; return false; }

    auto loaded = read_tiff_image_file(path);
    if (loaded.empty()) { std::cerr << "read_tiff_image_file failed" << std::endl; return false; }
    if (loaded.width() != 30 || loaded.height() != 20) return false;
    if (loaded.row(0)[0] != 0x55) return false;

    fs::remove(path);
    return true;
}

bool test_tiff_multipage() {
    using namespace ppp::core;
    using namespace ppp::core::tiff;

    Image page1(20, 10, PixelFormat::Gray8, 300.0, 300.0);
    page1.fill(0x11);
    Image page2(30, 15, PixelFormat::Gray8, 300.0, 300.0);
    page2.fill(0x22);

    auto path = fs::temp_directory_path() / "ppp_test_multipage.tif";

    if (!write_multipage_tiff({page1, page2}, path)) {
        std::cerr << "write_multipage_tiff failed" << std::endl;
        return false;
    }

    // Read back and verify both pages via Structure.
    auto file_data = read_tiff_file(path.string());
    if (!file_data) { std::cerr << "read_tiff_file failed" << std::endl; return false; }

    if (file_data->page_count() != 2) {
        std::cerr << "expected 2 pages, got " << file_data->page_count() << std::endl;
        return false;
    }

    auto w1 = file_data->page(0).get_int(Tag::ImageWidth);
    auto w2 = file_data->page(1).get_int(Tag::ImageWidth);
    if (!w1 || *w1 != 20) return false;
    if (!w2 || *w2 != 30) return false;

    fs::remove(path);
    return true;
}

// ---------------------------------------------------------------------------
// Resize / scaling tests
// ---------------------------------------------------------------------------

bool test_scale_nearest_gray8() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Create 4x4 with quadrant pattern.
    Image img(4, 4, PixelFormat::Gray8, 300.0, 300.0);
    for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) img.row(y)[x] = 0;
    for (int y = 0; y < 2; ++y) for (int x = 2; x < 4; ++x) img.row(y)[x] = 85;
    for (int y = 2; y < 4; ++y) for (int x = 0; x < 2; ++x) img.row(y)[x] = 170;
    for (int y = 2; y < 4; ++y) for (int x = 2; x < 4; ++x) img.row(y)[x] = 255;

    // Scale up 2x.
    auto up = scale_nearest(img, 8, 8);
    if (up.width() != 8 || up.height() != 8) return false;
    if (up.row(0)[0] != 0) return false;
    if (up.row(0)[4] != 85) return false;
    if (up.row(4)[0] != 170) return false;
    if (up.row(4)[4] != 255) return false;

    // Scale down 2x.
    auto down = scale_nearest(img, 2, 2);
    if (down.width() != 2 || down.height() != 2) return false;

    return true;
}

bool test_scale_bilinear_gray8() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Gradient: left=0, right=200.
    Image img(4, 1, PixelFormat::Gray8, 300.0, 300.0);
    img.row(0)[0] = 0;
    img.row(0)[1] = 66;
    img.row(0)[2] = 133;
    img.row(0)[3] = 200;

    auto scaled = scale_bilinear(img, 8, 1);
    if (scaled.width() != 8) return false;
    // Should have interpolated values between.
    if (scaled.row(0)[0] != 0) return false;
    if (scaled.row(0)[7] != 200) return false;
    // Middle values should be smooth.
    if (scaled.row(0)[3] < 50 || scaled.row(0)[3] > 150) return false;

    return true;
}

bool test_scale_bw1() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    Image img(8, 8, PixelFormat::BW1, 300.0, 300.0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            img.set_bw_pixel(x, y, 1);

    // Scale up 2x.
    auto up = scale_nearest(img, 16, 16);
    if (up.width() != 16 || up.height() != 16) return false;
    if (up.get_bw_pixel(0, 0) != 1) return false;
    if (up.get_bw_pixel(7, 7) != 1) return false;
    if (up.get_bw_pixel(8, 8) != 0) return false;

    // Bilinear on BW1 should fall back to nearest.
    auto bil = scale_bilinear(img, 16, 16);
    if (bil.width() != 16) return false;

    return true;
}

bool test_resize_basic() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // 100x80 image with 50x40 content block.
    Image img(100, 80, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0xFF);
    for (int y = 10; y < 50; ++y)
        for (int x = 20; x < 70; ++x)
            img.row(y)[x] = 0;

    geometry::Rect subimage{20, 10, 70, 50};

    ResizeConfig config;
    config.enabled = true;
    config.source = ResizeFrom::Subimage;
    config.canvas.preset = CanvasPreset::Custom;
    config.canvas.width = {200.0, MeasurementUnit::Pixels};
    config.canvas.height = {150.0, MeasurementUnit::Pixels};
    config.canvas.orientation = Orientation::Landscape;
    config.v_alignment = VAlignment::Center;
    config.h_alignment = HAlignment::Center;

    auto result = apply_resize(img, subimage, config);
    if (result.image.empty()) {
        std::cerr << "resize produced empty image" << std::endl;
        // Debug: check canvas resolution.
        auto canvas = ppp::core::ops::resolve_canvas(config.canvas, 300.0, 300.0, 100, 80);
        std::cerr << "canvas: " << canvas.width << "x" << canvas.height << std::endl;
        return false;
    }
    if (result.image.width() != 200 || result.image.height() != 150) {
        std::cerr << "resize output: " << result.image.width() << "x" << result.image.height() << std::endl;
        return false;
    }

    // Content should be centered (or at least within the canvas).
    if (result.content_rect.empty()) return false;

    return true;
}

bool test_resize_no_enlarge() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    // Small 20x20 image.
    Image img(20, 20, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0x80);

    geometry::Rect subimage{0, 0, 20, 20};

    ResizeConfig config;
    config.enabled = true;
    config.source = ResizeFrom::FullPage;
    config.canvas.preset = CanvasPreset::Custom;
    config.canvas.width = {200.0, MeasurementUnit::Pixels};
    config.canvas.height = {200.0, MeasurementUnit::Pixels};
    config.allow_enlarge = false;
    config.allow_shrink = true;
    config.v_alignment = VAlignment::Top;
    config.h_alignment = HAlignment::Center;

    auto result = apply_resize(img, subimage, config);
    if (result.image.empty()) return false;

    // Content should NOT be enlarged — stays at 20x20.
    if (result.content_rect.width() != 20 || result.content_rect.height() != 20) {
        std::cerr << "content size: " << result.content_rect.width() << "x"
                  << result.content_rect.height() << std::endl;
        return false;
    }

    return true;
}

bool test_resize_alignment() {
    using namespace ppp::core;
    using namespace ppp::core::ops;

    Image img(50, 50, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0x00);

    geometry::Rect subimage{0, 0, 50, 50};

    ResizeConfig config;
    config.enabled = true;
    config.source = ResizeFrom::FullPage;
    config.canvas.preset = CanvasPreset::Custom;
    config.canvas.width = {100.0, MeasurementUnit::Pixels};
    config.canvas.height = {100.0, MeasurementUnit::Pixels};
    config.allow_enlarge = false;

    // Top alignment.
    config.v_alignment = VAlignment::Top;
    config.h_alignment = HAlignment::Center;
    auto r = apply_resize(img, subimage, config);
    if (r.content_rect.top != 0) return false;
    if (r.content_rect.left != 25) return false;

    // Bottom alignment.
    config.v_alignment = VAlignment::Bottom;
    r = apply_resize(img, subimage, config);
    if (r.content_rect.top != 50) return false;

    // Center.
    config.v_alignment = VAlignment::Center;
    r = apply_resize(img, subimage, config);
    if (r.content_rect.top != 25) return false;

    return true;
}

bool test_pipeline_with_resize() {
    using namespace ppp::core;

    // Image with content block.
    Image img(200, 150, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0xFF);
    for (int y = 20; y < 100; ++y)
        for (int x = 30; x < 170; ++x)
            img.row(y)[x] = 0;

    ProcessingProfile profile;
    profile.position_image = false;  // Skip main margin step.
    profile.resize.enabled = true;
    profile.resize.source = ResizeFrom::FullPage;
    profile.resize.canvas.preset = CanvasPreset::Custom;
    profile.resize.canvas.width = {100.0, MeasurementUnit::Pixels};
    profile.resize.canvas.height = {75.0, MeasurementUnit::Pixels};
    profile.resize.canvas.orientation = Orientation::Landscape;
    profile.resize.v_alignment = VAlignment::Center;
    profile.resize.h_alignment = HAlignment::Center;

    auto result = run_pipeline(img, profile);
    if (!result.success) { std::cerr << "pipeline failed: " << result.error << std::endl; return false; }

    // Should have been resized.
    if (result.image.width() != 100 || result.image.height() != 75) {
        std::cerr << "pipeline resize output: " << result.image.width()
                  << "x" << result.image.height() << std::endl;
        return false;
    }

    // Check resize step was applied.
    bool resize_applied = false;
    for (const auto& s : result.steps) {
        if (s.name == "resize" && s.applied) resize_applied = true;
    }
    if (!resize_applied) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Histogram and auto-threshold tests
// ---------------------------------------------------------------------------

bool test_histogram_gray8() {
    using namespace ppp::core;

    // Uniform gray image — all pixels at value 128.
    Image img(100, 100, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(128);

    auto hist = ops::compute_histogram(img);
    if (hist.total_pixels != 10000) {
        std::cerr << "hist gray8: wrong total " << hist.total_pixels << std::endl;
        return false;
    }
    if (hist.bins[128] != 10000) {
        std::cerr << "hist gray8: bin[128]=" << hist.bins[128] << std::endl;
        return false;
    }
    // All other bins should be zero.
    for (int i = 0; i < 256; ++i) {
        if (i != 128 && hist.bins[i] != 0) {
            std::cerr << "hist gray8: bin[" << i << "]=" << hist.bins[i] << std::endl;
            return false;
        }
    }

    // Check statistics.
    if (std::abs(hist.mean() - 128.0) > 0.01) {
        std::cerr << "hist gray8: mean=" << hist.mean() << std::endl;
        return false;
    }
    if (hist.median() != 128) {
        std::cerr << "hist gray8: median=" << (int)hist.median() << std::endl;
        return false;
    }
    if (hist.min_value() != 128 || hist.max_value() != 128) {
        std::cerr << "hist gray8: min/max wrong" << std::endl;
        return false;
    }

    return true;
}

bool test_histogram_bimodal() {
    using namespace ppp::core;

    // Half black (0), half white (255).
    Image img(100, 100, PixelFormat::Gray8, 300.0, 300.0);
    for (int y = 0; y < 100; ++y) {
        auto* row = img.row(y);
        for (int x = 0; x < 100; ++x) {
            row[x] = (y < 50) ? static_cast<std::uint8_t>(0) : static_cast<std::uint8_t>(255);
        }
    }

    auto hist = ops::compute_histogram(img);
    if (hist.bins[0] != 5000 || hist.bins[255] != 5000) {
        std::cerr << "hist bimodal: bins[0]=" << hist.bins[0]
                  << " bins[255]=" << hist.bins[255] << std::endl;
        return false;
    }

    return true;
}

bool test_histogram_rgb24() {
    using namespace ppp::core;

    // Pure red image — luminance = 0.299*255 ≈ 76.
    Image img(50, 50, PixelFormat::RGB24, 300.0, 300.0);
    for (int y = 0; y < 50; ++y) {
        auto* row = img.row(y);
        for (int x = 0; x < 50; ++x) {
            row[x * 3 + 0] = 255;  // R.
            row[x * 3 + 1] = 0;    // G.
            row[x * 3 + 2] = 0;    // B.
        }
    }

    auto hist = ops::compute_histogram(img);
    // Luminance ≈ 76.245 → bin 76.
    auto lum = static_cast<int>(0.299 * 255 + 0.5);
    if (hist.bins[lum] != 2500) {
        std::cerr << "hist rgb24: expected bin[" << lum << "]=2500, got "
                  << hist.bins[lum] << std::endl;
        return false;
    }

    return true;
}

bool test_histogram_bw1() {
    using namespace ppp::core;

    Image img(80, 60, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);  // All white.
    // Set some foreground pixels.
    for (int x = 0; x < 40; ++x)
        img.set_bw_pixel(x, 0, 1);

    auto hist = ops::compute_histogram(img);
    // 40 foreground (bin 0), rest background (bin 255).
    if (hist.bins[0] != 40) {
        std::cerr << "hist bw1: bins[0]=" << hist.bins[0] << " (expected 40)" << std::endl;
        return false;
    }
    if (hist.bins[255] != 80 * 60 - 40) {
        std::cerr << "hist bw1: bins[255]=" << hist.bins[255] << std::endl;
        return false;
    }

    return true;
}

bool test_otsu_threshold() {
    using namespace ppp::core;

    // Create a bimodal histogram with peaks at 50 and 200.
    ops::Histogram hist;
    for (int i = 40; i <= 60; ++i) hist.bins[i] = 100;
    for (int i = 190; i <= 210; ++i) hist.bins[i] = 100;
    hist.total_pixels = 2100 + 2100;
    // Correct total.
    hist.total_pixels = 0;
    for (int i = 0; i < 256; ++i) hist.total_pixels += hist.bins[i];

    auto t = ops::otsu_threshold(hist);
    // Otsu threshold should be between the two peaks.
    if (t < 60 || t > 190) {
        std::cerr << "otsu: threshold=" << (int)t << " (expected 60..190)" << std::endl;
        return false;
    }

    return true;
}

bool test_binarize_gray8() {
    using namespace ppp::core;

    Image img(40, 30, PixelFormat::Gray8, 300.0, 300.0);
    // Left half dark (50), right half light (200).
    for (int y = 0; y < 30; ++y) {
        auto* row = img.row(y);
        for (int x = 0; x < 40; ++x) {
            row[x] = (x < 20) ? static_cast<std::uint8_t>(50) : static_cast<std::uint8_t>(200);
        }
    }

    auto bw = ops::binarize(img, 128);
    if (bw.format() != PixelFormat::BW1) {
        std::cerr << "binarize: wrong format" << std::endl;
        return false;
    }
    if (bw.width() != 40 || bw.height() != 30) {
        std::cerr << "binarize: wrong size" << std::endl;
        return false;
    }

    // Left half should be foreground (1), right half background (0).
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) {
            int expected = (x < 20) ? 1 : 0;
            if (bw.get_bw_pixel(x, y) != expected) {
                std::cerr << "binarize: pixel (" << x << "," << y << ") = "
                          << bw.get_bw_pixel(x, y) << " expected " << expected << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool test_binarize_otsu() {
    using namespace ppp::core;

    // Bimodal image: top half dark, bottom half light.
    Image img(60, 40, PixelFormat::Gray8, 300.0, 300.0);
    for (int y = 0; y < 40; ++y) {
        auto* row = img.row(y);
        for (int x = 0; x < 60; ++x) {
            row[x] = (y < 20) ? static_cast<std::uint8_t>(30) : static_cast<std::uint8_t>(220);
        }
    }

    auto bw = ops::binarize_otsu(img);
    if (bw.format() != PixelFormat::BW1) {
        std::cerr << "binarize_otsu: wrong format" << std::endl;
        return false;
    }

    // Top half should be foreground, bottom background.
    if (bw.get_bw_pixel(30, 5) != 1) {
        std::cerr << "binarize_otsu: dark region should be foreground" << std::endl;
        return false;
    }
    if (bw.get_bw_pixel(30, 35) != 0) {
        std::cerr << "binarize_otsu: light region should be background" << std::endl;
        return false;
    }

    return true;
}

bool test_binarize_already_bw1() {
    using namespace ppp::core;

    Image img(50, 40, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    img.set_bw_pixel(10, 10, 1);

    auto bw = ops::binarize(img, 128);
    if (bw.format() != PixelFormat::BW1) return false;
    if (bw.get_bw_pixel(10, 10) != 1) return false;

    auto bw2 = ops::binarize_otsu(img);
    if (bw2.get_bw_pixel(10, 10) != 1) return false;

    return true;
}

// ---------------------------------------------------------------------------
// Morphological operations tests
// ---------------------------------------------------------------------------

bool test_morph_dilate() {
    using namespace ppp::core;

    Image img(20, 20, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    img.set_bw_pixel(10, 10, 1);  // Single pixel.

    ops::dilate(img, ops::StructuringElement::Cross);

    // Should expand to 4-neighbors.
    if (!img.get_bw_pixel(10, 10)) return false;
    if (!img.get_bw_pixel(9, 10)) return false;
    if (!img.get_bw_pixel(11, 10)) return false;
    if (!img.get_bw_pixel(10, 9)) return false;
    if (!img.get_bw_pixel(10, 11)) return false;
    // Diagonal should NOT be set for Cross.
    if (img.get_bw_pixel(9, 9)) {
        std::cerr << "dilate cross: diagonal should not be set" << std::endl;
        return false;
    }

    return true;
}

bool test_morph_dilate_square() {
    using namespace ppp::core;

    Image img(20, 20, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    img.set_bw_pixel(10, 10, 1);

    ops::dilate(img, ops::StructuringElement::Square);

    // All 8-neighbors should be set.
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (!img.get_bw_pixel(10 + dx, 10 + dy)) {
                std::cerr << "dilate square: (" << 10+dx << "," << 10+dy << ") not set" << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool test_morph_erode() {
    using namespace ppp::core;

    // 3x3 block — after cross erosion, only center should remain.
    Image img(20, 20, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    for (int y = 9; y <= 11; ++y)
        for (int x = 9; x <= 11; ++x)
            img.set_bw_pixel(x, y, 1);

    ops::erode(img, ops::StructuringElement::Cross);

    if (!img.get_bw_pixel(10, 10)) {
        std::cerr << "erode: center should survive" << std::endl;
        return false;
    }
    // Edge pixels of the block should be eroded.
    if (img.get_bw_pixel(9, 9)) {
        std::cerr << "erode: corner should be removed" << std::endl;
        return false;
    }

    return true;
}

bool test_morph_open_removes_noise() {
    using namespace ppp::core;

    Image img(30, 30, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    // Large block (survives opening).
    for (int y = 5; y < 25; ++y)
        for (int x = 5; x < 25; ++x)
            img.set_bw_pixel(x, y, 1);

    // Single-pixel noise.
    img.set_bw_pixel(2, 2, 1);

    auto result = ops::morph_open(img, ops::StructuringElement::Cross);

    // Noise should be removed.
    if (result.get_bw_pixel(2, 2)) {
        std::cerr << "morph_open: noise should be removed" << std::endl;
        return false;
    }
    // Block interior should survive.
    if (!result.get_bw_pixel(15, 15)) {
        std::cerr << "morph_open: block interior should survive" << std::endl;
        return false;
    }

    return true;
}

bool test_morph_close_fills_holes() {
    using namespace ppp::core;

    Image img(30, 30, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    // Solid block with a single-pixel hole.
    for (int y = 5; y < 25; ++y)
        for (int x = 5; x < 25; ++x)
            img.set_bw_pixel(x, y, 1);
    img.set_bw_pixel(15, 15, 0);  // Hole.

    auto result = ops::morph_close(img, ops::StructuringElement::Cross);

    // Hole should be filled.
    if (!result.get_bw_pixel(15, 15)) {
        std::cerr << "morph_close: hole should be filled" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Color dropout tests
// ---------------------------------------------------------------------------

bool test_color_dropout_red() {
    using namespace ppp::core;

    // Create 4x1 RGB image: red, green, blue, black pixels.
    Image img(4, 1, PixelFormat::RGB24, 300.0, 300.0);
    auto* row = img.row(0);
    // Pixel 0: pure red (255,0,0) — should be dropped.
    row[0] = 255; row[1] = 0; row[2] = 0;
    // Pixel 1: pure green (0,255,0) — should stay.
    row[3] = 0; row[4] = 255; row[5] = 0;
    // Pixel 2: pure blue (0,0,255) — should stay.
    row[6] = 0; row[7] = 0; row[8] = 255;
    // Pixel 3: black (0,0,0) — should stay.
    row[9] = 0; row[10] = 0; row[11] = 0;

    ColorDropoutConfig config;
    config.enabled = true;
    config.color = DropoutColor::Red;
    config.threshold = 30;

    auto result = ops::color_dropout(img, config);
    auto* rrow = result.row(0);

    // Red pixel should become white.
    if (rrow[0] != 255 || rrow[1] != 255 || rrow[2] != 255) {
        std::cerr << "color_dropout: red pixel not dropped" << std::endl;
        return false;
    }
    // Green pixel should be unchanged.
    if (rrow[3] != 0 || rrow[4] != 255 || rrow[5] != 0) {
        std::cerr << "color_dropout: green pixel was modified" << std::endl;
        return false;
    }
    // Black pixel should be unchanged.
    if (rrow[9] != 0 || rrow[10] != 0 || rrow[11] != 0) {
        std::cerr << "color_dropout: black pixel was modified" << std::endl;
        return false;
    }

    return true;
}

bool test_color_dropout_green() {
    using namespace ppp::core;

    Image img(3, 1, PixelFormat::RGB24, 300.0, 300.0);
    auto* row = img.row(0);
    // Pixel 0: red.
    row[0] = 255; row[1] = 0; row[2] = 0;
    // Pixel 1: green.
    row[3] = 0; row[4] = 255; row[5] = 0;
    // Pixel 2: blue.
    row[6] = 0; row[7] = 0; row[8] = 255;

    ColorDropoutConfig config;
    config.enabled = true;
    config.color = DropoutColor::Green;
    config.threshold = 30;

    auto result = ops::color_dropout(img, config);
    auto* rrow = result.row(0);

    // Red pixel should be unchanged.
    if (rrow[0] != 255 || rrow[1] != 0 || rrow[2] != 0) {
        std::cerr << "color_dropout green: red pixel was modified" << std::endl;
        return false;
    }
    // Green pixel should become white.
    if (rrow[3] != 255 || rrow[4] != 255 || rrow[5] != 255) {
        std::cerr << "color_dropout green: green pixel not dropped" << std::endl;
        return false;
    }
    // Blue pixel should be unchanged.
    if (rrow[6] != 0 || rrow[7] != 0 || rrow[8] != 255) {
        std::cerr << "color_dropout green: blue pixel was modified" << std::endl;
        return false;
    }

    return true;
}

bool test_color_dropout_disabled() {
    using namespace ppp::core;

    Image img(1, 1, PixelFormat::RGB24, 300.0, 300.0);
    auto* row = img.row(0);
    row[0] = 255; row[1] = 0; row[2] = 0;  // Pure red.

    ColorDropoutConfig config;
    config.enabled = false;
    config.color = DropoutColor::Red;
    config.threshold = 30;

    auto result = ops::color_dropout(img, config);
    auto* rrow = result.row(0);

    // Disabled — should be unchanged.
    if (rrow[0] != 255 || rrow[1] != 0 || rrow[2] != 0) {
        std::cerr << "color_dropout disabled: pixel was modified" << std::endl;
        return false;
    }

    return true;
}

bool test_color_dropout_bw_passthrough() {
    using namespace ppp::core;

    Image img(10, 10, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    ColorDropoutConfig config;
    config.enabled = true;
    config.color = DropoutColor::Red;
    config.threshold = 30;

    auto result = ops::color_dropout(img, config);

    // BW1 image should pass through unchanged.
    if (result.format() != PixelFormat::BW1 || result.width() != 10 || result.height() != 10) {
        std::cerr << "color_dropout bw: format/size changed" << std::endl;
        return false;
    }

    return true;
}

bool test_color_dropout_config_json() {
    using namespace ppp::core;

    ProcessingProfile profile;
    profile.color_dropout.enabled = true;
    profile.color_dropout.color = DropoutColor::Blue;
    profile.color_dropout.threshold = 42;

    auto json = processing_profile_to_json(profile);
    auto parsed = processing_profile_from_json(json);

    if (!parsed) {
        std::cerr << "color_dropout config: JSON round-trip parse failed" << std::endl;
        return false;
    }
    if (!parsed->color_dropout.enabled) {
        std::cerr << "color_dropout config: enabled lost" << std::endl;
        return false;
    }
    if (parsed->color_dropout.color != DropoutColor::Blue) {
        std::cerr << "color_dropout config: color lost" << std::endl;
        return false;
    }
    if (parsed->color_dropout.threshold != 42) {
        std::cerr << "color_dropout config: threshold lost" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Blank page detection tests
// ---------------------------------------------------------------------------

bool test_blank_page_empty_image() {
    using namespace ppp::core;

    Image img;  // Empty.
    BlankPageConfig config;
    config.enabled = true;
    config.threshold_percent = 0.5;

    auto result = ops::detect_blank_page(img, config, 300.0, 300.0);
    if (!result.is_blank) {
        std::cerr << "blank page: empty image should be blank" << std::endl;
        return false;
    }

    return true;
}

bool test_blank_page_white_image() {
    using namespace ppp::core;

    Image img(200, 150, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);  // All white.

    BlankPageConfig config;
    config.enabled = true;
    config.threshold_percent = 0.5;

    auto result = ops::detect_blank_page(img, config, 300.0, 300.0);
    if (!result.is_blank) {
        std::cerr << "blank page: white image should be blank, fg%="
                  << result.foreground_percent << std::endl;
        return false;
    }
    if (result.foreground_percent != 0.0) {
        std::cerr << "blank page: white image fg% should be 0" << std::endl;
        return false;
    }

    return true;
}

bool test_blank_page_content_image() {
    using namespace ppp::core;

    Image img(200, 150, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    // Fill a large block with foreground.
    for (int y = 20; y < 130; ++y)
        for (int x = 20; x < 180; ++x)
            img.set_bw_pixel(x, y, 1);

    BlankPageConfig config;
    config.enabled = true;
    config.threshold_percent = 0.5;

    auto result = ops::detect_blank_page(img, config, 300.0, 300.0);
    if (result.is_blank) {
        std::cerr << "blank page: content image should NOT be blank, fg%="
                  << result.foreground_percent << std::endl;
        return false;
    }

    return true;
}

bool test_blank_page_sparse_content() {
    using namespace ppp::core;

    Image img(200, 150, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    // Just a few pixels — well below 0.5%.
    img.set_bw_pixel(50, 50, 1);
    img.set_bw_pixel(100, 75, 1);
    img.set_bw_pixel(150, 100, 1);

    BlankPageConfig config;
    config.enabled = true;
    config.threshold_percent = 0.5;

    auto result = ops::detect_blank_page(img, config, 300.0, 300.0);
    if (!result.is_blank) {
        std::cerr << "blank page: sparse content should be blank, fg%="
                  << result.foreground_percent << std::endl;
        return false;
    }

    return true;
}

bool test_blank_page_edge_margin() {
    using namespace ppp::core;

    Image img(300, 300, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    // Fill border strip (10 px wide) on all edges.
    for (int x = 0; x < 300; ++x) {
        for (int d = 0; d < 10; ++d) {
            img.set_bw_pixel(x, d, 1);
            img.set_bw_pixel(x, 299 - d, 1);
        }
    }
    for (int y = 0; y < 300; ++y) {
        for (int d = 0; d < 10; ++d) {
            img.set_bw_pixel(d, y, 1);
            img.set_bw_pixel(299 - d, y, 1);
        }
    }

    BlankPageConfig config;
    config.enabled = true;
    config.threshold_percent = 0.5;
    // 0.1 inches at 300 DPI = 30 pixels — excludes 10px border.
    config.edge_margin = {0.1, MeasurementUnit::Inches};

    auto result = ops::detect_blank_page(img, config, 300.0, 300.0);
    if (!result.is_blank) {
        std::cerr << "blank page edge margin: should be blank, fg%="
                  << result.foreground_percent << std::endl;
        return false;
    }

    return true;
}

bool test_blank_page_gray_input() {
    using namespace ppp::core;

    Image img(100, 80, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0xFF);  // White.

    BlankPageConfig config;
    config.enabled = true;
    config.threshold_percent = 0.5;

    auto result = ops::detect_blank_page(img, config, 300.0, 300.0);
    if (!result.is_blank) {
        std::cerr << "blank page gray: white image should be blank" << std::endl;
        return false;
    }

    return true;
}

bool test_pipeline_blank_page_step() {
    using namespace ppp::core;

    Image img(200, 150, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);  // White/blank.

    ProcessingProfile profile;
    profile.blank_page.enabled = true;
    profile.blank_page.threshold_percent = 0.5;
    profile.position_image = false;

    auto result = run_pipeline(img, profile);
    if (!result.success) {
        std::cerr << "pipeline blank: failed: " << result.error << std::endl;
        return false;
    }
    if (!result.is_blank) {
        std::cerr << "pipeline blank: should be blank" << std::endl;
        return false;
    }

    bool found = false;
    for (const auto& s : result.steps) {
        if (s.name == "blank_page" && s.applied) {
            found = true;
            if (s.detail.find("BLANK") == std::string::npos) {
                std::cerr << "pipeline blank: detail should say BLANK: "
                          << s.detail << std::endl;
                return false;
            }
        }
    }
    if (!found) {
        std::cerr << "pipeline blank: step not found" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Movement limit tests
// ---------------------------------------------------------------------------

bool test_movement_limit_within() {
    using namespace ppp::core;

    MovementLimitConfig config;
    config.enabled = true;
    config.max_horizontal = {2.0, MeasurementUnit::Inches};
    config.max_vertical = {2.0, MeasurementUnit::Inches};

    // Content moved 100px at 300 DPI = 0.33 inches — within limit.
    auto result = ops::check_movement_limit(50, 50, 150, 150, config, 300.0, 300.0);

    if (result.clamped) {
        std::cerr << "movement_limit within: should not be clamped" << std::endl;
        return false;
    }
    if (result.dx != 100 || result.dy != 100) {
        std::cerr << "movement_limit within: wrong dx/dy" << std::endl;
        return false;
    }

    return true;
}

bool test_movement_limit_clamped() {
    using namespace ppp::core;

    MovementLimitConfig config;
    config.enabled = true;
    config.max_horizontal = {0.5, MeasurementUnit::Inches};  // 150px at 300 DPI.
    config.max_vertical = {0.5, MeasurementUnit::Inches};

    // Content moved 300px at 300 DPI = 1.0 inches — exceeds 0.5 limit.
    auto result = ops::check_movement_limit(50, 50, 350, 350, config, 300.0, 300.0);

    if (!result.clamped) {
        std::cerr << "movement_limit clamped: should be clamped" << std::endl;
        return false;
    }
    // max_dx = 0.5 * 300 = 150.
    if (result.dx != 150 || result.dy != 150) {
        std::cerr << "movement_limit clamped: dx=" << result.dx
                  << " dy=" << result.dy << " (expected 150)" << std::endl;
        return false;
    }

    return true;
}

bool test_movement_limit_negative() {
    using namespace ppp::core;

    MovementLimitConfig config;
    config.enabled = true;
    config.max_horizontal = {1.0, MeasurementUnit::Inches};
    config.max_vertical = {1.0, MeasurementUnit::Inches};

    // Content moved -500px at 300 DPI = -1.67 inches — exceeds 1.0 limit.
    auto result = ops::check_movement_limit(500, 500, 0, 0, config, 300.0, 300.0);

    if (!result.clamped) {
        std::cerr << "movement_limit negative: should be clamped" << std::endl;
        return false;
    }
    // max_dx = 1.0 * 300 = 300, so dx clamped to -300.
    if (result.dx != -300 || result.dy != -300) {
        std::cerr << "movement_limit negative: dx=" << result.dx
                  << " dy=" << result.dy << " (expected -300)" << std::endl;
        return false;
    }

    return true;
}

bool test_movement_limit_disabled() {
    using namespace ppp::core;

    MovementLimitConfig config;
    config.enabled = false;

    auto result = ops::check_movement_limit(0, 0, 9999, 9999, config, 300.0, 300.0);

    if (result.clamped) {
        std::cerr << "movement_limit disabled: should not be clamped" << std::endl;
        return false;
    }
    if (result.dx != 9999 || result.dy != 9999) {
        std::cerr << "movement_limit disabled: displacement should be unchanged" << std::endl;
        return false;
    }

    return true;
}

bool test_movement_limit_pipeline() {
    using namespace ppp::core;

    // Create image with content in upper-left corner.
    Image img(300, 300, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    for (int y = 10; y < 30; ++y)
        for (int x = 10; x < 100; ++x)
            img.set_bw_pixel(x, y, 1);

    ProcessingProfile profile;
    profile.position_image = true;
    profile.canvas.preset = CanvasPreset::Custom;
    profile.canvas.width = {1.0, MeasurementUnit::Inches};
    profile.canvas.height = {1.0, MeasurementUnit::Inches};
    // Set large top margin to force big displacement.
    profile.margins[0].top.distance = {0.8, MeasurementUnit::Inches};
    profile.margins[0].top.mode = MarginMode::Set;
    // Enable movement limit to clamp it.
    profile.movement_limit.enabled = true;
    profile.movement_limit.max_vertical = {0.2, MeasurementUnit::Inches};
    profile.movement_limit.max_horizontal = {0.2, MeasurementUnit::Inches};

    auto result = run_pipeline(img, profile);
    if (!result.success) {
        std::cerr << "movement_limit pipeline: " << result.error << std::endl;
        return false;
    }

    // Check that movement_limit step exists and was applied.
    bool found = false;
    for (const auto& s : result.steps) {
        if (s.name == "movement_limit") {
            found = true;
            if (!s.applied) {
                std::cerr << "movement_limit pipeline: should have clamped" << std::endl;
                return false;
            }
        }
    }
    if (!found) {
        std::cerr << "movement_limit pipeline: step not found" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Batch processing tests
// ---------------------------------------------------------------------------

bool test_batch_basic() {
    using namespace ppp::core;

    // Create 3 simple images.
    std::vector<Image> images;
    for (int i = 0; i < 3; ++i) {
        Image img(100, 80, PixelFormat::BW1, 300.0, 300.0);
        img.fill(0);
        for (int x = 10; x < 90; ++x)
            img.set_bw_pixel(x, 40, 1);
        images.push_back(std::move(img));
    }

    ProcessingProfile profile;
    profile.position_image = false;

    auto batch = run_batch(images, profile);

    if (!batch.success) {
        std::cerr << "batch basic: " << batch.error << std::endl;
        return false;
    }
    if (batch.total != 3 || batch.succeeded != 3 || batch.failed != 0) {
        std::cerr << "batch basic: total=" << batch.total
                  << " succeeded=" << batch.succeeded
                  << " failed=" << batch.failed << std::endl;
        return false;
    }
    if (batch.pages.size() != 3) {
        std::cerr << "batch basic: wrong page count" << std::endl;
        return false;
    }

    return true;
}

bool test_batch_blank_pages() {
    using namespace ppp::core;

    std::vector<Image> images;

    // Page 0: content.
    Image img0(100, 80, PixelFormat::BW1, 300.0, 300.0);
    img0.fill(0);
    for (int y = 20; y < 60; ++y)
        for (int x = 20; x < 80; ++x)
            img0.set_bw_pixel(x, y, 1);
    images.push_back(std::move(img0));

    // Page 1: blank.
    Image img1(100, 80, PixelFormat::BW1, 300.0, 300.0);
    img1.fill(0);
    images.push_back(std::move(img1));

    // Page 2: content.
    Image img2(100, 80, PixelFormat::BW1, 300.0, 300.0);
    img2.fill(0);
    for (int x = 10; x < 90; ++x)
        img2.set_bw_pixel(x, 40, 1);
    images.push_back(std::move(img2));

    ProcessingProfile profile;
    profile.position_image = false;
    profile.blank_page.enabled = true;
    profile.blank_page.threshold_percent = 0.5;

    auto batch = run_batch(images, profile);

    if (batch.blank != 1) {
        std::cerr << "batch blank: expected 1 blank, got " << batch.blank << std::endl;
        return false;
    }

    // collect_images should skip blank page.
    auto collected = collect_images(batch, false);
    if (collected.size() != 2) {
        std::cerr << "batch blank: collect_images got " << collected.size()
                  << " (expected 2)" << std::endl;
        return false;
    }

    // collect_images with include_blank should get all 3.
    auto all = collect_images(batch, true);
    if (all.size() != 3) {
        std::cerr << "batch blank: collect_images(all) got " << all.size()
                  << " (expected 3)" << std::endl;
        return false;
    }

    return true;
}

bool test_batch_progress_callback() {
    using namespace ppp::core;

    std::vector<Image> images;
    for (int i = 0; i < 5; ++i) {
        Image img(50, 50, PixelFormat::Gray8, 300.0, 300.0);
        img.fill(128);
        images.push_back(std::move(img));
    }

    ProcessingProfile profile;
    profile.position_image = false;

    int callback_count = 0;
    auto batch = run_batch(images, profile,
        [&](std::size_t idx, std::size_t total, const ProcessingResult&) -> bool {
            ++callback_count;
            return true;  // Continue.
        });

    if (callback_count != 5) {
        std::cerr << "batch progress: callback called " << callback_count
                  << " times (expected 5)" << std::endl;
        return false;
    }

    return true;
}

bool test_batch_cancel() {
    using namespace ppp::core;

    std::vector<Image> images;
    for (int i = 0; i < 10; ++i) {
        Image img(50, 50, PixelFormat::Gray8, 300.0, 300.0);
        img.fill(128);
        images.push_back(std::move(img));
    }

    ProcessingProfile profile;
    profile.position_image = false;

    auto batch = run_batch(images, profile,
        [](std::size_t idx, std::size_t, const ProcessingResult&) -> bool {
            return idx < 3;  // Cancel after page 3.
        });

    if (batch.success) {
        std::cerr << "batch cancel: should not be success" << std::endl;
        return false;
    }
    // Should have processed pages 0,1,2,3 (cancel after idx=3).
    if (batch.pages.size() != 4) {
        std::cerr << "batch cancel: processed " << batch.pages.size()
                  << " pages (expected 4)" << std::endl;
        return false;
    }

    return true;
}

bool test_batch_empty() {
    using namespace ppp::core;

    std::vector<Image> images;
    ProcessingProfile profile;

    auto batch = run_batch(images, profile);

    if (batch.total != 0 || batch.succeeded != 0) {
        std::cerr << "batch empty: unexpected counts" << std::endl;
        return false;
    }
    if (!batch.pages.empty()) {
        std::cerr << "batch empty: should have no pages" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Enum from_string round-trip tests
// ---------------------------------------------------------------------------

bool test_enum_from_string_roundtrips() {
    using namespace ppp::core;

    // MarginMode.
    if (margin_mode_from_string("set") != MarginMode::Set) return false;
    if (margin_mode_from_string("check") != MarginMode::Check) return false;
    if (margin_mode_from_string("bogus").has_value()) return false;

    // DespeckleMode.
    if (despeckle_mode_from_string("none") != DespeckleMode::None) return false;
    if (despeckle_mode_from_string("single_pixel") != DespeckleMode::SinglePixel) return false;
    if (despeckle_mode_from_string("object") != DespeckleMode::Object) return false;

    // Rotation.
    if (rotation_from_string("cw90") != Rotation::CW90) return false;
    if (rotation_from_string("ccw90") != Rotation::CCW90) return false;
    if (rotation_from_string("r180") != Rotation::R180) return false;
    if (rotation_from_string("none") != Rotation::None) return false;

    // Orientation.
    if (orientation_from_string("portrait") != Orientation::Portrait) return false;
    if (orientation_from_string("landscape") != Orientation::Landscape) return false;

    // ResizeFrom.
    if (resize_from_from_string("subimage") != ResizeFrom::Subimage) return false;
    if (resize_from_from_string("full_page") != ResizeFrom::FullPage) return false;
    if (resize_from_from_string("custom") != ResizeFrom::Custom) return false;
    if (resize_from_from_string("smart") != ResizeFrom::Smart) return false;

    // VAlignment / HAlignment.
    if (v_alignment_from_string("top") != VAlignment::Top) return false;
    if (v_alignment_from_string("center") != VAlignment::Center) return false;
    if (v_alignment_from_string("bottom") != VAlignment::Bottom) return false;
    if (v_alignment_from_string("proportional") != VAlignment::Proportional) return false;
    if (h_alignment_from_string("center") != HAlignment::Center) return false;
    if (h_alignment_from_string("proportional") != HAlignment::Proportional) return false;

    // EdgeCleanupOrder.
    if (edge_cleanup_order_from_string("before_deskew") != EdgeCleanupOrder::BeforeDeskew) return false;
    if (edge_cleanup_order_from_string("after_deskew") != EdgeCleanupOrder::AfterDeskew) return false;

    // ConflictPolicy.
    if (conflict_policy_from_string("report") != ConflictPolicy::Report) return false;
    if (conflict_policy_from_string("overwrite") != ConflictPolicy::Overwrite) return false;

    // PathMode.
    if (path_mode_from_string("absolute") != PathMode::Absolute) return false;
    if (path_mode_from_string("portable") != PathMode::Portable) return false;

    // to_string → from_string round-trips.
    if (rotation_from_string(to_string(Rotation::CW90)) != Rotation::CW90) return false;
    if (orientation_from_string(to_string(Orientation::Landscape)) != Orientation::Landscape) return false;
    if (edge_cleanup_order_from_string(to_string(EdgeCleanupOrder::AfterDeskew)) != EdgeCleanupOrder::AfterDeskew) return false;
    if (conflict_policy_from_string(to_string(ConflictPolicy::Overwrite)) != ConflictPolicy::Overwrite) return false;
    if (path_mode_from_string(to_string(PathMode::Portable)) != PathMode::Portable) return false;

    return true;
}

bool test_blank_page_config_json_roundtrip() {
    using namespace ppp::core;

    ProcessingProfile profile;
    profile.name = "blank_test";
    profile.blank_page.enabled = true;
    profile.blank_page.threshold_percent = 1.5;
    profile.blank_page.min_components = 3;
    profile.blank_page.edge_margin = {0.25, MeasurementUnit::Inches};

    auto json = processing_profile_to_json(profile);

    // Verify blank_page appears in JSON.
    if (json.find("blank_page") == std::string::npos) {
        std::cerr << "blank_page not in JSON output" << std::endl;
        return false;
    }
    if (json.find("threshold_percent") == std::string::npos) {
        std::cerr << "threshold_percent not in JSON output" << std::endl;
        return false;
    }

    // Parse back.
    auto parsed = processing_profile_from_json(json);
    if (!parsed) {
        std::cerr << "blank_page JSON parse failed" << std::endl;
        return false;
    }

    if (!parsed->blank_page.enabled) {
        std::cerr << "blank_page.enabled not preserved" << std::endl;
        return false;
    }
    if (std::abs(parsed->blank_page.threshold_percent - 1.5) > 0.01) {
        std::cerr << "blank_page.threshold_percent not preserved" << std::endl;
        return false;
    }
    if (parsed->blank_page.min_components != 3) {
        std::cerr << "blank_page.min_components not preserved" << std::endl;
        return false;
    }
    if (std::abs(parsed->blank_page.edge_margin.value - 0.25) > 0.01) {
        std::cerr << "blank_page.edge_margin not preserved" << std::endl;
        return false;
    }
    if (parsed->blank_page.edge_margin.unit != MeasurementUnit::Inches) {
        std::cerr << "blank_page.edge_margin.unit not preserved" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// BMP tests
// ---------------------------------------------------------------------------

bool test_bmp_write_read_gray8() {
    using namespace ppp::core;

    Image img(40, 30, PixelFormat::Gray8, 150.0, 150.0);
    img.fill(0);
    // Draw a gradient pattern.
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 40; ++x)
            img.row(y)[x] = static_cast<std::uint8_t>((x * 6) & 0xFF);

    auto buf = bmp::write_bmp_to_memory(img);
    if (buf.empty()) { std::cerr << "bmp gray8 write failed" << std::endl; return false; }

    // Check BMP signature.
    if (buf[0] != 'B' || buf[1] != 'M') {
        std::cerr << "bmp gray8: bad signature" << std::endl;
        return false;
    }

    // Round-trip.
    auto img2 = bmp::read_bmp(buf.data(), buf.size());
    if (img2.empty()) { std::cerr << "bmp gray8 read failed" << std::endl; return false; }
    if (img2.width() != 40 || img2.height() != 30) {
        std::cerr << "bmp gray8: wrong size " << img2.width() << "x" << img2.height() << std::endl;
        return false;
    }
    if (img2.format() != PixelFormat::Gray8) {
        std::cerr << "bmp gray8: wrong format" << std::endl;
        return false;
    }

    // Verify pixel data.
    for (int y = 0; y < 30; ++y) {
        for (int x = 0; x < 40; ++x) {
            if (img2.row(y)[x] != img.row(y)[x]) {
                std::cerr << "bmp gray8: pixel mismatch at (" << x << "," << y << ")" << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool test_bmp_write_read_rgb24() {
    using namespace ppp::core;

    Image img(25, 20, PixelFormat::RGB24, 200.0, 200.0);
    img.fill(0);
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 25; ++x) {
            auto* p = img.row(y) + x * 3;
            p[0] = static_cast<std::uint8_t>(x * 10);      // R.
            p[1] = static_cast<std::uint8_t>(y * 12);      // G.
            p[2] = static_cast<std::uint8_t>((x + y) * 5); // B.
        }
    }

    auto buf = bmp::write_bmp_to_memory(img);
    if (buf.empty()) { std::cerr << "bmp rgb24 write failed" << std::endl; return false; }

    auto img2 = bmp::read_bmp(buf.data(), buf.size());
    if (img2.empty()) { std::cerr << "bmp rgb24 read failed" << std::endl; return false; }
    if (img2.width() != 25 || img2.height() != 20 || img2.format() != PixelFormat::RGB24) {
        std::cerr << "bmp rgb24: wrong dims/format" << std::endl;
        return false;
    }

    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 25; ++x) {
            const auto* a = img.row(y) + x * 3;
            const auto* b = img2.row(y) + x * 3;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2]) {
                std::cerr << "bmp rgb24: pixel mismatch at (" << x << "," << y << ")" << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool test_bmp_write_read_bw1() {
    using namespace ppp::core;

    Image img(80, 50, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);  // White.
    // Draw a diagonal stripe.
    for (int y = 0; y < 50; ++y) {
        int x = y;
        if (x < 80) img.set_bw_pixel(x, y, 1);
        if (x + 1 < 80) img.set_bw_pixel(x + 1, y, 1);
    }

    auto buf = bmp::write_bmp_to_memory(img);
    if (buf.empty()) { std::cerr << "bmp bw1 write failed" << std::endl; return false; }

    auto img2 = bmp::read_bmp(buf.data(), buf.size());
    if (img2.empty()) { std::cerr << "bmp bw1 read failed" << std::endl; return false; }
    if (img2.width() != 80 || img2.height() != 50 || img2.format() != PixelFormat::BW1) {
        std::cerr << "bmp bw1: wrong dims/format" << std::endl;
        return false;
    }

    for (int y = 0; y < 50; ++y) {
        for (int x = 0; x < 80; ++x) {
            if (img2.get_bw_pixel(x, y) != img.get_bw_pixel(x, y)) {
                std::cerr << "bmp bw1: pixel mismatch at (" << x << "," << y << ")" << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool test_bmp_write_read_rgba32() {
    using namespace ppp::core;

    Image img(15, 10, PixelFormat::RGBA32, 96.0, 96.0);
    img.fill(0);
    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 15; ++x) {
            auto* p = img.row(y) + x * 4;
            p[0] = static_cast<std::uint8_t>(x * 17);
            p[1] = static_cast<std::uint8_t>(y * 25);
            p[2] = static_cast<std::uint8_t>(128);
            p[3] = static_cast<std::uint8_t>(255);
        }
    }

    auto buf = bmp::write_bmp_to_memory(img);
    if (buf.empty()) { std::cerr << "bmp rgba32 write failed" << std::endl; return false; }

    auto img2 = bmp::read_bmp(buf.data(), buf.size());
    if (img2.empty()) { std::cerr << "bmp rgba32 read failed" << std::endl; return false; }
    if (img2.width() != 15 || img2.height() != 10 || img2.format() != PixelFormat::RGBA32) {
        std::cerr << "bmp rgba32: wrong dims/format" << std::endl;
        return false;
    }

    for (int y = 0; y < 10; ++y) {
        for (int x = 0; x < 15; ++x) {
            const auto* a = img.row(y) + x * 4;
            const auto* b = img2.row(y) + x * 4;
            if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3]) {
                std::cerr << "bmp rgba32: pixel mismatch at (" << x << "," << y << ")" << std::endl;
                return false;
            }
        }
    }

    return true;
}

bool test_bmp_file_roundtrip() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(60, 45, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(0);
    for (int y = 10; y < 35; ++y)
        for (int x = 15; x < 50; ++x)
            img.row(y)[x] = 200;

    auto tmp = fs::temp_directory_path() / "ppp_bmp_test.bmp";
    if (!bmp::write_bmp(img, tmp)) {
        std::cerr << "bmp file write failed" << std::endl;
        return false;
    }

    auto img2 = bmp::read_bmp_file(tmp);
    fs::remove(tmp);

    if (img2.empty()) { std::cerr << "bmp file read failed" << std::endl; return false; }
    if (img2.width() != 60 || img2.height() != 45) {
        std::cerr << "bmp file: wrong size" << std::endl;
        return false;
    }

    // Check a sample pixel.
    if (img2.row(20)[30] != 200) {
        std::cerr << "bmp file: pixel value mismatch" << std::endl;
        return false;
    }

    return true;
}

bool test_bmp_invalid_data() {
    using namespace ppp::core;

    // Empty data.
    auto img1 = bmp::read_bmp(nullptr, 0);
    if (!img1.empty()) { std::cerr << "bmp: should reject null" << std::endl; return false; }

    // Too small.
    std::uint8_t small[10] = {0};
    auto img2 = bmp::read_bmp(small, sizeof(small));
    if (!img2.empty()) { std::cerr << "bmp: should reject small" << std::endl; return false; }

    // Wrong signature.
    std::uint8_t bad_sig[30] = {0};
    bad_sig[0] = 'X'; bad_sig[1] = 'Y';
    auto img3 = bmp::read_bmp(bad_sig, sizeof(bad_sig));
    if (!img3.empty()) { std::cerr << "bmp: should reject bad signature" << std::endl; return false; }

    return true;
}

bool test_bmp_dpi_preservation() {
    using namespace ppp::core;

    Image img(20, 15, PixelFormat::Gray8, 400.0, 350.0);
    img.fill(128);

    auto buf = bmp::write_bmp_to_memory(img);
    auto img2 = bmp::read_bmp(buf.data(), buf.size());
    if (img2.empty()) { std::cerr << "bmp dpi: read failed" << std::endl; return false; }

    // DPI should be approximately preserved (rounding through ppm conversion).
    double dx = std::abs(img2.dpi_x() - 400.0);
    double dy = std::abs(img2.dpi_y() - 350.0);
    if (dx > 1.0 || dy > 1.0) {
        std::cerr << "bmp dpi: " << img2.dpi_x() << "x" << img2.dpi_y()
                  << " (expected ~400x350)" << std::endl;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Output writer tests
// ---------------------------------------------------------------------------

bool test_output_write_tiff() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(100, 80, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(128);

    auto tmp_dir = fs::temp_directory_path() / "ppp_output_test";
    fs::create_directories(tmp_dir);
    auto source = tmp_dir / "source.tif";

    OutputConfig config;
    config.tiff_output = true;
    config.raster_format = RasterFormat::Raw;  // Uncompressed for read-back.
    config.save_to_different_dir = false;

    auto result = output::write_output(img, source, config);
    if (!result.success) {
        std::cerr << "output tiff: " << result.error << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }
    if (result.format != "tiff") {
        std::cerr << "output tiff: format=" << result.format << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }
    if (!fs::exists(result.output_path)) {
        std::cerr << "output tiff: file not created" << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }

    // Verify we can read it back (reader only supports uncompressed).
    auto img2 = tiff::read_tiff_image_file(result.output_path);
    if (img2.empty()) {
        std::cerr << "output tiff: cannot read back" << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }

    fs::remove_all(tmp_dir);
    return true;
}

bool test_output_write_bmp() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(60, 40, PixelFormat::RGB24, 300.0, 300.0);
    img.fill(0);

    auto tmp_dir = fs::temp_directory_path() / "ppp_output_test_bmp";
    fs::create_directories(tmp_dir);
    auto source = tmp_dir / "source.bmp";

    OutputConfig config;
    config.new_extension = ".bmp";
    config.save_to_different_dir = false;

    auto result = output::write_output(img, source, config);
    if (!result.success) {
        std::cerr << "output bmp: " << result.error << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }
    if (result.format != "bmp") {
        std::cerr << "output bmp: format=" << result.format << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }

    fs::remove_all(tmp_dir);
    return true;
}

bool test_output_different_dir() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(50, 30, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(200);

    auto tmp_dir = fs::temp_directory_path() / "ppp_output_diffdir";
    auto out_dir = tmp_dir / "output";

    OutputConfig config;
    config.save_to_different_dir = true;
    config.output_directory = out_dir.string();
    config.tiff_output = true;

    auto result = output::write_output(img, tmp_dir / "input.tif", config);
    if (!result.success) {
        std::cerr << "output diffdir: " << result.error << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }

    // Should be in the output directory.
    if (result.output_path.parent_path() != out_dir) {
        std::cerr << "output diffdir: wrong dir: " << result.output_path.string() << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }

    fs::remove_all(tmp_dir);
    return true;
}

bool test_output_conflict_report() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(30, 20, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(100);

    auto tmp_dir = fs::temp_directory_path() / "ppp_output_conflict";
    fs::create_directories(tmp_dir);

    // Write the first time.
    OutputConfig config;
    config.save_to_different_dir = false;
    config.conflict_policy = ConflictPolicy::Overwrite;
    auto source = tmp_dir / "test.tif";
    auto r1 = output::write_output(img, source, config);
    if (!r1.success) { fs::remove_all(tmp_dir); return false; }

    // Now try with Report policy — should fail since file exists.
    config.conflict_policy = ConflictPolicy::Report;
    auto r2 = output::write_output(img, source, config);
    if (r2.success) {
        std::cerr << "output conflict: should have failed" << std::endl;
        fs::remove_all(tmp_dir);
        return false;
    }

    fs::remove_all(tmp_dir);
    return true;
}

bool test_output_write_to() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(40, 30, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    img.set_bw_pixel(20, 15, 1);

    auto tmp = fs::temp_directory_path() / "ppp_output_to.tif";
    OutputConfig config;
    config.raster_format = RasterFormat::Raw;  // Uncompressed for read-back.
    auto result = output::write_output_to(img, tmp, config);
    if (!result.success) {
        std::cerr << "output write_to: " << result.error << std::endl;
        return false;
    }

    auto img2 = tiff::read_tiff_image_file(tmp);
    fs::remove(tmp);
    if (img2.empty()) {
        std::cerr << "output write_to: cannot read back" << std::endl;
        return false;
    }

    return true;
}

bool test_output_multipage() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img1(50, 40, PixelFormat::Gray8, 300.0, 300.0);
    img1.fill(100);
    Image img2(50, 40, PixelFormat::Gray8, 300.0, 300.0);
    img2.fill(200);

    auto tmp = fs::temp_directory_path() / "ppp_output_multi.tif";
    auto result = output::write_multipage_output({img1, img2}, tmp);
    if (!result.success) {
        std::cerr << "output multipage: " << result.error << std::endl;
        return false;
    }
    if (!fs::exists(tmp)) {
        std::cerr << "output multipage: file not created" << std::endl;
        return false;
    }

    fs::remove(tmp);
    return true;
}

// ---------------------------------------------------------------------------
// PDF output tests
// ---------------------------------------------------------------------------

bool test_pdf_write_gray8() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(100, 80, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(128);

    auto tmp = fs::temp_directory_path() / "ppp_test_gray8.pdf";
    bool ok = pdf::write_pdf(img, tmp);
    if (!ok) {
        std::cerr << "pdf write gray8: write failed" << std::endl;
        return false;
    }
    if (!fs::exists(tmp)) {
        std::cerr << "pdf write gray8: file not created" << std::endl;
        return false;
    }

    // Basic validation: check file starts with %PDF.
    std::ifstream f(tmp, std::ios::binary);
    char header[5] = {};
    f.read(header, 5);
    if (std::string(header, 5) != "%PDF-") {
        std::cerr << "pdf write gray8: invalid PDF header" << std::endl;
        fs::remove(tmp);
        return false;
    }

    fs::remove(tmp);
    return true;
}

bool test_pdf_write_rgb24() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(60, 40, PixelFormat::RGB24, 150.0, 150.0);
    img.fill(200);

    auto tmp = fs::temp_directory_path() / "ppp_test_rgb24.pdf";
    bool ok = pdf::write_pdf(img, tmp);
    if (!ok) {
        std::cerr << "pdf write rgb24: write failed" << std::endl;
        return false;
    }
    if (!fs::exists(tmp) || fs::file_size(tmp) < 100) {
        std::cerr << "pdf write rgb24: file too small or missing" << std::endl;
        fs::remove(tmp);
        return false;
    }

    fs::remove(tmp);
    return true;
}

bool test_pdf_write_bw1() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(80, 60, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    for (int x = 10; x < 70; ++x)
        img.set_bw_pixel(x, 30, 1);

    auto tmp = fs::temp_directory_path() / "ppp_test_bw1.pdf";
    bool ok = pdf::write_pdf(img, tmp);
    if (!ok) {
        std::cerr << "pdf write bw1: write failed" << std::endl;
        return false;
    }

    // Check header.
    std::ifstream f(tmp, std::ios::binary);
    char header[5] = {};
    f.read(header, 5);
    if (std::string(header, 5) != "%PDF-") {
        std::cerr << "pdf write bw1: invalid PDF header" << std::endl;
        fs::remove(tmp);
        return false;
    }

    fs::remove(tmp);
    return true;
}

bool test_pdf_write_multipage() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img1(50, 40, PixelFormat::Gray8, 300.0, 300.0);
    img1.fill(100);
    Image img2(50, 40, PixelFormat::Gray8, 300.0, 300.0);
    img2.fill(200);
    Image img3(50, 40, PixelFormat::RGB24, 300.0, 300.0);
    img3.fill(150);

    auto tmp = fs::temp_directory_path() / "ppp_test_multi.pdf";
    bool ok = pdf::write_multipage_pdf({img1, img2, img3}, tmp);
    if (!ok) {
        std::cerr << "pdf multipage: write failed" << std::endl;
        return false;
    }

    // Check header and that file has reasonable size.
    std::ifstream f(tmp, std::ios::binary);
    char header[5] = {};
    f.read(header, 5);
    if (std::string(header, 5) != "%PDF-") {
        std::cerr << "pdf multipage: invalid header" << std::endl;
        fs::remove(tmp);
        return false;
    }

    fs::remove(tmp);
    return true;
}

bool test_pdf_to_memory() {
    using namespace ppp::core;

    Image img(30, 20, PixelFormat::Gray8, 72.0, 72.0);
    img.fill(64);

    auto data = pdf::write_pdf_to_memory(img);
    if (data.size() < 50) {
        std::cerr << "pdf to memory: output too small" << std::endl;
        return false;
    }

    // Check starts with %PDF-.
    if (data[0] != '%' || data[1] != 'P' || data[2] != 'D' || data[3] != 'F') {
        std::cerr << "pdf to memory: invalid header" << std::endl;
        return false;
    }

    return true;
}

bool test_output_write_to_pdf() {
    using namespace ppp::core;
    namespace fs = std::filesystem;

    Image img(50, 40, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(128);

    auto tmp = fs::temp_directory_path() / "ppp_output_test.pdf";
    auto result = output::write_output_to(img, tmp);
    if (!result.success) {
        std::cerr << "output write_to pdf: " << result.error << std::endl;
        return false;
    }
    if (result.format != "pdf") {
        std::cerr << "output write_to pdf: format=" << result.format << std::endl;
        return false;
    }

    fs::remove(tmp);
    return true;
}

// ---------------------------------------------------------------------------
// Deskew tests
// ---------------------------------------------------------------------------

bool test_detect_skew_angle_zero() {
    using namespace ppp::core;

    // Create a BW1 image with horizontal text lines (no skew).
    // 200x100 with 3 horizontal lines of foreground pixels.
    Image img(200, 100, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);  // All white (BW1: 0 = white).

    // Draw 3 horizontal lines at y=20, y=50, y=80.
    for (int line : {20, 50, 80}) {
        for (int x = 10; x < 190; ++x) {
            img.set_bw_pixel(x, line, 1);
            img.set_bw_pixel(x, line + 1, 1);
        }
    }

    double angle = ops::detect_skew_angle(img);
    // Horizontal lines should produce angle near zero.
    if (std::abs(angle) > 0.5) {
        std::cerr << "detect_skew_angle zero: expected ~0, got " << angle << std::endl;
        return false;
    }

    return true;
}

bool test_rotate_arbitrary_identity() {
    using namespace ppp::core;

    // Rotating by 0 degrees should return the same image.
    Image img(50, 40, PixelFormat::Gray8, 300.0, 300.0);
    img.fill(128);
    img.row(20)[25] = 42;  // Set a distinctive pixel.

    Image rotated = ops::rotate_arbitrary(img, 0.0);
    if (rotated.width() != 50 || rotated.height() != 40) {
        std::cerr << "rotate_arbitrary identity: wrong size" << std::endl;
        return false;
    }
    if (rotated.row(20)[25] != 42) {
        std::cerr << "rotate_arbitrary identity: pixel mismatch" << std::endl;
        return false;
    }

    return true;
}

bool test_rotate_arbitrary_bw1() {
    using namespace ppp::core;

    // A small BW1 image rotated by a small angle should produce a valid result.
    Image img(100, 80, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);  // White.

    // Draw a horizontal line.
    for (int x = 10; x < 90; ++x) {
        img.set_bw_pixel(x, 40, 1);
    }

    Image rotated = ops::rotate_arbitrary(img, 2.0);
    // Output should be larger (to contain rotated content without clipping).
    if (rotated.empty()) {
        std::cerr << "rotate_arbitrary BW1: empty result" << std::endl;
        return false;
    }
    if (rotated.format() != PixelFormat::BW1) {
        std::cerr << "rotate_arbitrary BW1: wrong format" << std::endl;
        return false;
    }

    return true;
}

bool test_apply_deskew_disabled() {
    using namespace ppp::core;

    Image img(100, 80, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    DeskewConfig config;
    config.enabled = false;

    auto result = ops::apply_deskew(img, config);
    if (result.corrected) {
        std::cerr << "apply_deskew disabled: should not correct" << std::endl;
        return false;
    }
    if (result.angle != 0.0) {
        std::cerr << "apply_deskew disabled: angle should be 0" << std::endl;
        return false;
    }

    return true;
}

bool test_apply_deskew_below_threshold() {
    using namespace ppp::core;

    // Create an image with perfectly horizontal lines — angle should be ~0,
    // which is below min_angle threshold, so correction should not be applied.
    Image img(200, 100, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    for (int line : {20, 50, 80}) {
        for (int x = 10; x < 190; ++x) {
            img.set_bw_pixel(x, line, 1);
            img.set_bw_pixel(x, line + 1, 1);
        }
    }

    DeskewConfig config;
    config.enabled = true;
    config.min_angle = 0.5;  // Only correct angles >= 0.5 degrees.
    config.max_angle = 5.0;

    auto result = ops::apply_deskew(img, config);
    // Angle should be near zero, below threshold — not corrected.
    if (result.corrected) {
        std::cerr << "apply_deskew below threshold: should not correct, angle="
                  << result.angle << std::endl;
        return false;
    }

    return true;
}

bool test_pipeline_deskew_step() {
    using namespace ppp::core;

    // Test that deskew step appears in pipeline output.
    Image img(200, 100, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);

    // Horizontal lines.
    for (int line : {20, 50, 80}) {
        for (int x = 10; x < 190; ++x) {
            img.set_bw_pixel(x, line, 1);
        }
    }

    ProcessingProfile profile;
    profile.deskew.enabled = true;
    profile.deskew.min_angle = 0.05;
    profile.deskew.max_angle = 5.0;
    profile.position_image = false;

    auto result = run_pipeline(img, profile);
    if (!result.success) {
        std::cerr << "pipeline deskew: failed: " << result.error << std::endl;
        return false;
    }

    // Find the deskew step.
    bool found_deskew = false;
    for (const auto& s : result.steps) {
        if (s.name == "deskew") {
            found_deskew = true;
            // Should contain angle info.
            if (s.detail.find("angle=") == std::string::npos) {
                std::cerr << "pipeline deskew: missing angle in detail: "
                          << s.detail << std::endl;
                return false;
            }
        }
    }
    if (!found_deskew) {
        std::cerr << "pipeline deskew: step not found" << std::endl;
        return false;
    }

    return true;
}

bool test_pipeline_run_step_deskew() {
    using namespace ppp::core;

    Image img(200, 100, PixelFormat::BW1, 300.0, 300.0);
    img.fill(0);
    for (int x = 10; x < 190; ++x)
        img.set_bw_pixel(x, 50, 1);

    ProcessingProfile profile;
    profile.deskew.enabled = true;
    profile.deskew.min_angle = 0.05;
    profile.deskew.max_angle = 5.0;

    auto result = run_step(img, profile, "deskew");
    if (!result.success) {
        std::cerr << "run_step deskew: failed: " << result.error << std::endl;
        return false;
    }
    if (result.steps.empty() || result.steps[0].name != "deskew") {
        std::cerr << "run_step deskew: wrong step name" << std::endl;
        return false;
    }

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
        {"tiff_parse_synthetic", test_tiff_parse_synthetic},
        {"tiff_ifd_accessors", test_tiff_ifd_accessors},
        {"tiff_invalid_data", test_tiff_invalid_data},
        {"tiff_big_endian", test_tiff_big_endian},
        {"geometry_rect_basics", test_geometry_rect_basics},
        {"geometry_combinable", test_geometry_combinable},
        {"geometry_sort", test_geometry_sort},
        {"geometry_banding", test_geometry_banding},
        {"geometry_spans_from_bitmap", test_geometry_spans_from_bitmap},
        {"geometry_connected_components", test_geometry_connected_components},
        {"geometry_connected_components_roi", test_geometry_connected_components_roi},
        {"image_construction", test_image_construction},
        {"image_bw_pixel_access", test_image_bw_pixel_access},
        {"image_fill_and_invert", test_image_fill_and_invert},
        {"image_crop", test_image_crop},
        {"image_rotate_cw90", test_image_rotate_cw90},
        {"image_rotate_ccw90", test_image_rotate_ccw90},
        {"image_rotate_180", test_image_rotate_180},
        {"image_pad_and_blit", test_image_pad_and_blit},
        {"image_convert_gray_to_bw", test_image_convert_gray_to_bw},
        {"image_convert_bw_to_gray", test_image_convert_bw_to_gray},
        {"image_convert_rgb_to_gray", test_image_convert_rgb_to_gray},
        {"image_deep_copy", test_image_deep_copy},
        {"ops_unit_conversion", test_ops_unit_conversion},
        {"ops_subimage_detection", test_ops_subimage_detection},
        {"ops_subimage_filters_small", test_ops_subimage_filters_small},
        {"ops_edge_cleanup", test_ops_edge_cleanup},
        {"ops_despeckle_single_pixel", test_ops_despeckle_single_pixel},
        {"ops_despeckle_object", test_ops_despeckle_object},
        {"ops_canvas_resolution", test_ops_canvas_resolution},
        {"ops_apply_margins", test_ops_apply_margins},
        {"ops_apply_margins_centered", test_ops_apply_margins_centered},
        {"pipeline_empty_image", test_pipeline_empty_image},
        {"pipeline_passthrough", test_pipeline_passthrough},
        {"pipeline_rotation", test_pipeline_rotation},
        {"pipeline_edge_cleanup_and_despeckle", test_pipeline_edge_cleanup_and_despeckle},
        {"pipeline_full_with_margins", test_pipeline_full_with_margins},
        {"pipeline_odd_even_pages", test_pipeline_odd_even_pages},
        {"pipeline_run_step", test_pipeline_run_step},
        {"pipeline_step_log_detail", test_pipeline_step_log_detail},
        {"tiff_write_gray8_uncompressed", test_tiff_write_gray8_uncompressed},
        {"tiff_write_bw1_uncompressed", test_tiff_write_bw1_uncompressed},
        {"tiff_write_rgb24_uncompressed", test_tiff_write_rgb24_uncompressed},
        {"tiff_write_packbits", test_tiff_write_packbits},
        {"tiff_write_lzw", test_tiff_write_lzw},
        {"tiff_roundtrip_gray8", test_tiff_roundtrip_gray8},
        {"tiff_roundtrip_bw1", test_tiff_roundtrip_bw1},
        {"tiff_roundtrip_rgb24", test_tiff_roundtrip_rgb24},
        {"tiff_file_roundtrip", test_tiff_file_roundtrip},
        {"tiff_multipage", test_tiff_multipage},
        {"scale_nearest_gray8", test_scale_nearest_gray8},
        {"scale_bilinear_gray8", test_scale_bilinear_gray8},
        {"scale_bw1", test_scale_bw1},
        {"resize_basic", test_resize_basic},
        {"resize_no_enlarge", test_resize_no_enlarge},
        {"resize_alignment", test_resize_alignment},
        {"pipeline_with_resize", test_pipeline_with_resize},
        {"histogram_gray8", test_histogram_gray8},
        {"histogram_bimodal", test_histogram_bimodal},
        {"histogram_rgb24", test_histogram_rgb24},
        {"histogram_bw1", test_histogram_bw1},
        {"otsu_threshold", test_otsu_threshold},
        {"binarize_gray8", test_binarize_gray8},
        {"binarize_otsu", test_binarize_otsu},
        {"binarize_already_bw1", test_binarize_already_bw1},
        {"color_dropout_red", test_color_dropout_red},
        {"color_dropout_green", test_color_dropout_green},
        {"color_dropout_disabled", test_color_dropout_disabled},
        {"color_dropout_bw_passthrough", test_color_dropout_bw_passthrough},
        {"color_dropout_config_json", test_color_dropout_config_json},
        {"morph_dilate", test_morph_dilate},
        {"morph_dilate_square", test_morph_dilate_square},
        {"morph_erode", test_morph_erode},
        {"morph_open_removes_noise", test_morph_open_removes_noise},
        {"morph_close_fills_holes", test_morph_close_fills_holes},
        {"blank_page_empty_image", test_blank_page_empty_image},
        {"blank_page_white_image", test_blank_page_white_image},
        {"blank_page_content_image", test_blank_page_content_image},
        {"blank_page_sparse_content", test_blank_page_sparse_content},
        {"blank_page_edge_margin", test_blank_page_edge_margin},
        {"blank_page_gray_input", test_blank_page_gray_input},
        {"pipeline_blank_page_step", test_pipeline_blank_page_step},
        {"movement_limit_within", test_movement_limit_within},
        {"movement_limit_clamped", test_movement_limit_clamped},
        {"movement_limit_negative", test_movement_limit_negative},
        {"movement_limit_disabled", test_movement_limit_disabled},
        {"movement_limit_pipeline", test_movement_limit_pipeline},
        {"batch_basic", test_batch_basic},
        {"batch_blank_pages", test_batch_blank_pages},
        {"batch_progress_callback", test_batch_progress_callback},
        {"batch_cancel", test_batch_cancel},
        {"batch_empty", test_batch_empty},
        {"enum_from_string_roundtrips", test_enum_from_string_roundtrips},
        {"blank_page_config_json_roundtrip", test_blank_page_config_json_roundtrip},
        {"bmp_write_read_gray8", test_bmp_write_read_gray8},
        {"bmp_write_read_rgb24", test_bmp_write_read_rgb24},
        {"bmp_write_read_bw1", test_bmp_write_read_bw1},
        {"bmp_write_read_rgba32", test_bmp_write_read_rgba32},
        {"bmp_file_roundtrip", test_bmp_file_roundtrip},
        {"bmp_invalid_data", test_bmp_invalid_data},
        {"bmp_dpi_preservation", test_bmp_dpi_preservation},
        {"output_write_tiff", test_output_write_tiff},
        {"output_write_bmp", test_output_write_bmp},
        {"output_different_dir", test_output_different_dir},
        {"output_conflict_report", test_output_conflict_report},
        {"output_write_to", test_output_write_to},
        {"output_multipage", test_output_multipage},
        {"pdf_write_gray8", test_pdf_write_gray8},
        {"pdf_write_rgb24", test_pdf_write_rgb24},
        {"pdf_write_bw1", test_pdf_write_bw1},
        {"pdf_write_multipage", test_pdf_write_multipage},
        {"pdf_to_memory", test_pdf_to_memory},
        {"output_write_to_pdf", test_output_write_to_pdf},
        {"detect_skew_angle_zero", test_detect_skew_angle_zero},
        {"rotate_arbitrary_identity", test_rotate_arbitrary_identity},
        {"rotate_arbitrary_bw1", test_rotate_arbitrary_bw1},
        {"apply_deskew_disabled", test_apply_deskew_disabled},
        {"apply_deskew_below_threshold", test_apply_deskew_below_threshold},
        {"pipeline_deskew_step", test_pipeline_deskew_step},
        {"pipeline_run_step_deskew", test_pipeline_run_step_deskew},
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

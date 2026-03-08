#pragma once

#include "ppp/core/job.h"
#include "ppp/core/job_repository.h"
#include "ppp/core/scheduling_policy.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace ppp::core {

struct JobEvent {
    std::string job_id;
    JobState state;
    std::optional<std::string> message;
};

struct JobSummary {
    std::size_t total{0};
    std::size_t submitted{0};
    std::size_t validating{0};
    std::size_t rendering{0};
    std::size_t exception{0};
    std::size_t completed{0};
    std::size_t cancelled{0};
    std::size_t outstanding{0};
    std::optional<TimePoint> oldest_created{};
    std::optional<TimePoint> oldest_outstanding{};
    std::optional<TimePoint> latest_update{};
    std::optional<TimePoint> next_due{};
};

/// Provides lifecycle operations for PPP jobs built on top of a repository.
class JobService {
public:
    using EventSink = std::function<void(const JobEvent&)>;

    explicit JobService(JobRepository& repository);

    std::string create_job(JobPayload payload, std::optional<std::string> correlation_id = std::nullopt,
                           std::int32_t priority = 0,
                           std::optional<TimePoint> due_at = std::nullopt);
    void mark_validating(std::string_view id);
    void mark_rendering(std::string_view id);
    void mark_completed(std::string_view id);
    void mark_failed(std::string_view id, std::string message);
    void mark_cancelled(std::string_view id, std::optional<std::string> reason = std::nullopt);
    [[nodiscard]] bool retry(std::string_view id, bool clear_error = true,
                             std::optional<std::string> note = std::nullopt);
    [[nodiscard]] bool update_priority(std::string_view id, std::int32_t priority);
    [[nodiscard]] bool update_due_at(std::string_view id, std::optional<TimePoint> due_at);
    [[nodiscard]] bool update_correlation(std::string_view id, std::optional<std::string> correlation_id);
    [[nodiscard]] bool add_attachment(std::string_view id, std::string attachment);
    [[nodiscard]] bool remove_attachment(std::string_view id, std::string_view attachment);
    [[nodiscard]] bool clear_attachments(std::string_view id);
    [[nodiscard]] bool add_tag(std::string_view id, std::string tag);
    [[nodiscard]] bool remove_tag(std::string_view id, std::string_view tag);
    [[nodiscard]] bool clear_tags(std::string_view id);

    [[nodiscard]] std::optional<JobRecord> claim_next_submitted(JobState next_state = JobState::Validating);

    [[nodiscard]] std::optional<JobRecord> get(std::string_view id) const;
    [[nodiscard]] std::vector<JobRecord> list(std::optional<JobState> state_filter = std::nullopt) const;
    [[nodiscard]] std::vector<JobRecord> list_with_tags(
        std::optional<JobState> state_filter, std::vector<std::string> required_tags,
        std::optional<std::string> correlation_id = std::nullopt) const;
    [[nodiscard]] JobSummary summarize() const;

    /// Apply the provided scheduling policy to outstanding jobs. Returns the
    /// number of records whose priority changed as a result of the policy.
    std::size_t apply_scheduling_policy(const SchedulingPolicy& policy,
                                        TimePoint reference_time = Clock::now());

    /// Remove jobs in the provided state whose most recent activity predates the
    /// supplied timestamp. Returns the number of purged records.
    std::size_t purge(JobState state, TimePoint older_than);

    /// Move any jobs stuck in validating or rendering states back to submitted
    /// so workers can resume them. Returns the number of jobs transitioned.
    std::size_t resume_active_jobs();

    void on_event(EventSink sink);

private:
    void transition(std::string_view id, JobState state, std::optional<std::string> message = std::nullopt);

    JobRepository& repository_;
    EventSink sink_;
};

} // namespace ppp::core

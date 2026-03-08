#include "ppp/core/job_service.h"

#include <algorithm>
#include <chrono>
#include <random>
#include <sstream>
#include <utility>

namespace ppp::core {

namespace {
[[nodiscard]] std::string generate_job_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex;
    for (int i = 0; i < 2; ++i) {
        oss << dist(rng);
    }
    return oss.str();
}

void emit(JobService::EventSink& sink, const JobEvent& event) {
    if (sink) {
        sink(event);
    }
}
} // namespace

JobService::JobService(JobRepository& repository) : repository_(repository) {}

std::string JobService::create_job(JobPayload payload, std::optional<std::string> correlation_id,
                                   std::int32_t priority, std::optional<TimePoint> due_at) {
    JobRecord record;
    record.id = generate_job_id();
    record.payload = std::move(payload);
    record.correlation_id = std::move(correlation_id);
    record.created_at = Clock::now();
    record.updated_at = record.created_at;
    record.priority = priority;
    record.due_at = std::move(due_at);
    repository_.upsert(record);

    emit(sink_, JobEvent{record.id, record.state, std::nullopt});
    return record.id;
}

void JobService::mark_validating(std::string_view id) {
    transition(id, JobState::Validating);
}

void JobService::mark_rendering(std::string_view id) {
    transition(id, JobState::Rendering);
}

void JobService::mark_completed(std::string_view id) {
    transition(id, JobState::Completed);
}

void JobService::mark_failed(std::string_view id, std::string message) {
    transition(id, JobState::Exception, std::move(message));
}

void JobService::mark_cancelled(std::string_view id, std::optional<std::string> reason) {
    transition(id, JobState::Cancelled, std::move(reason));
}

bool JobService::retry(std::string_view id, bool clear_error, std::optional<std::string> note) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    record->state = JobState::Submitted;
    if (clear_error) {
        record->error_message.reset();
    }

    repository_.upsert(*record);
    emit(sink_, JobEvent{std::string{id}, JobState::Submitted, std::move(note)});
    return true;
}

bool JobService::update_priority(std::string_view id, std::int32_t priority) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    if (record->priority == priority) {
        return true;
    }

    record->priority = priority;
    repository_.upsert(*record);

    emit(sink_, JobEvent{std::string{id}, record->state,
                         std::string{"priority set to "} + std::to_string(priority)});
    return true;
}

bool JobService::update_due_at(std::string_view id, std::optional<TimePoint> due_at) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    if (record->due_at == due_at) {
        return true;
    }

    record->due_at = std::move(due_at);
    repository_.upsert(*record);

    std::string message;
    if (record->due_at) {
        message = "due_at set";
    } else {
        message = "due_at cleared";
    }
    emit(sink_, JobEvent{std::string{id}, record->state, std::move(message)});
    return true;
}

bool JobService::update_correlation(std::string_view id, std::optional<std::string> correlation_id) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    if (record->correlation_id == correlation_id) {
        return true;
    }

    record->correlation_id = std::move(correlation_id);
    repository_.upsert(*record);

    std::string message;
    if (record->correlation_id) {
        message = "correlation set to " + *record->correlation_id;
    } else {
        message = "correlation cleared";
    }
    emit(sink_, JobEvent{std::string{id}, record->state, std::move(message)});
    return true;
}

bool JobService::add_attachment(std::string_view id, std::string attachment) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    const auto already_present = std::find(record->payload.attachments.begin(), record->payload.attachments.end(), attachment) !=
                                 record->payload.attachments.end();
    if (already_present) {
        return true;
    }

    record->payload.attachments.push_back(attachment);
    repository_.upsert(*record);

    emit(sink_, JobEvent{std::string{id}, record->state,
                         std::string{"attachment added: "} + std::move(attachment)});
    return true;
}

bool JobService::remove_attachment(std::string_view id, std::string_view attachment) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    auto& attachments = record->payload.attachments;
    const auto original_size = attachments.size();
    attachments.erase(std::remove(attachments.begin(), attachments.end(), attachment), attachments.end());
    if (attachments.size() == original_size) {
        return true;
    }

    repository_.upsert(*record);
    emit(sink_, JobEvent{std::string{id}, record->state,
                         std::string{"attachment removed: "} + std::string{attachment}});
    return true;
}

bool JobService::clear_attachments(std::string_view id) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    if (record->payload.attachments.empty()) {
        return true;
    }

    record->payload.attachments.clear();
    repository_.upsert(*record);

    emit(sink_, JobEvent{std::string{id}, record->state, std::string{"attachments cleared"}});
    return true;
}

bool JobService::add_tag(std::string_view id, std::string tag) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    const auto already_present = std::find(record->payload.tags.begin(), record->payload.tags.end(), tag) !=
                                 record->payload.tags.end();
    if (already_present) {
        return true;
    }

    record->payload.tags.push_back(tag);
    repository_.upsert(*record);

    emit(sink_, JobEvent{std::string{id}, record->state,
                         std::string{"tag added: "} + std::move(tag)});
    return true;
}

bool JobService::remove_tag(std::string_view id, std::string_view tag) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    auto& tags = record->payload.tags;
    const auto original_size = tags.size();
    tags.erase(std::remove(tags.begin(), tags.end(), tag), tags.end());
    if (tags.size() == original_size) {
        return true;
    }

    repository_.upsert(*record);
    emit(sink_, JobEvent{std::string{id}, record->state,
                         std::string{"tag removed: "} + std::string{tag}});
    return true;
}

bool JobService::clear_tags(std::string_view id) {
    auto record = repository_.fetch(id);
    if (!record) {
        return false;
    }

    if (record->payload.tags.empty()) {
        return true;
    }

    record->payload.tags.clear();
    repository_.upsert(*record);

    emit(sink_, JobEvent{std::string{id}, record->state, std::string{"tags cleared"}});
    return true;
}

std::optional<JobRecord> JobService::claim_next_submitted(JobState next_state) {
    auto record = repository_.claim_next_submitted(next_state);
    if (record) {
        emit(sink_, JobEvent{record->id, record->state, std::nullopt});
    }
    return record;
}

std::optional<JobRecord> JobService::get(std::string_view id) const {
    return repository_.fetch(id);
}

std::vector<JobRecord> JobService::list(std::optional<JobState> state_filter) const {
    return repository_.list(state_filter);
}

std::vector<JobRecord> JobService::list_with_tags(std::optional<JobState> state_filter,
                                                  std::vector<std::string> required_tags,
                                                  std::optional<std::string> correlation_id) const {
    auto records = repository_.list(state_filter);
    if (required_tags.empty() && !correlation_id) {
        return records;
    }

    if (!required_tags.empty()) {
        std::sort(required_tags.begin(), required_tags.end());
        required_tags.erase(std::unique(required_tags.begin(), required_tags.end()), required_tags.end());
    }

    std::vector<JobRecord> filtered;
    filtered.reserve(records.size());
    for (const auto& record : records) {
        if (correlation_id) {
            if (!record.correlation_id || *record.correlation_id != *correlation_id) {
                continue;
            }
        }

        if (required_tags.empty()) {
            filtered.push_back(record);
            continue;
        }

        const auto& tags = record.payload.tags;
        const bool matches = std::all_of(required_tags.begin(), required_tags.end(),
                                         [&tags](const std::string& tag) {
                                             return std::find(tags.begin(), tags.end(), tag) != tags.end();
                                         });
        if (matches) {
            filtered.push_back(record);
        }
    }

    return filtered;
}

JobSummary JobService::summarize() const {
    JobSummary summary;
    const auto records = repository_.list(std::nullopt);
    for (const auto& record : records) {
        ++summary.total;
        switch (record.state) {
        case JobState::Submitted:
            ++summary.submitted;
            break;
        case JobState::Validating:
            ++summary.validating;
            break;
        case JobState::Rendering:
            ++summary.rendering;
            break;
        case JobState::Exception:
            ++summary.exception;
            break;
        case JobState::Completed:
            ++summary.completed;
            break;
        case JobState::Cancelled:
            ++summary.cancelled;
            break;
        }

        if (!summary.oldest_created || record.created_at < *summary.oldest_created) {
            summary.oldest_created = record.created_at;
        }

        const auto activity = record.updated_at.value_or(record.created_at);
        if (!summary.latest_update || activity > *summary.latest_update) {
            summary.latest_update = activity;
        }

        const bool outstanding = record.state != JobState::Completed && record.state != JobState::Cancelled;
        if (outstanding) {
            ++summary.outstanding;
            if (!summary.oldest_outstanding || record.created_at < *summary.oldest_outstanding) {
                summary.oldest_outstanding = record.created_at;
            }
            if (record.due_at) {
                if (!summary.next_due || *record.due_at < *summary.next_due) {
                    summary.next_due = record.due_at;
                }
            }
        }
    }
    return summary;
}

std::size_t JobService::apply_scheduling_policy(const SchedulingPolicy& policy, TimePoint reference_time) {
    if (policy.escalations.empty() && !policy.overdue_priority) {
        return 0;
    }

    auto escalations = policy.escalations;
    std::sort(escalations.begin(), escalations.end(), [](const SchedulingEscalation& lhs, const SchedulingEscalation& rhs) {
        if (lhs.within == rhs.within) {
            return lhs.priority > rhs.priority;
        }
        return lhs.within < rhs.within;
    });

    std::size_t updated = 0;
    const auto records = repository_.list(std::nullopt);
    for (auto record : records) {
        if (record.state == JobState::Completed || record.state == JobState::Cancelled) {
            continue;
        }
        if (!record.due_at) {
            continue;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(*record.due_at - reference_time);
        std::optional<std::int32_t> target_priority;

        if (remaining <= std::chrono::seconds::zero()) {
            if (policy.overdue_priority) {
                target_priority = *policy.overdue_priority;
            } else if (policy.escalate_overdue && !escalations.empty()) {
                target_priority = escalations.back().priority;
            }
        } else {
            for (const auto& escalation : escalations) {
                if (remaining <= escalation.within) {
                    target_priority = escalation.priority;
                    break;
                }
            }
        }

        if (!target_priority) {
            continue;
        }

        const auto new_priority = std::max(record.priority, *target_priority);
        if (new_priority == record.priority) {
            continue;
        }

        record.priority = new_priority;
        repository_.upsert(record);
        emit(sink_, JobEvent{record.id, record.state,
                             std::string{"priority escalated to "} + std::to_string(new_priority)});
        ++updated;
    }

    return updated;
}

std::size_t JobService::resume_active_jobs() {
    const auto records = repository_.list(std::nullopt);
    std::size_t resumed = 0;
    for (const auto& record : records) {
        if (record.state != JobState::Validating && record.state != JobState::Rendering) {
            continue;
        }

        transition(record.id, JobState::Submitted);
        ++resumed;
    }
    return resumed;
}

std::size_t JobService::purge(JobState state, TimePoint older_than) {
    const auto records = repository_.list(state);
    std::size_t removed = 0;
    for (const auto& record : records) {
        const auto reference = record.updated_at.value_or(record.created_at);
        if (reference < older_than) {
            repository_.remove(record.id);
            ++removed;
        }
    }
    return removed;
}

void JobService::on_event(EventSink sink) {
    sink_ = std::move(sink);
}

void JobService::transition(std::string_view id, JobState state, std::optional<std::string> message) {
    repository_.transition(id, state, message);
    emit(sink_, JobEvent{std::string{id}, state, std::move(message)});
}

} // namespace ppp::core

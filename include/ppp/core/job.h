#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ppp::core {

using Clock = std::chrono::system_clock;
using TimePoint = std::chrono::time_point<Clock>;

/// Represents the lifecycle state for a processing job.
enum class JobState : std::uint8_t {
    Submitted = 0,
    Validating,
    Rendering,
    Exception,
    Completed,
    Cancelled
};

/// Describes the immutable payload for a job.
struct JobPayload {
    std::string source_path;
    std::optional<std::string> profile_name;
    std::vector<std::string> attachments;
    std::vector<std::string> tags;
};

/// Captures the persisted state for a job instance.
struct JobRecord {
    std::string id;
    JobState state{JobState::Submitted};
    JobPayload payload;
    TimePoint created_at{Clock::now()};
    std::optional<TimePoint> updated_at{};
    std::optional<std::string> correlation_id{};
    std::optional<std::string> error_message{};
    std::int32_t priority{0};
    std::uint32_t attempt_count{0};
    std::optional<TimePoint> last_attempt_at{};
    std::optional<TimePoint> due_at{};
};

[[nodiscard]] std::string_view to_string(JobState state) noexcept;
[[nodiscard]] std::optional<JobState> job_state_from_string(std::string_view state) noexcept;

} // namespace ppp::core

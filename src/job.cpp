#include "ppp/core/job.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace ppp::core {

namespace {
[[nodiscard]] std::string normalized(std::string_view value) {
    std::string result(value.begin(), value.end());
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}
} // namespace

std::string_view to_string(JobState state) noexcept {
    switch (state) {
    case JobState::Submitted:
        return "submitted";
    case JobState::Validating:
        return "validating";
    case JobState::Rendering:
        return "rendering";
    case JobState::Exception:
        return "exception";
    case JobState::Completed:
        return "completed";
    case JobState::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

std::optional<JobState> job_state_from_string(std::string_view state) noexcept {
    const auto value = normalized(state);
    if (value == "submitted") return JobState::Submitted;
    if (value == "validating") return JobState::Validating;
    if (value == "rendering") return JobState::Rendering;
    if (value == "exception") return JobState::Exception;
    if (value == "completed") return JobState::Completed;
    if (value == "cancelled") return JobState::Cancelled;
    return std::nullopt;
}

} // namespace ppp::core

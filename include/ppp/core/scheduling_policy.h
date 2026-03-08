#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace ppp::core {

/// Represents an escalation rule that boosts priority when a job is due within
/// the provided window. The window is measured in wall-clock seconds relative
/// to the reference time supplied when applying the policy.
struct SchedulingEscalation {
    std::chrono::seconds within{0};
    std::int32_t priority{0};
};

/// Describes a scheduling policy that can escalate priorities for jobs based on
/// their due timestamps.
struct SchedulingPolicy {
    std::vector<SchedulingEscalation> escalations;
    std::optional<std::int32_t> overdue_priority{};
    bool escalate_overdue{true};
};

} // namespace ppp::core


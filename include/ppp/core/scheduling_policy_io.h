#pragma once

#include "ppp/core/scheduling_policy.h"

#include <filesystem>
#include <istream>
#include <vector>

namespace ppp::core {

/// Represents the parsed policy alongside metadata that signals which
/// directives were explicitly configured. This allows callers to determine
/// whether the input only relied on defaults or provided concrete overrides.
struct SchedulingPolicyLoadResult {
    SchedulingPolicy policy;
    bool has_escalations{false};
    bool overdue_priority_specified{false};
    bool escalate_overdue_specified{false};
};

/// Parse a scheduling policy from a configuration stream. The expected format
/// is a series of key-value pairs, one per line, that describe escalation
/// windows and overdue behaviour. Lines beginning with `#`, `;`, or `//` are
/// treated as comments and ignored. Escalations are expressed as
/// `within-minutes=<minutes> priority=<priority>` (commas may be used instead of
/// spaces). Use `overdue=<priority>` to set the overdue escalation priority and
/// `escalate-overdue=false` to disable automatic promotion when no overdue
/// priority is specified.
[[nodiscard]] SchedulingPolicyLoadResult read_scheduling_policy_detailed(std::istream& input);
[[nodiscard]] SchedulingPolicy read_scheduling_policy(std::istream& input);

/// Load a scheduling policy from the provided configuration file path.
[[nodiscard]] SchedulingPolicyLoadResult load_scheduling_policy_detailed(const std::filesystem::path& path);
[[nodiscard]] SchedulingPolicy load_scheduling_policy(const std::filesystem::path& path);

/// Load and merge policies from the provided stack of paths. File paths are
/// processed in the order provided while directories are expanded into
/// lexicographically sorted policy files before being merged. Later directives
/// overwrite earlier ones for overdue behaviour while escalations accumulate.
[[nodiscard]] SchedulingPolicyLoadResult load_scheduling_policy_stack(const std::vector<std::filesystem::path>& paths);

/// Convenience helper to load every policy file in the specified directory.
/// Files are applied in lexicographic order and the result includes metadata
/// describing which directives were supplied.
[[nodiscard]] SchedulingPolicyLoadResult load_scheduling_policy_directory(const std::filesystem::path& directory);

} // namespace ppp::core


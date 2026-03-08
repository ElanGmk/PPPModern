#include "ppp/core/scheduling_policy_io.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ppp::core::SchedulingEscalation;
using ppp::core::SchedulingPolicy;
using ppp::core::SchedulingPolicyLoadResult;

std::string trim(std::string_view value) {
    auto begin = value.begin();
    auto end = value.end();
    while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
        ++begin;
    }
    while (begin != end) {
        const auto last = end - 1;
        if (!std::isspace(static_cast<unsigned char>(*last))) {
            break;
        }
        end = last;
    }
    return std::string{begin, end};
}

bool iequals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::optional<std::int64_t> parse_integer(std::string_view text) {
    std::int64_t value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

bool parse_boolean(std::string_view text) {
    if (iequals(text, "true") || iequals(text, "yes") || iequals(text, "1")) {
        return true;
    }
    if (iequals(text, "false") || iequals(text, "no") || iequals(text, "0")) {
        return false;
    }
    throw std::runtime_error("invalid boolean value in scheduling policy: " + std::string{text});
}

struct Token {
    std::string key;
    std::string value;
    bool has_value{false};
};

std::vector<Token> tokenize_line(const std::string& line) {
    std::string sanitized;
    sanitized.reserve(line.size());
    for (char ch : line) {
        if (ch == ',') {
            sanitized.push_back(' ');
        } else {
            sanitized.push_back(ch);
        }
    }

    std::istringstream iss{sanitized};
    std::string token;
    std::vector<Token> tokens;
    while (iss >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            tokens.push_back(Token{token, {}, false});
            continue;
        }
        auto key = trim(std::string_view{token.data(), eq});
        auto value = trim(std::string_view{token.data() + eq + 1, token.size() - eq - 1});
        tokens.push_back(Token{key, value, true});
    }
    return tokens;
}

std::int32_t parse_priority_token(std::string_view value) {
    const auto parsed = parse_integer(value);
    if (!parsed) {
        throw std::runtime_error("invalid priority in scheduling policy: " + std::string{value});
    }
    if (*parsed < std::numeric_limits<std::int32_t>::min() ||
        *parsed > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("priority out of range in scheduling policy: " + std::string{value});
    }
    return static_cast<std::int32_t>(*parsed);
}

std::chrono::minutes parse_minutes_token(std::string_view value) {
    const auto parsed = parse_integer(value);
    if (!parsed || *parsed < 0) {
        throw std::runtime_error("invalid minutes value in scheduling policy: " + std::string{value});
    }
    return std::chrono::minutes{*parsed};
}

SchedulingPolicyLoadResult parse_policy_stream(std::istream& input) {
    SchedulingPolicyLoadResult parsed;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        const auto comment_hash = line.find('#');
        const auto comment_semicolon = line.find(';');
        const auto comment_slashes = line.find("//");
        auto cut = std::string::npos;
        if (comment_hash != std::string::npos) {
            cut = comment_hash;
        }
        if (comment_semicolon != std::string::npos) {
            cut = std::min(cut, comment_semicolon);
        }
        if (comment_slashes != std::string::npos) {
            cut = std::min(cut, comment_slashes);
        }
        if (cut != std::string::npos) {
            line.resize(cut);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const auto tokens = tokenize_line(line);
        bool handled = false;
        bool escalation_token_seen = false;
        std::optional<std::chrono::minutes> within;
        std::optional<std::int32_t> priority;

        for (const auto& token : tokens) {
            const auto key = token.key;
            if (iequals(key, "within") || iequals(key, "within-minutes") || iequals(key, "minutes")) {
                if (!token.has_value) {
                    throw std::runtime_error("missing value for within-minutes on line " + std::to_string(line_number));
                }
                within = parse_minutes_token(token.value);
                escalation_token_seen = true;
                handled = true;
                continue;
            }
            if (iequals(key, "priority")) {
                if (!token.has_value) {
                    throw std::runtime_error("missing value for priority on line " + std::to_string(line_number));
                }
                priority = parse_priority_token(token.value);
                escalation_token_seen = true;
                handled = true;
                continue;
            }
            if (iequals(key, "overdue") || iequals(key, "overdue-priority")) {
                if (!token.has_value) {
                    throw std::runtime_error("missing value for overdue on line " + std::to_string(line_number));
                }
                parsed.policy.overdue_priority = parse_priority_token(token.value);
                parsed.overdue_priority_specified = true;
                handled = true;
                continue;
            }
            if (iequals(key, "escalate-overdue")) {
                if (!token.has_value) {
                    throw std::runtime_error("missing value for escalate-overdue on line " + std::to_string(line_number));
                }
                parsed.policy.escalate_overdue = parse_boolean(token.value);
                parsed.escalate_overdue_specified = true;
                handled = true;
                continue;
            }
            if (!token.has_value &&
                (iequals(key, "no-overdue-escalation") || iequals(key, "disable-overdue-escalation"))) {
                parsed.policy.escalate_overdue = false;
                parsed.escalate_overdue_specified = true;
                handled = true;
                continue;
            }
        }

        if (escalation_token_seen) {
            if (!within) {
                throw std::runtime_error("within-minutes not specified on line " + std::to_string(line_number));
            }
            if (!priority) {
                throw std::runtime_error("priority not specified on line " + std::to_string(line_number));
            }
            parsed.policy.escalations.push_back(SchedulingEscalation{
                .within = std::chrono::duration_cast<std::chrono::seconds>(*within),
                .priority = *priority,
            });
            parsed.has_escalations = true;
            continue;
        }

        if (!handled) {
            throw std::runtime_error("unrecognized scheduling policy directive on line " + std::to_string(line_number));
        }
    }

    return parsed;
}

SchedulingPolicyLoadResult load_policy_file(const std::filesystem::path& path) {
    std::ifstream file{path};
    if (!file) {
        throw std::runtime_error("failed to open scheduling policy file: " + path.string());
    }
    return parse_policy_stream(file);
}

bool is_policy_candidate(const std::filesystem::directory_entry& entry) {
    if (!entry.is_regular_file()) {
        return false;
    }
    const auto name = entry.path().filename().string();
    if (!name.empty() && name[0] == '.') {
        return false;
    }
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (extension.empty()) {
        return true;
    }
    static const std::vector<std::string> allowed = {".policy", ".cfg", ".conf", ".config", ".ini", ".txt"};
    return std::find(allowed.begin(), allowed.end(), extension) != allowed.end();
}

std::vector<std::filesystem::path> expand_directory(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) {
        throw std::runtime_error("scheduling policy directory does not exist: " + directory.string());
    }
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error("scheduling policy path is not a directory: " + directory.string());
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        if (is_policy_candidate(entry)) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

SchedulingPolicyLoadResult combine_policies(const std::vector<SchedulingPolicyLoadResult>& policies) {
    SchedulingPolicyLoadResult result;
    for (const auto& policy : policies) {
        if (policy.has_escalations) {
            result.policy.escalations.insert(result.policy.escalations.end(), policy.policy.escalations.begin(),
                                             policy.policy.escalations.end());
            result.has_escalations = true;
        }
        if (policy.overdue_priority_specified) {
            result.policy.overdue_priority = policy.policy.overdue_priority;
            result.overdue_priority_specified = true;
        }
        if (policy.escalate_overdue_specified) {
            result.policy.escalate_overdue = policy.policy.escalate_overdue;
            result.escalate_overdue_specified = true;
        }
    }
    return result;
}

SchedulingPolicyLoadResult load_policy_stack_impl(const std::vector<std::filesystem::path>& paths) {
    std::vector<SchedulingPolicyLoadResult> parsed_policies;
    for (const auto& path : paths) {
        if (std::filesystem::is_directory(path)) {
            const auto files = expand_directory(path);
            for (const auto& file : files) {
                parsed_policies.push_back(load_policy_file(file));
            }
            continue;
        }
        parsed_policies.push_back(load_policy_file(path));
    }
    return combine_policies(parsed_policies);
}

} // namespace

namespace ppp::core {

SchedulingPolicyLoadResult read_scheduling_policy_detailed(std::istream& input) {
    return parse_policy_stream(input);
}

SchedulingPolicy read_scheduling_policy(std::istream& input) {
    return parse_policy_stream(input).policy;
}

SchedulingPolicyLoadResult load_scheduling_policy_detailed(const std::filesystem::path& path) {
    return load_policy_file(path);
}

SchedulingPolicy load_scheduling_policy(const std::filesystem::path& path) {
    return load_scheduling_policy_detailed(path).policy;
}

SchedulingPolicyLoadResult load_scheduling_policy_stack(const std::vector<std::filesystem::path>& paths) {
    return load_policy_stack_impl(paths);
}

SchedulingPolicyLoadResult load_scheduling_policy_directory(const std::filesystem::path& directory) {
    return load_policy_stack_impl({directory});
}

} // namespace ppp::core


#include "ppp/core/job_repository.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace ppp::core {

namespace {

namespace fs = std::filesystem;
constexpr std::string_view kJobExtension = ".job";

[[nodiscard]] std::string encode_string(const std::string& value) {
    std::ostringstream oss;
    oss << std::quoted(value, '"', '\\');
    return oss.str();
}

[[nodiscard]] std::string encode_optional_string(const std::optional<std::string>& value) {
    if (!value) {
        return "null";
    }
    return encode_string(*value);
}

[[nodiscard]] std::optional<std::string> decode_string(std::string_view value) {
    std::istringstream iss(std::string{value});
    std::string result;
    if (!(iss >> std::quoted(result, '"', '\\'))) {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::optional<std::string> decode_optional_string(std::string_view value) {
    if (value == "null") {
        return std::nullopt;
    }
    return decode_string(value);
}

[[nodiscard]] std::optional<std::int64_t> parse_int64(std::string_view value) {
    std::int64_t result{};
    const auto begin = value.data();
    const auto end = begin + value.size();
    if (const auto [ptr, ec] = std::from_chars(begin, end, result); ec == std::errc{} && ptr == end) {
        return result;
    }
    return std::nullopt;
}

[[nodiscard]] std::string encode_time(const TimePoint& tp) {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(tp.time_since_epoch()).count();
    return std::to_string(micros);
}

[[nodiscard]] std::string encode_optional_time(const std::optional<TimePoint>& value) {
    if (!value) {
        return "null";
    }
    return encode_string(encode_time(*value));
}

[[nodiscard]] std::optional<TimePoint> decode_optional_time(std::string_view value, bool* valid = nullptr) {
    if (value == "null") {
        if (valid) {
            *valid = true;
        }
        return std::nullopt;
    }

    std::string decoded_storage;
    std::string_view numeric_value = value;
    if (!value.empty() && value.front() == '"') {
        auto decoded = decode_string(value);
        if (!decoded) {
            if (valid) {
                *valid = false;
            }
            return std::nullopt;
        }
        decoded_storage = std::move(*decoded);
        numeric_value = decoded_storage;
    }

    const auto micros = parse_int64(numeric_value);
    if (!micros) {
        if (valid) {
            *valid = false;
        }
        return std::nullopt;
    }

    if (valid) {
        *valid = true;
    }
    return TimePoint{std::chrono::microseconds{*micros}};
}

[[nodiscard]] bool due_precedes(const std::optional<TimePoint>& lhs, const std::optional<TimePoint>& rhs) {
    if (lhs && rhs) {
        return *lhs < *rhs;
    }
    if (lhs && !rhs) {
        return true;
    }
    if (!lhs && rhs) {
        return false;
    }
    return false;
}

[[nodiscard]] bool better_candidate(const JobRecord& lhs, const JobRecord& rhs) {
    if (lhs.priority != rhs.priority) {
        return lhs.priority > rhs.priority;
    }
    if (lhs.due_at != rhs.due_at) {
        if (due_precedes(lhs.due_at, rhs.due_at)) {
            return true;
        }
        if (due_precedes(rhs.due_at, lhs.due_at)) {
            return false;
        }
    }
    if (lhs.created_at != rhs.created_at) {
        return lhs.created_at < rhs.created_at;
    }
    return lhs.id < rhs.id;
}

[[nodiscard]] std::optional<TimePoint> decode_time(std::string_view value) {
    const auto micros = parse_int64(value);
    if (!micros) {
        return std::nullopt;
    }
    return TimePoint{std::chrono::microseconds{*micros}};
}

[[nodiscard]] std::string attachment_key(std::size_t index) {
    return "attachment_" + std::to_string(index);
}

[[nodiscard]] std::string tag_key(std::size_t index) { return "tag_" + std::to_string(index); }

[[nodiscard]] fs::path job_path(const fs::path& root, std::string_view id) {
    fs::path filename{std::string{id}};
    filename += kJobExtension;
    return root / filename;
}

[[nodiscard]] std::optional<JobRecord> read_record(const fs::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        auto key = line.substr(0, pos);
        auto value = line.substr(pos + 1);
        values[std::move(key)] = std::move(value);
    }

    const auto get_value = [&values](std::string_view key) -> std::optional<std::string_view> {
        const auto it = values.find(std::string{key});
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    };

    const auto id_value = get_value("id");
    const auto state_value = get_value("state");
    const auto created_value = get_value("created_at");
    const auto updated_value = get_value("updated_at");
    const auto source_value = get_value("source_path");
    const auto profile_value = get_value("profile_name");
    const auto correlation_value = get_value("correlation_id");
    const auto error_value = get_value("error_message");
    const auto attachments_value = get_value("attachments_count");
    const auto tags_value = get_value("tags_count");
    const auto attempts_value = get_value("attempt_count");
    const auto priority_value = get_value("priority");
    const auto last_attempt_value = get_value("last_attempt_at");
    const auto due_value = get_value("due_at");

    if (!id_value || !state_value || !created_value || !updated_value || !source_value || !attachments_value) {
        return std::nullopt;
    }

    auto id_string = decode_string(*id_value);
    const auto state = job_state_from_string(*state_value);
    const auto created_at = decode_time(*created_value);
    bool updated_ok = true;
    const auto updated_at = decode_optional_time(*updated_value, &updated_ok);
    auto source_path = decode_string(*source_value);
    const auto attachments_count = parse_int64(*attachments_value);
    std::int64_t tags_count_value = 0;
    if (tags_value) {
        const auto parsed = parse_int64(*tags_value);
        if (!parsed) {
            return std::nullopt;
        }
        tags_count_value = *parsed;
    }

    if (!id_string || !state || !created_at || !updated_ok || !source_path || !attachments_count ||
        *attachments_count < 0 || tags_count_value < 0) {
        return std::nullopt;
    }

    std::uint32_t attempt_count = 0;
    if (attempts_value) {
        const auto attempts = parse_int64(*attempts_value);
        if (!attempts || *attempts < 0) {
            return std::nullopt;
        }
        attempt_count = static_cast<std::uint32_t>(*attempts);
    }

    std::int32_t priority = 0;
    if (priority_value) {
        const auto parsed_priority = parse_int64(*priority_value);
        if (!parsed_priority) {
            return std::nullopt;
        }
        priority = static_cast<std::int32_t>(*parsed_priority);
    }

    JobRecord record;
    record.id = std::move(*id_string);
    record.state = *state;
    record.created_at = *created_at;
    record.updated_at = updated_at;
    record.payload.source_path = std::move(*source_path);
    record.priority = priority;
    record.attempt_count = attempt_count;

    if (last_attempt_value) {
        bool last_attempt_ok = true;
        const auto last_attempt = decode_optional_time(*last_attempt_value, &last_attempt_ok);
        if (!last_attempt_ok) {
            return std::nullopt;
        }
        record.last_attempt_at = last_attempt;
    }

    if (due_value) {
        bool due_ok = true;
        const auto due = decode_optional_time(*due_value, &due_ok);
        if (!due_ok) {
            return std::nullopt;
        }
        record.due_at = due;
    }

    if (correlation_value) {
        record.correlation_id = decode_optional_string(*correlation_value);
    }
    if (error_value) {
        record.error_message = decode_optional_string(*error_value);
    }
    if (profile_value) {
        record.payload.profile_name = decode_optional_string(*profile_value);
    }

    record.payload.attachments.clear();
    record.payload.attachments.reserve(static_cast<std::size_t>(*attachments_count));
    for (std::size_t i = 0; i < static_cast<std::size_t>(*attachments_count); ++i) {
        const auto key = attachment_key(i);
        const auto value = get_value(key);
        if (!value) {
            return std::nullopt;
        }
        auto attachment = decode_string(*value);
        if (!attachment) {
            return std::nullopt;
        }
        record.payload.attachments.push_back(std::move(*attachment));
    }

    record.payload.tags.clear();
    record.payload.tags.reserve(static_cast<std::size_t>(tags_count_value));
    for (std::size_t i = 0; i < static_cast<std::size_t>(tags_count_value); ++i) {
        const auto key = tag_key(i);
        const auto value = get_value(key);
        if (!value) {
            return std::nullopt;
        }
        auto tag = decode_string(*value);
        if (!tag) {
            return std::nullopt;
        }
        record.payload.tags.push_back(std::move(*tag));
    }

    return record;
}

void write_record(const fs::path& path, const JobRecord& record) {
    const auto temp_path = path.string() + ".tmp";
    std::ofstream output(temp_path, std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to write job record " + record.id + " to " + temp_path);
    }

    output << "id=" << encode_string(record.id) << '\n';
    output << "state=" << to_string(record.state) << '\n';
    output << "created_at=" << encode_time(record.created_at) << '\n';
    output << "updated_at=" << encode_optional_time(record.updated_at) << '\n';
    output << "correlation_id=" << encode_optional_string(record.correlation_id) << '\n';
    output << "error_message=" << encode_optional_string(record.error_message) << '\n';
    output << "source_path=" << encode_string(record.payload.source_path) << '\n';
    output << "profile_name=" << encode_optional_string(record.payload.profile_name) << '\n';
    output << "attachments_count=" << record.payload.attachments.size() << '\n';
    output << "tags_count=" << record.payload.tags.size() << '\n';
    output << "attempt_count=" << record.attempt_count << '\n';
    output << "priority=" << record.priority << '\n';
    output << "last_attempt_at=" << encode_optional_time(record.last_attempt_at) << '\n';
    output << "due_at=" << encode_optional_time(record.due_at) << '\n';
    for (std::size_t i = 0; i < record.payload.attachments.size(); ++i) {
        output << attachment_key(i) << '=' << encode_string(record.payload.attachments[i]) << '\n';
    }
    for (std::size_t i = 0; i < record.payload.tags.size(); ++i) {
        output << tag_key(i) << '=' << encode_string(record.payload.tags[i]) << '\n';
    }
    output.close();

    std::error_code ec;
    fs::rename(temp_path, path, ec);
    if (ec) {
        fs::remove(temp_path);
        throw std::runtime_error("failed to commit job record " + record.id + ": " + ec.message());
    }
}

} // namespace

struct InMemoryJobRepository::Impl {
    mutable std::mutex mutex;
    std::unordered_map<std::string, JobRecord> jobs;
};

InMemoryJobRepository::InMemoryJobRepository() : impl_(std::make_unique<Impl>()) {}
InMemoryJobRepository::~InMemoryJobRepository() = default;

std::optional<JobRecord> InMemoryJobRepository::fetch(std::string_view id) const {
    std::scoped_lock lock(impl_->mutex);
    if (auto it = impl_->jobs.find(std::string{id}); it != impl_->jobs.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<JobRecord> InMemoryJobRepository::list(std::optional<JobState> state_filter) const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<JobRecord> result;
    result.reserve(impl_->jobs.size());
    for (const auto& [_, record] : impl_->jobs) {
        if (!state_filter || record.state == *state_filter) {
            result.push_back(record);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return better_candidate(lhs, rhs);
    });
    return result;
}

std::optional<JobRecord> InMemoryJobRepository::claim_next_submitted(JobState next_state) {
    std::scoped_lock lock(impl_->mutex);
    JobRecord* candidate = nullptr;
    for (auto& [_, record] : impl_->jobs) {
        if (record.state != JobState::Submitted) {
            continue;
        }
        if (!candidate || better_candidate(record, *candidate)) {
            candidate = &record;
        }
    }

    if (!candidate) {
        return std::nullopt;
    }

    const auto now = Clock::now();
    candidate->state = next_state;
    candidate->updated_at = now;
    candidate->last_attempt_at = now;
    candidate->error_message.reset();
    ++candidate->attempt_count;
    return *candidate;
}

void InMemoryJobRepository::upsert(JobRecord record) {
    std::scoped_lock lock(impl_->mutex);
    record.updated_at = Clock::now();
    impl_->jobs[record.id] = std::move(record);
}

void InMemoryJobRepository::transition(std::string_view id, JobState state,
                                       std::optional<std::string> error) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->jobs.find(std::string{id});
    if (it == impl_->jobs.end()) {
        return;
    }
    it->second.state = state;
    it->second.updated_at = Clock::now();
    it->second.error_message = std::move(error);
}

void InMemoryJobRepository::remove(std::string_view id) {
    std::scoped_lock lock(impl_->mutex);
    impl_->jobs.erase(std::string{id});
}

struct FileJobRepository::Impl {
    fs::path root;
    mutable std::mutex mutex;

    explicit Impl(fs::path root_directory) : root(std::move(root_directory)) {
        if (root.empty()) {
            throw std::invalid_argument("root directory must not be empty");
        }
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) {
            throw std::runtime_error("failed to initialize repository directory " + root.string() + ": " + ec.message());
        }
    }
};

FileJobRepository::FileJobRepository(std::filesystem::path root_directory)
    : impl_(std::make_unique<Impl>(std::move(root_directory))) {}
FileJobRepository::~FileJobRepository() = default;

std::optional<JobRecord> FileJobRepository::fetch(std::string_view id) const {
    std::scoped_lock lock(impl_->mutex);
    return read_record(job_path(impl_->root, id));
}

std::vector<JobRecord> FileJobRepository::list(std::optional<JobState> state_filter) const {
    std::scoped_lock lock(impl_->mutex);
    std::vector<JobRecord> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(impl_->root, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == kJobExtension) {
            if (auto record = read_record(entry.path())) {
                if (!state_filter || record->state == *state_filter) {
                    result.push_back(std::move(*record));
                }
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return better_candidate(lhs, rhs);
    });
    return result;
}

std::optional<JobRecord> FileJobRepository::claim_next_submitted(JobState next_state) {
    std::scoped_lock lock(impl_->mutex);
    std::error_code ec;
    bool found = false;
    fs::path selected_path;
    JobRecord selected_record;

    for (const auto& entry : fs::directory_iterator(impl_->root, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != kJobExtension) {
            continue;
        }
        auto record = read_record(entry.path());
        if (!record || record->state != JobState::Submitted) {
            continue;
        }
        if (!found || better_candidate(*record, selected_record)) {
            selected_path = entry.path();
            selected_record = std::move(*record);
            found = true;
        }
    }

    if (!found) {
        return std::nullopt;
    }

    const auto now = Clock::now();
    selected_record.state = next_state;
    selected_record.updated_at = now;
    selected_record.last_attempt_at = now;
    selected_record.error_message.reset();
    ++selected_record.attempt_count;
    write_record(selected_path, selected_record);
    return selected_record;
}

void FileJobRepository::upsert(JobRecord record) {
    std::scoped_lock lock(impl_->mutex);
    record.updated_at = Clock::now();
    const auto path = job_path(impl_->root, record.id);
    write_record(path, record);
}

void FileJobRepository::transition(std::string_view id, JobState state,
                                   std::optional<std::string> error) {
    std::scoped_lock lock(impl_->mutex);
    const auto path = job_path(impl_->root, id);
    auto record = read_record(path);
    if (!record) {
        return;
    }
    record->state = state;
    record->updated_at = Clock::now();
    record->error_message = std::move(error);
    write_record(path, *record);
}

void FileJobRepository::remove(std::string_view id) {
    std::scoped_lock lock(impl_->mutex);
    const auto path = job_path(impl_->root, id);
    std::error_code ec;
    fs::remove(path, ec);
}

} // namespace ppp::core

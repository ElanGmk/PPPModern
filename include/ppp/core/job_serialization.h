#pragma once

#include "ppp/core/job.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ppp::core {

struct JobSummary;

[[nodiscard]] std::string job_record_to_json(const JobRecord& record);
[[nodiscard]] std::string job_records_to_json_array(const std::vector<JobRecord>& records);
[[nodiscard]] std::string job_summary_to_json(const JobSummary& summary);
[[nodiscard]] bool write_job_records_to_json_file(const std::vector<JobRecord>& records,
                                                  const std::filesystem::path& path);

[[nodiscard]] std::optional<JobRecord> job_record_from_json(std::string_view json);
[[nodiscard]] std::optional<std::vector<JobRecord>> job_records_from_json_array(std::string_view json);
[[nodiscard]] std::optional<std::vector<JobRecord>> read_job_records_from_json_file(const std::filesystem::path& path);

} // namespace ppp::core


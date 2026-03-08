#pragma once

#include "ppp/core/processing_config.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ppp::core {

[[nodiscard]] std::string processing_profile_to_json(const ProcessingProfile& profile);
[[nodiscard]] std::optional<ProcessingProfile> processing_profile_from_json(std::string_view json);

[[nodiscard]] bool write_processing_profile(const ProcessingProfile& profile, const std::filesystem::path& path);
[[nodiscard]] std::optional<ProcessingProfile> read_processing_profile(const std::filesystem::path& path);

} // namespace ppp::core

#pragma once

#include "ppp/core/image.h"
#include "ppp/core/processing_config.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ppp::core::output {

// ---------------------------------------------------------------------------
// Output result
// ---------------------------------------------------------------------------

struct OutputResult {
    bool success{false};
    std::filesystem::path output_path;  ///< Path of the written file.
    std::string error;                  ///< Error message if !success.
    std::string format;                 ///< Format written ("tiff", "bmp").
};

// ---------------------------------------------------------------------------
// Output path resolution
// ---------------------------------------------------------------------------

/// Resolve the output file path based on OutputConfig and the source path.
///
/// If save_to_different_dir is true, uses output_directory as the base.
/// Otherwise, writes next to the source file.  Applies new_extension if set.
/// Handles conflict_policy (report error vs overwrite existing).
///
/// @param source_path   Original source image file path.
/// @param config        Output configuration.
/// @param suffix        Optional suffix to append before extension (e.g. "_processed").
/// @return Resolved output path, or empty path on error.
[[nodiscard]] std::filesystem::path resolve_output_path(
    const std::filesystem::path& source_path,
    const OutputConfig& config,
    const std::string& suffix = "");

// ---------------------------------------------------------------------------
// Write processed image
// ---------------------------------------------------------------------------

/// Write a processed image to disk according to OutputConfig.
///
/// Selects the output format based on raster_format and tiff_output settings.
/// For TIFF output, uses the configured compression (Group4 for BW1, LZW for
/// grayscale/color).  For BMP, writes uncompressed.
///
/// @param image         Processed image to write.
/// @param source_path   Original source file path (for output path resolution).
/// @param config        Output configuration.
/// @param suffix        Optional suffix for the output filename.
/// @return Result with success flag, output path, and format.
[[nodiscard]] OutputResult write_output(
    const Image& image,
    const std::filesystem::path& source_path,
    const OutputConfig& config,
    const std::string& suffix = "");

/// Write a processed image to a specific path, auto-detecting format from extension.
///
/// Supports .tif/.tiff (TIFF), .bmp (BMP).  Falls back to TIFF for unknown
/// extensions.
///
/// @param image   Processed image.
/// @param path    Output file path.
/// @param config  Output configuration (for compression settings).
/// @return Result with success flag.
[[nodiscard]] OutputResult write_output_to(
    const Image& image,
    const std::filesystem::path& path,
    const OutputConfig& config = {});

/// Write multiple images as a multi-page TIFF.
///
/// @param images  Vector of processed images.
/// @param path    Output file path.
/// @param config  Output configuration.
/// @return Result with success flag.
[[nodiscard]] OutputResult write_multipage_output(
    const std::vector<Image>& images,
    const std::filesystem::path& path,
    const OutputConfig& config = {});

} // namespace ppp::core::output

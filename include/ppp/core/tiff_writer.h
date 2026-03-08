#pragma once

#include "ppp/core/image.h"
#include "ppp/core/tiff.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ppp::core::tiff {

// ---------------------------------------------------------------------------
// TIFF write options
// ---------------------------------------------------------------------------

struct WriteOptions {
    Compression compression{Compression::Uncompressed};
    Photometric photometric{Photometric::BlackIsZero};

    /// Software tag string (written to the TIFF header).
    std::string software{"PPP Modern"};

    /// For multi-page output, set to true on all pages after the first.
    bool append{false};
};

// ---------------------------------------------------------------------------
// TIFF writer — produces baseline TIFF files from Image objects
// ---------------------------------------------------------------------------

/// Write a single Image as a TIFF file.
///
/// Supports:
///   - BW1:   WhiteIsZero/BlackIsZero, Uncompressed or Group4Fax or PackBits
///   - Gray8: BlackIsZero/WhiteIsZero, Uncompressed or PackBits or LZW
///   - RGB24: RGB, Uncompressed or PackBits or LZW
///
/// @param image    Source image.
/// @param path     Output file path.
/// @param options  Compression and metadata options.
/// @return true on success.
[[nodiscard]] bool write_tiff(const Image& image,
                              const std::filesystem::path& path,
                              const WriteOptions& options = {});

/// Write a single Image as a TIFF to a memory buffer.
[[nodiscard]] std::vector<std::uint8_t> write_tiff_to_memory(
    const Image& image,
    const WriteOptions& options = {});

/// Write multiple Images as pages in a single multi-page TIFF file.
///
/// @param images   Vector of images (one per page).
/// @param path     Output file path.
/// @param options  Base options (append flag is managed internally).
/// @return true on success.
[[nodiscard]] bool write_multipage_tiff(
    const std::vector<Image>& images,
    const std::filesystem::path& path,
    const WriteOptions& options = {});

// ---------------------------------------------------------------------------
// Round-trip helper — read TIFF pixel data into an Image
// ---------------------------------------------------------------------------

/// Read TIFF pixel data into an Image.
///
/// Currently supports uncompressed BW1, Gray8, and RGB24 images.
/// Uses the Structure parser to get IFD metadata, then extracts pixel
/// strips from the raw file data.
///
/// @param data  Raw TIFF file bytes.
/// @param size  Number of bytes.
/// @param page  0-based page index to extract.
/// @return Image with pixel data, or empty Image on failure.
[[nodiscard]] Image read_tiff_image(const std::uint8_t* data,
                                    std::size_t size,
                                    std::size_t page = 0);

/// Read a TIFF file into an Image.
[[nodiscard]] Image read_tiff_image_file(const std::filesystem::path& path,
                                         std::size_t page = 0);

} // namespace ppp::core::tiff

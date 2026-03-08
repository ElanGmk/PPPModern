#pragma once

#include "ppp/core/image.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ppp::core::bmp {

// ---------------------------------------------------------------------------
// BMP read
// ---------------------------------------------------------------------------

/// Read a BMP file from raw bytes into an Image.
///
/// Supports:
///   - 1-bit (BW1), 8-bit grayscale (Gray8), 24-bit (RGB24), 32-bit (RGBA32)
///   - Uncompressed (BI_RGB) and RLE8 compression
///   - Top-down and bottom-up row ordering
///   - OS/2 and Windows BITMAPINFOHEADER variants
///
/// @param data  Raw BMP file bytes.
/// @param size  Number of bytes.
/// @return Image with pixel data, or empty Image on failure.
[[nodiscard]] Image read_bmp(const std::uint8_t* data, std::size_t size);

/// Read a BMP file from disk.
[[nodiscard]] Image read_bmp_file(const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// BMP write
// ---------------------------------------------------------------------------

struct WriteOptions {
    /// DPI override.  If <= 0, uses image DPI (or defaults to 96).
    double dpi_x{0};
    double dpi_y{0};
};

/// Write an Image as a BMP file.
///
/// Supports BW1 (1-bit), Gray8 (8-bit with grayscale palette),
/// RGB24 (24-bit), and RGBA32 (32-bit).  Always writes uncompressed
/// bottom-up BMPs compatible with all Windows versions.
///
/// @param image    Source image.
/// @param path     Output file path.
/// @param options  Optional write parameters.
/// @return true on success.
[[nodiscard]] bool write_bmp(const Image& image,
                              const std::filesystem::path& path,
                              const WriteOptions& options = {});

/// Write an Image as a BMP to a memory buffer.
[[nodiscard]] std::vector<std::uint8_t> write_bmp_to_memory(
    const Image& image,
    const WriteOptions& options = {});

} // namespace ppp::core::bmp

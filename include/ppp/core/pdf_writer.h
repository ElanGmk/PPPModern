#pragma once

#include "ppp/core/image.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ppp::core::pdf {

// ---------------------------------------------------------------------------
// PDF write options
// ---------------------------------------------------------------------------

struct PdfWriteOptions {
    /// Page width in points (72 points = 1 inch).  0 = derive from image DPI.
    double page_width_pt{0.0};
    /// Page height in points.  0 = derive from image DPI.
    double page_height_pt{0.0};
    /// Producer metadata string.
    std::string producer{"PPP Modern"};
};

// ---------------------------------------------------------------------------
// PDF writer — produces PDF files with embedded raster images
// ---------------------------------------------------------------------------

/// Write a single image as a one-page PDF file.
///
/// The image is embedded as a raw (uncompressed) raster stream.
/// Supports BW1, Gray8, and RGB24 pixel formats.  RGBA32 images are
/// converted to RGB24 before embedding (alpha is dropped).
///
/// If page dimensions are zero in options, they are derived from the
/// image dimensions and DPI (defaulting to 72 DPI if not set).
///
/// @param image    Source image.
/// @param path     Output file path.
/// @param options  PDF metadata and page size options.
/// @return true on success.
[[nodiscard]] bool write_pdf(const Image& image,
                              const std::filesystem::path& path,
                              const PdfWriteOptions& options = {});

/// Write a single image as a PDF to a memory buffer.
[[nodiscard]] std::vector<std::uint8_t> write_pdf_to_memory(
    const Image& image,
    const PdfWriteOptions& options = {});

/// Write multiple images as pages in a single PDF file.
///
/// @param images   Vector of images (one per page).
/// @param path     Output file path.
/// @param options  PDF metadata and page size options.
/// @return true on success.
[[nodiscard]] bool write_multipage_pdf(
    const std::vector<Image>& images,
    const std::filesystem::path& path,
    const PdfWriteOptions& options = {});

} // namespace ppp::core::pdf

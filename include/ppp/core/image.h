#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace ppp::core {

// ---------------------------------------------------------------------------
// Pixel format
// ---------------------------------------------------------------------------

/// Bits-per-pixel format for raster images.
enum class PixelFormat : std::uint8_t {
    BW1 = 1,      ///< 1 bit per pixel, packed MSB-first (bitonal).
    Gray8 = 8,    ///< 8 bits per pixel, grayscale.
    RGB24 = 24,   ///< 24 bits per pixel, 3 channels (R, G, B).
    RGBA32 = 32   ///< 32 bits per pixel, 4 channels (R, G, B, A).
};

/// Number of bytes per pixel (for formats >= 8 bpp).
/// Returns 0 for BW1 since it is sub-byte packed.
[[nodiscard]] constexpr std::size_t bytes_per_pixel(PixelFormat fmt) noexcept {
    switch (fmt) {
        case PixelFormat::BW1:    return 0;
        case PixelFormat::Gray8:  return 1;
        case PixelFormat::RGB24:  return 3;
        case PixelFormat::RGBA32: return 4;
    }
    return 0;
}

/// Compute the minimum stride (bytes per row) for the given width and format.
/// Rows are padded to 4-byte boundaries (DWORD-aligned), matching Windows
/// bitmap convention and TIFF default strip alignment.
[[nodiscard]] constexpr std::int32_t compute_stride(std::int32_t width,
                                                     PixelFormat fmt) noexcept {
    std::int32_t bits_per_row = width * static_cast<std::int32_t>(fmt);
    std::int32_t bytes_per_row = (bits_per_row + 7) / 8;
    return (bytes_per_row + 3) & ~3;  // Round up to 4-byte boundary.
}

// ---------------------------------------------------------------------------
// Image — owning raster pixel buffer
// ---------------------------------------------------------------------------

/// In-memory raster image with value semantics (deep-copy on copy).
///
/// Pixel data is stored in a contiguous buffer, row-major, top-to-bottom.
/// Each row is DWORD-aligned (stride is a multiple of 4 bytes).
///
/// For BW1 images, bits are packed MSB-first within each byte (bit 7 of
/// byte 0 is the leftmost pixel).  A set bit (1) represents foreground
/// (black) by convention, matching TIFF PhotometricInterpretation=0
/// (WhiteIsZero).
class Image {
public:
    /// Construct an empty (0x0) image.
    Image() = default;

    /// Construct an image of the given size, filled with zero bytes.
    Image(std::int32_t width, std::int32_t height, PixelFormat format);

    /// Construct an image of the given size with explicit resolution.
    Image(std::int32_t width, std::int32_t height, PixelFormat format,
          double dpi_x, double dpi_y);

    // Rule of five — deep copy, default move.
    Image(const Image& other);
    Image& operator=(const Image& other);
    Image(Image&&) noexcept = default;
    Image& operator=(Image&&) noexcept = default;
    ~Image() = default;

    // -----------------------------------------------------------------------
    // Properties
    // -----------------------------------------------------------------------

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] PixelFormat format() const noexcept { return format_; }
    [[nodiscard]] std::int32_t stride() const noexcept { return stride_; }
    [[nodiscard]] double dpi_x() const noexcept { return dpi_x_; }
    [[nodiscard]] double dpi_y() const noexcept { return dpi_y_; }
    [[nodiscard]] bool empty() const noexcept { return width_ <= 0 || height_ <= 0; }

    void set_dpi(double x, double y) noexcept { dpi_x_ = x; dpi_y_ = y; }

    // -----------------------------------------------------------------------
    // Pixel data access
    // -----------------------------------------------------------------------

    /// Pointer to the start of the given row (0-based, top = 0).
    [[nodiscard]] std::uint8_t* row(std::int32_t y) noexcept {
        return data_.data() + static_cast<std::ptrdiff_t>(y) * stride_;
    }
    [[nodiscard]] const std::uint8_t* row(std::int32_t y) const noexcept {
        return data_.data() + static_cast<std::ptrdiff_t>(y) * stride_;
    }

    /// Raw pixel buffer.
    [[nodiscard]] std::uint8_t* data() noexcept { return data_.data(); }
    [[nodiscard]] const std::uint8_t* data() const noexcept { return data_.data(); }
    [[nodiscard]] std::size_t data_size() const noexcept { return data_.size(); }

    // -----------------------------------------------------------------------
    // BW1 pixel access helpers
    // -----------------------------------------------------------------------

    /// Get a single pixel in a BW1 image (returns 0 or 1).
    [[nodiscard]] int get_bw_pixel(std::int32_t x, std::int32_t y) const noexcept {
        const auto* r = row(y);
        return (r[x >> 3] >> (7 - (x & 7))) & 1;
    }

    /// Set a single pixel in a BW1 image to 0 or 1.
    void set_bw_pixel(std::int32_t x, std::int32_t y, int value) noexcept {
        auto* r = row(y);
        int byte_idx = x >> 3;
        int bit_idx = 7 - (x & 7);
        if (value)
            r[byte_idx] |= static_cast<std::uint8_t>(1 << bit_idx);
        else
            r[byte_idx] &= static_cast<std::uint8_t>(~(1 << bit_idx));
    }

    // -----------------------------------------------------------------------
    // Basic operations (return new images)
    // -----------------------------------------------------------------------

    /// Fill the entire image with a byte value (0x00 = white for BW1/Gray8,
    /// 0xFF = black for BW1, white for Gray8/RGB24).
    void fill(std::uint8_t value) noexcept;

    /// Extract a rectangular sub-region as a new image.
    /// Coordinates are clamped to image bounds.
    [[nodiscard]] Image crop(std::int32_t x, std::int32_t y,
                             std::int32_t w, std::int32_t h) const;

    /// Rotate by 90 degrees clockwise.
    [[nodiscard]] Image rotate_cw90() const;

    /// Rotate by 90 degrees counter-clockwise.
    [[nodiscard]] Image rotate_ccw90() const;

    /// Rotate by 180 degrees.
    [[nodiscard]] Image rotate_180() const;

    /// Create a new image with padding added around all edges.
    /// The padding area is filled with `fill_value`.
    [[nodiscard]] Image pad(std::int32_t top, std::int32_t left,
                            std::int32_t right, std::int32_t bottom,
                            std::uint8_t fill_value = 0) const;

    /// Copy a region from `src` into this image at position (dst_x, dst_y).
    /// Clips to the bounds of both images.
    void blit(const Image& src, std::int32_t dst_x, std::int32_t dst_y);

    /// Invert all pixel values (bitwise NOT on the entire buffer).
    void invert() noexcept;

    // -----------------------------------------------------------------------
    // Conversion
    // -----------------------------------------------------------------------

    /// Convert to a different pixel format.
    /// Supported conversions:
    ///   BW1 → Gray8, RGB24
    ///   Gray8 → BW1 (threshold), RGB24
    ///   RGB24 → Gray8 (luminance), BW1 (via Gray8 threshold)
    [[nodiscard]] Image convert(PixelFormat target,
                                std::uint8_t bw_threshold = 128) const;

private:
    std::int32_t width_{0};
    std::int32_t height_{0};
    PixelFormat format_{PixelFormat::Gray8};
    std::int32_t stride_{0};
    double dpi_x_{0.0};
    double dpi_y_{0.0};
    std::vector<std::uint8_t> data_;
};

} // namespace ppp::core

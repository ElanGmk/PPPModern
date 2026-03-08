#include "ppp/core/image.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace ppp::core {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Image::Image(std::int32_t width, std::int32_t height, PixelFormat format)
    : width_(width),
      height_(height),
      format_(format),
      stride_(compute_stride(width, format)),
      data_(static_cast<std::size_t>(stride_) * height, 0) {}

Image::Image(std::int32_t width, std::int32_t height, PixelFormat format,
             double dpi_x, double dpi_y)
    : width_(width),
      height_(height),
      format_(format),
      stride_(compute_stride(width, format)),
      dpi_x_(dpi_x),
      dpi_y_(dpi_y),
      data_(static_cast<std::size_t>(stride_) * height, 0) {}

// ---------------------------------------------------------------------------
// Copy
// ---------------------------------------------------------------------------

Image::Image(const Image& other)
    : width_(other.width_),
      height_(other.height_),
      format_(other.format_),
      stride_(other.stride_),
      dpi_x_(other.dpi_x_),
      dpi_y_(other.dpi_y_),
      data_(other.data_) {}

Image& Image::operator=(const Image& other) {
    if (this != &other) {
        width_ = other.width_;
        height_ = other.height_;
        format_ = other.format_;
        stride_ = other.stride_;
        dpi_x_ = other.dpi_x_;
        dpi_y_ = other.dpi_y_;
        data_ = other.data_;
    }
    return *this;
}

// ---------------------------------------------------------------------------
// fill
// ---------------------------------------------------------------------------

void Image::fill(std::uint8_t value) noexcept {
    std::memset(data_.data(), value, data_.size());
}

// ---------------------------------------------------------------------------
// crop
// ---------------------------------------------------------------------------

Image Image::crop(std::int32_t x, std::int32_t y,
                  std::int32_t w, std::int32_t h) const {
    // Clamp to image bounds.
    x = std::max(x, std::int32_t{0});
    y = std::max(y, std::int32_t{0});
    w = std::min(w, width_ - x);
    h = std::min(h, height_ - y);
    if (w <= 0 || h <= 0) return {};

    Image result(w, h, format_, dpi_x_, dpi_y_);

    if (format_ == PixelFormat::BW1) {
        // Bit-level copy for 1bpp images.
        for (std::int32_t row = 0; row < h; ++row) {
            for (std::int32_t col = 0; col < w; ++col) {
                int px = get_bw_pixel(x + col, y + row);
                result.set_bw_pixel(col, row, px);
            }
        }
    } else {
        std::size_t bpp = bytes_per_pixel(format_);
        std::size_t copy_bytes = static_cast<std::size_t>(w) * bpp;
        for (std::int32_t row = 0; row < h; ++row) {
            const auto* src = this->row(y + row) + static_cast<std::size_t>(x) * bpp;
            auto* dst = result.row(row);
            std::memcpy(dst, src, copy_bytes);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// rotate_cw90
// ---------------------------------------------------------------------------

Image Image::rotate_cw90() const {
    if (empty()) return {};

    // New dimensions: width ← height, height ← width.
    Image result(height_, width_, format_, dpi_y_, dpi_x_);

    if (format_ == PixelFormat::BW1) {
        for (std::int32_t sy = 0; sy < height_; ++sy) {
            for (std::int32_t sx = 0; sx < width_; ++sx) {
                // (sx, sy) → (height_ - 1 - sy, sx) rotated CW90 in dest.
                int px = get_bw_pixel(sx, sy);
                result.set_bw_pixel(height_ - 1 - sy, sx, px);
            }
        }
    } else {
        std::size_t bpp = bytes_per_pixel(format_);
        for (std::int32_t sy = 0; sy < height_; ++sy) {
            const auto* src_row = row(sy);
            for (std::int32_t sx = 0; sx < width_; ++sx) {
                std::int32_t dx = height_ - 1 - sy;
                std::int32_t dy = sx;
                std::memcpy(result.row(dy) + static_cast<std::size_t>(dx) * bpp,
                            src_row + static_cast<std::size_t>(sx) * bpp, bpp);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// rotate_ccw90
// ---------------------------------------------------------------------------

Image Image::rotate_ccw90() const {
    if (empty()) return {};

    Image result(height_, width_, format_, dpi_y_, dpi_x_);

    if (format_ == PixelFormat::BW1) {
        for (std::int32_t sy = 0; sy < height_; ++sy) {
            for (std::int32_t sx = 0; sx < width_; ++sx) {
                int px = get_bw_pixel(sx, sy);
                result.set_bw_pixel(sy, width_ - 1 - sx, px);
            }
        }
    } else {
        std::size_t bpp = bytes_per_pixel(format_);
        for (std::int32_t sy = 0; sy < height_; ++sy) {
            const auto* src_row = row(sy);
            for (std::int32_t sx = 0; sx < width_; ++sx) {
                std::int32_t dx = sy;
                std::int32_t dy = width_ - 1 - sx;
                std::memcpy(result.row(dy) + static_cast<std::size_t>(dx) * bpp,
                            src_row + static_cast<std::size_t>(sx) * bpp, bpp);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// rotate_180
// ---------------------------------------------------------------------------

Image Image::rotate_180() const {
    if (empty()) return {};

    Image result(width_, height_, format_, dpi_x_, dpi_y_);

    if (format_ == PixelFormat::BW1) {
        for (std::int32_t sy = 0; sy < height_; ++sy) {
            for (std::int32_t sx = 0; sx < width_; ++sx) {
                int px = get_bw_pixel(sx, sy);
                result.set_bw_pixel(width_ - 1 - sx, height_ - 1 - sy, px);
            }
        }
    } else {
        std::size_t bpp = bytes_per_pixel(format_);
        for (std::int32_t sy = 0; sy < height_; ++sy) {
            const auto* src_row = row(sy);
            auto* dst_row = result.row(height_ - 1 - sy);
            for (std::int32_t sx = 0; sx < width_; ++sx) {
                std::int32_t dx = width_ - 1 - sx;
                std::memcpy(dst_row + static_cast<std::size_t>(dx) * bpp,
                            src_row + static_cast<std::size_t>(sx) * bpp, bpp);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// pad
// ---------------------------------------------------------------------------

Image Image::pad(std::int32_t top, std::int32_t left,
                 std::int32_t right, std::int32_t bottom,
                 std::uint8_t fill_value) const {
    std::int32_t new_w = width_ + left + right;
    std::int32_t new_h = height_ + top + bottom;
    if (new_w <= 0 || new_h <= 0) return {};

    Image result(new_w, new_h, format_, dpi_x_, dpi_y_);
    result.fill(fill_value);
    result.blit(*this, left, top);
    return result;
}

// ---------------------------------------------------------------------------
// blit
// ---------------------------------------------------------------------------

void Image::blit(const Image& src, std::int32_t dst_x, std::int32_t dst_y) {
    assert(src.format() == format_);
    if (src.empty() || empty()) return;

    // Compute source region that maps into destination.
    std::int32_t sx = 0, sy = 0;
    std::int32_t w = src.width();
    std::int32_t h = src.height();

    if (dst_x < 0) { sx -= dst_x; w += dst_x; dst_x = 0; }
    if (dst_y < 0) { sy -= dst_y; h += dst_y; dst_y = 0; }
    w = std::min(w, width_ - dst_x);
    h = std::min(h, height_ - dst_y);
    if (w <= 0 || h <= 0) return;

    if (format_ == PixelFormat::BW1) {
        for (std::int32_t r = 0; r < h; ++r) {
            for (std::int32_t c = 0; c < w; ++c) {
                int px = src.get_bw_pixel(sx + c, sy + r);
                set_bw_pixel(dst_x + c, dst_y + r, px);
            }
        }
    } else {
        std::size_t bpp = bytes_per_pixel(format_);
        std::size_t copy_bytes = static_cast<std::size_t>(w) * bpp;
        for (std::int32_t r = 0; r < h; ++r) {
            const auto* src_ptr = src.row(sy + r) + static_cast<std::size_t>(sx) * bpp;
            auto* dst_ptr = row(dst_y + r) + static_cast<std::size_t>(dst_x) * bpp;
            std::memcpy(dst_ptr, src_ptr, copy_bytes);
        }
    }
}

// ---------------------------------------------------------------------------
// invert
// ---------------------------------------------------------------------------

void Image::invert() noexcept {
    for (auto& byte : data_) {
        byte = static_cast<std::uint8_t>(~byte);
    }
}

// ---------------------------------------------------------------------------
// convert
// ---------------------------------------------------------------------------

Image Image::convert(PixelFormat target, std::uint8_t bw_threshold) const {
    if (target == format_) {
        return *this;  // No conversion needed.
    }
    if (empty()) return {};

    // BW1 → Gray8
    if (format_ == PixelFormat::BW1 && target == PixelFormat::Gray8) {
        Image result(width_, height_, PixelFormat::Gray8, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                dst[x] = get_bw_pixel(x, y) ? 0 : 255;  // 1=black=0, 0=white=255.
            }
        }
        return result;
    }

    // BW1 → RGB24
    if (format_ == PixelFormat::BW1 && target == PixelFormat::RGB24) {
        Image result(width_, height_, PixelFormat::RGB24, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                std::uint8_t val = get_bw_pixel(x, y) ? 0 : 255;
                dst[x * 3] = val;
                dst[x * 3 + 1] = val;
                dst[x * 3 + 2] = val;
            }
        }
        return result;
    }

    // Gray8 → BW1 (threshold)
    if (format_ == PixelFormat::Gray8 && target == PixelFormat::BW1) {
        Image result(width_, height_, PixelFormat::BW1, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            const auto* src = row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                result.set_bw_pixel(x, y, src[x] < bw_threshold ? 1 : 0);
            }
        }
        return result;
    }

    // Gray8 → RGB24
    if (format_ == PixelFormat::Gray8 && target == PixelFormat::RGB24) {
        Image result(width_, height_, PixelFormat::RGB24, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            const auto* src = row(y);
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                dst[x * 3] = src[x];
                dst[x * 3 + 1] = src[x];
                dst[x * 3 + 2] = src[x];
            }
        }
        return result;
    }

    // RGB24 → Gray8 (luminance: 0.299R + 0.587G + 0.114B)
    if (format_ == PixelFormat::RGB24 && target == PixelFormat::Gray8) {
        Image result(width_, height_, PixelFormat::Gray8, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            const auto* src = row(y);
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                auto r = src[x * 3];
                auto g = src[x * 3 + 1];
                auto b = src[x * 3 + 2];
                dst[x] = static_cast<std::uint8_t>(
                    (77 * r + 150 * g + 29 * b + 128) >> 8);
            }
        }
        return result;
    }

    // RGB24 → BW1 (via Gray8 threshold)
    if (format_ == PixelFormat::RGB24 && target == PixelFormat::BW1) {
        return convert(PixelFormat::Gray8).convert(PixelFormat::BW1, bw_threshold);
    }

    // Gray8 → RGBA32
    if (format_ == PixelFormat::Gray8 && target == PixelFormat::RGBA32) {
        Image result(width_, height_, PixelFormat::RGBA32, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            const auto* src = row(y);
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                dst[x * 4] = src[x];
                dst[x * 4 + 1] = src[x];
                dst[x * 4 + 2] = src[x];
                dst[x * 4 + 3] = 255;
            }
        }
        return result;
    }

    // RGB24 → RGBA32
    if (format_ == PixelFormat::RGB24 && target == PixelFormat::RGBA32) {
        Image result(width_, height_, PixelFormat::RGBA32, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            const auto* src = row(y);
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                dst[x * 4] = src[x * 3];
                dst[x * 4 + 1] = src[x * 3 + 1];
                dst[x * 4 + 2] = src[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        return result;
    }

    // RGBA32 → RGB24 (drop alpha)
    if (format_ == PixelFormat::RGBA32 && target == PixelFormat::RGB24) {
        Image result(width_, height_, PixelFormat::RGB24, dpi_x_, dpi_y_);
        for (std::int32_t y = 0; y < height_; ++y) {
            const auto* src = row(y);
            auto* dst = result.row(y);
            for (std::int32_t x = 0; x < width_; ++x) {
                dst[x * 3] = src[x * 4];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        return result;
    }

    // RGBA32 → Gray8
    if (format_ == PixelFormat::RGBA32 && target == PixelFormat::Gray8) {
        return convert(PixelFormat::RGB24).convert(PixelFormat::Gray8);
    }

    // RGBA32 → BW1
    if (format_ == PixelFormat::RGBA32 && target == PixelFormat::BW1) {
        return convert(PixelFormat::Gray8).convert(PixelFormat::BW1, bw_threshold);
    }

    // BW1 → RGBA32
    if (format_ == PixelFormat::BW1 && target == PixelFormat::RGBA32) {
        return convert(PixelFormat::RGB24).convert(PixelFormat::RGBA32);
    }

    // Unsupported conversion — return empty.
    return {};
}

} // namespace ppp::core

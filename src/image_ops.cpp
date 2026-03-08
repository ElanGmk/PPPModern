#include "ppp/core/image_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ppp::core::ops {

// ---------------------------------------------------------------------------
// Unit conversion
// ---------------------------------------------------------------------------

double to_pixels(const Measurement& m, double dpi) noexcept {
    switch (m.unit) {
        case MeasurementUnit::Inches:      return m.value * dpi;
        case MeasurementUnit::Pixels:      return m.value;
        case MeasurementUnit::Millimeters: return m.value * dpi / 25.4;
    }
    return m.value;
}

Measurement from_pixels(double pixels, double dpi,
                         MeasurementUnit unit) noexcept {
    switch (unit) {
        case MeasurementUnit::Inches:      return {pixels / dpi, unit};
        case MeasurementUnit::Pixels:      return {pixels, unit};
        case MeasurementUnit::Millimeters: return {pixels * 25.4 / dpi, unit};
    }
    return {pixels, unit};
}

// ---------------------------------------------------------------------------
// Subimage detection
// ---------------------------------------------------------------------------

SubimageResult detect_subimage(const Image& image,
                               const SubimageConfig& config,
                               double dpi_x,
                               double dpi_y) {
    SubimageResult result;
    if (image.empty()) return result;

    // Convert to BW1 if needed.
    Image bw = (image.format() == PixelFormat::BW1)
                   ? image
                   : image.convert(PixelFormat::BW1);

    // Extract spans and find components.
    auto spans = geometry::spans_from_bitmap(
        bw.data(), bw.width(), bw.height(), bw.stride(), 1);

    geometry::Rect full{0, 0, bw.width(), bw.height()};
    auto components = geometry::find_components(spans, full, 4);

    // Filter by minimum pixel size.
    geometry::Rect bounds;
    bool first = true;

    for (const auto& comp : components) {
        if (comp.bounds.width() < config.min_width_px ||
            comp.bounds.height() < config.min_height_px) {
            continue;
        }

        // Check maximum size constraints.
        double max_w_px = to_pixels(config.max_width, dpi_x);
        double max_h_px = to_pixels(config.max_height, dpi_y);
        if (max_w_px > 0 && comp.bounds.width() > static_cast<std::int32_t>(max_w_px))
            continue;
        if (max_h_px > 0 && comp.bounds.height() > static_cast<std::int32_t>(max_h_px))
            continue;

        if (first) {
            bounds = comp.bounds;
            first = false;
        } else {
            bounds = bounds.united(comp.bounds);
        }
        result.components.push_back(comp);
    }

    result.bounds = bounds;

    // Check if the detected content is too small.
    if (!bounds.empty()) {
        if (bounds.width() < config.min_width_px ||
            bounds.height() < config.min_height_px) {
            result.too_small = true;
        }
    } else {
        result.too_small = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Edge cleanup
// ---------------------------------------------------------------------------

namespace {

void clear_rect_bw(Image& image, const geometry::Rect& r) {
    // Clear foreground pixels (set to 0) in the rectangle.
    for (std::int32_t y = std::max(r.top, 0);
         y < std::min(r.bottom, image.height()); ++y) {
        auto* row = image.row(y);
        for (std::int32_t x = std::max(r.left, 0);
             x < std::min(r.right, image.width()); ++x) {
            int byte_idx = x >> 3;
            int bit_idx = 7 - (x & 7);
            row[byte_idx] &= static_cast<std::uint8_t>(~(1 << bit_idx));
        }
    }
}

void clear_rect_gray(Image& image, const geometry::Rect& r, std::uint8_t bg) {
    for (std::int32_t y = std::max(r.top, 0);
         y < std::min(r.bottom, image.height()); ++y) {
        auto* row = image.row(y);
        std::size_t bpp = bytes_per_pixel(image.format());
        for (std::int32_t x = std::max(r.left, 0);
             x < std::min(r.right, image.width()); ++x) {
            std::memset(row + static_cast<std::size_t>(x) * bpp, bg, bpp);
        }
    }
}

void clear_rect(Image& image, const geometry::Rect& r) {
    if (image.format() == PixelFormat::BW1) {
        clear_rect_bw(image, r);
    } else {
        // Use 0xFF for white background (Gray8/RGB24).
        clear_rect_gray(image, r, 0xFF);
    }
}

}  // namespace

void edge_cleanup(Image& image, const EdgeValues& edges,
                  double dpi_x, double dpi_y) {
    if (image.empty()) return;

    auto w = image.width();
    auto h = image.height();

    auto top_px = static_cast<std::int32_t>(std::round(to_pixels(edges.top, dpi_y)));
    auto bottom_px = static_cast<std::int32_t>(std::round(to_pixels(edges.bottom, dpi_y)));
    auto left_px = static_cast<std::int32_t>(std::round(to_pixels(edges.left, dpi_x)));
    auto right_px = static_cast<std::int32_t>(std::round(to_pixels(edges.right, dpi_x)));

    // Top strip.
    if (top_px > 0)
        clear_rect(image, {0, 0, w, std::min(top_px, h)});
    // Bottom strip.
    if (bottom_px > 0)
        clear_rect(image, {0, std::max(0, h - bottom_px), w, h});
    // Left strip.
    if (left_px > 0)
        clear_rect(image, {0, 0, std::min(left_px, w), h});
    // Right strip.
    if (right_px > 0)
        clear_rect(image, {std::max(0, w - right_px), 0, w, h});
}

// ---------------------------------------------------------------------------
// Hole cleanup
// ---------------------------------------------------------------------------

void hole_cleanup(Image& image, const EdgeValues& edges,
                  double dpi_x, double dpi_y) {
    if (image.empty()) return;
    if (image.format() != PixelFormat::BW1) return;

    auto w = image.width();
    auto h = image.height();

    auto top_px = static_cast<std::int32_t>(std::round(to_pixels(edges.top, dpi_y)));
    auto bottom_px = static_cast<std::int32_t>(std::round(to_pixels(edges.bottom, dpi_y)));
    auto left_px = static_cast<std::int32_t>(std::round(to_pixels(edges.left, dpi_x)));
    auto right_px = static_cast<std::int32_t>(std::round(to_pixels(edges.right, dpi_x)));

    // Plausible punch-hole size range at 300 DPI: ~6mm to ~10mm diameter.
    // Scale for actual DPI.
    double scale = std::min(dpi_x, dpi_y) / 300.0;
    auto min_hole_px = static_cast<std::int32_t>(std::round(15 * scale));
    auto max_hole_px = static_cast<std::int32_t>(std::round(60 * scale));

    auto spans = geometry::spans_from_bitmap(
        image.data(), w, h, image.stride(), 1);

    // Check each edge strip for hole-like components.
    geometry::Rect edge_rois[] = {
        {0, 0, w, std::min(top_px, h)},                    // Top
        {0, std::max(0, h - bottom_px), w, h},              // Bottom
        {0, 0, std::min(left_px, w), h},                    // Left
        {std::max(0, w - right_px), 0, w, h},               // Right
    };

    for (const auto& roi : edge_rois) {
        if (roi.empty()) continue;

        auto components = geometry::find_components(spans, roi, 4);
        for (const auto& comp : components) {
            auto cw = comp.bounds.width();
            auto ch = comp.bounds.height();

            // Check if roughly circular and in plausible size range.
            if (cw < min_hole_px || ch < min_hole_px) continue;
            if (cw > max_hole_px || ch > max_hole_px) continue;

            double aspect = static_cast<double>(std::max(cw, ch)) /
                            static_cast<double>(std::min(cw, ch));
            if (aspect > 2.0) continue;  // Too elongated to be a punch hole.

            // Clear the component.
            clear_rect_bw(image, comp.bounds);
        }
    }
}

// ---------------------------------------------------------------------------
// Despeckle
// ---------------------------------------------------------------------------

void despeckle(Image& image, const DespeckleConfig& config) {
    if (config.mode == DespeckleMode::None) return;
    if (image.empty()) return;
    if (image.format() != PixelFormat::BW1) return;

    auto spans = geometry::spans_from_bitmap(
        image.data(), image.width(), image.height(), image.stride(), 1);

    geometry::Rect full{0, 0, image.width(), image.height()};
    auto components = geometry::find_components(spans, full, 4);

    for (const auto& comp : components) {
        bool remove = false;

        if (config.mode == DespeckleMode::SinglePixel) {
            // Remove single-pixel noise.
            remove = (comp.pixel_count == 1);
        } else if (config.mode == DespeckleMode::Object) {
            // Remove objects within the size range.
            auto max_dim = std::max(comp.bounds.width(), comp.bounds.height());
            remove = (max_dim >= config.object_min && max_dim <= config.object_max);
        }

        if (remove) {
            clear_rect_bw(image, comp.bounds);
        }
    }
}

// ---------------------------------------------------------------------------
// Blank page detection
// ---------------------------------------------------------------------------

BlankPageResult detect_blank_page(const Image& image,
                                   const BlankPageConfig& config,
                                   double dpi_x, double dpi_y) {
    BlankPageResult result;

    if (image.empty()) {
        result.is_blank = true;
        return result;
    }

    // Convert to BW1 for analysis.
    Image bw = (image.format() == PixelFormat::BW1)
                   ? image
                   : image.convert(PixelFormat::BW1);

    // Compute edge margin in pixels.
    auto margin_px_x = static_cast<std::int32_t>(to_pixels(config.edge_margin, dpi_x));
    auto margin_px_y = static_cast<std::int32_t>(to_pixels(config.edge_margin, dpi_y));

    // Define the analysis region (inset by margin).
    std::int32_t x0 = std::min(margin_px_x, bw.width() / 2);
    std::int32_t y0 = std::min(margin_px_y, bw.height() / 2);
    std::int32_t x1 = std::max(bw.width() - margin_px_x, x0);
    std::int32_t y1 = std::max(bw.height() - margin_px_y, y0);

    // Count foreground pixels in the analysis region.
    std::int32_t fg_count = 0;
    std::int32_t total = (x1 - x0) * (y1 - y0);

    for (std::int32_t y = y0; y < y1; ++y) {
        for (std::int32_t x = x0; x < x1; ++x) {
            if (bw.get_bw_pixel(x, y)) {
                ++fg_count;
            }
        }
    }

    result.total_pixels = total;
    result.foreground_pixels = fg_count;
    result.foreground_percent = (total > 0)
        ? (static_cast<double>(fg_count) / static_cast<double>(total)) * 100.0
        : 0.0;

    // Count connected components if min_components is set.
    if (config.min_components > 0) {
        geometry::Rect roi{x0, y0, x1, y1};
        auto spans = geometry::spans_from_bitmap(
            bw.data(), bw.width(), bw.height(), bw.stride(), 1);
        auto components = geometry::find_components(spans, roi, 4);
        result.component_count = static_cast<std::int32_t>(components.size());
    }

    // Determine blank status.
    result.is_blank = (result.foreground_percent < config.threshold_percent);
    if (config.min_components > 0 && result.component_count < config.min_components) {
        result.is_blank = true;
    }

    return result;
}

// ---------------------------------------------------------------------------
// Canvas sizing
// ---------------------------------------------------------------------------

CanvasDimensions resolve_canvas(const CanvasConfig& config,
                                double dpi_x, double dpi_y,
                                std::int32_t src_w, std::int32_t src_h) {
    double w_inches = 0, h_inches = 0;

    switch (config.preset) {
        case CanvasPreset::Autodetect:
            return {src_w, src_h};

        case CanvasPreset::Letter:
            w_inches = 8.5;  h_inches = 11.0;
            break;
        case CanvasPreset::Legal:
            w_inches = 8.5;  h_inches = 14.0;
            break;
        case CanvasPreset::Tabloid:
            w_inches = 11.0; h_inches = 17.0;
            break;
        case CanvasPreset::A4:
            w_inches = 8.267; h_inches = 11.692;
            break;
        case CanvasPreset::A3:
            w_inches = 11.692; h_inches = 16.535;
            break;

        case CanvasPreset::Custom:
            w_inches = to_pixels(config.width, dpi_x) / dpi_x;
            h_inches = to_pixels(config.height, dpi_y) / dpi_y;
            break;
    }

    auto w_px = static_cast<std::int32_t>(std::round(w_inches * dpi_x));
    auto h_px = static_cast<std::int32_t>(std::round(h_inches * dpi_y));

    // Apply orientation.
    if (config.orientation == Orientation::Landscape && h_px > w_px) {
        std::swap(w_px, h_px);
    } else if (config.orientation == Orientation::Portrait && w_px > h_px) {
        std::swap(w_px, h_px);
    }

    return {w_px, h_px};
}

// ---------------------------------------------------------------------------
// Margin application
// ---------------------------------------------------------------------------

MarginResult apply_margins(const Image& source,
                           const geometry::Rect& subimage_bounds,
                           const MarginConfig& margins,
                           std::int32_t canvas_width,
                           std::int32_t canvas_height,
                           bool keep_outside,
                           double dpi_x,
                           double dpi_y) {
    MarginResult result;
    if (source.empty() || canvas_width <= 0 || canvas_height <= 0) {
        return result;
    }

    // Create output canvas (white/background).
    result.image = Image(canvas_width, canvas_height, source.format(),
                         source.dpi_x(), source.dpi_y());
    if (source.format() != PixelFormat::BW1) {
        result.image.fill(0xFF);  // White background for grayscale/color.
    }

    // Compute margin distances in pixels.
    auto m_top = static_cast<std::int32_t>(std::round(to_pixels(margins.top.distance, dpi_y)));
    auto m_left = static_cast<std::int32_t>(std::round(to_pixels(margins.left.distance, dpi_x)));
    auto m_right = static_cast<std::int32_t>(std::round(to_pixels(margins.right.distance, dpi_x)));
    auto m_bottom = static_cast<std::int32_t>(std::round(to_pixels(margins.bottom.distance, dpi_y)));

    // Content dimensions from subimage.
    auto content_w = subimage_bounds.width();
    auto content_h = subimage_bounds.height();

    // Determine placement position.
    std::int32_t dst_x = m_left;
    std::int32_t dst_y = m_top;

    if (margins.center_horizontal) {
        // Center content horizontally within the available space.
        std::int32_t avail_w = canvas_width - m_left - m_right;
        dst_x = m_left + (avail_w - content_w) / 2;
    }

    if (margins.center_vertical) {
        std::int32_t avail_h = canvas_height - m_top - m_bottom;
        dst_y = m_top + (avail_h - content_h) / 2;
    }

    // Extract and blit content.
    if (keep_outside) {
        // Copy the entire source image, positioned so the subimage lands at dst.
        std::int32_t offset_x = dst_x - subimage_bounds.left;
        std::int32_t offset_y = dst_y - subimage_bounds.top;
        result.image.blit(source, offset_x, offset_y);
    } else {
        // Only copy the subimage region.
        auto content = source.crop(subimage_bounds.left, subimage_bounds.top,
                                   content_w, content_h);
        result.image.blit(content, dst_x, dst_y);
    }

    result.content_rect = {dst_x, dst_y, dst_x + content_w, dst_y + content_h};
    return result;
}

// ---------------------------------------------------------------------------
// Nearest-neighbor scaling
// ---------------------------------------------------------------------------

Image scale_nearest(const Image& image, std::int32_t new_width, std::int32_t new_height) {
    if (image.empty() || new_width <= 0 || new_height <= 0) return {};

    double scale_x = image.dpi_x() > 0
        ? image.dpi_x() * static_cast<double>(new_width) / image.width()
        : 0;
    double scale_y = image.dpi_y() > 0
        ? image.dpi_y() * static_cast<double>(new_height) / image.height()
        : 0;

    Image result(new_width, new_height, image.format(), scale_x, scale_y);

    double x_ratio = static_cast<double>(image.width()) / new_width;
    double y_ratio = static_cast<double>(image.height()) / new_height;

    if (image.format() == PixelFormat::BW1) {
        for (std::int32_t y = 0; y < new_height; ++y) {
            auto sy = static_cast<std::int32_t>(y * y_ratio);
            sy = std::min(sy, image.height() - 1);
            for (std::int32_t x = 0; x < new_width; ++x) {
                auto sx = static_cast<std::int32_t>(x * x_ratio);
                sx = std::min(sx, image.width() - 1);
                result.set_bw_pixel(x, y, image.get_bw_pixel(sx, sy));
            }
        }
    } else {
        std::size_t bpp = bytes_per_pixel(image.format());
        for (std::int32_t y = 0; y < new_height; ++y) {
            auto sy = static_cast<std::int32_t>(y * y_ratio);
            sy = std::min(sy, image.height() - 1);
            const auto* src_row = image.row(sy);
            auto* dst_row = result.row(y);
            for (std::int32_t x = 0; x < new_width; ++x) {
                auto sx = static_cast<std::int32_t>(x * x_ratio);
                sx = std::min(sx, image.width() - 1);
                std::memcpy(dst_row + static_cast<std::size_t>(x) * bpp,
                            src_row + static_cast<std::size_t>(sx) * bpp, bpp);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Bilinear scaling
// ---------------------------------------------------------------------------

Image scale_bilinear(const Image& image, std::int32_t new_width, std::int32_t new_height) {
    if (image.empty() || new_width <= 0 || new_height <= 0) return {};

    // BW1 can't do bilinear — fall back to nearest.
    if (image.format() == PixelFormat::BW1) {
        return scale_nearest(image, new_width, new_height);
    }

    double scale_x = image.dpi_x() > 0
        ? image.dpi_x() * static_cast<double>(new_width) / image.width()
        : 0;
    double scale_y = image.dpi_y() > 0
        ? image.dpi_y() * static_cast<double>(new_height) / image.height()
        : 0;

    Image result(new_width, new_height, image.format(), scale_x, scale_y);

    std::size_t bpp = bytes_per_pixel(image.format());
    int channels = static_cast<int>(bpp);

    double x_ratio = static_cast<double>(image.width() - 1) / std::max(new_width - 1, 1);
    double y_ratio = static_cast<double>(image.height() - 1) / std::max(new_height - 1, 1);

    for (std::int32_t y = 0; y < new_height; ++y) {
        double src_y = y * y_ratio;
        auto y0 = static_cast<std::int32_t>(src_y);
        auto y1 = std::min(y0 + 1, image.height() - 1);
        double fy = src_y - y0;

        const auto* row0 = image.row(y0);
        const auto* row1 = image.row(y1);
        auto* dst_row = result.row(y);

        for (std::int32_t x = 0; x < new_width; ++x) {
            double src_x = x * x_ratio;
            auto x0 = static_cast<std::int32_t>(src_x);
            auto x1 = std::min(x0 + 1, image.width() - 1);
            double fx = src_x - x0;

            for (int c = 0; c < channels; ++c) {
                double v00 = row0[static_cast<std::size_t>(x0) * bpp + c];
                double v10 = row0[static_cast<std::size_t>(x1) * bpp + c];
                double v01 = row1[static_cast<std::size_t>(x0) * bpp + c];
                double v11 = row1[static_cast<std::size_t>(x1) * bpp + c];

                double v = v00 * (1 - fx) * (1 - fy)
                         + v10 * fx * (1 - fy)
                         + v01 * (1 - fx) * fy
                         + v11 * fx * fy;

                dst_row[static_cast<std::size_t>(x) * bpp + c] =
                    static_cast<std::uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Full resize operation
// ---------------------------------------------------------------------------

ResizeResult apply_resize(const Image& source,
                          const geometry::Rect& subimage_bounds,
                          const ResizeConfig& config,
                          std::size_t page_index,
                          bool odd_even_mode) {
    ResizeResult result;
    if (source.empty() || !config.enabled) return result;

    double dpi_x = source.dpi_x() > 0 ? source.dpi_x() : 300.0;
    double dpi_y = source.dpi_y() > 0 ? source.dpi_y() : 300.0;

    // 1. Determine source region.
    geometry::Rect src_rect;
    switch (config.source) {
        case ResizeFrom::Subimage:
            src_rect = subimage_bounds;
            break;
        case ResizeFrom::FullPage:
            src_rect = {0, 0, source.width(), source.height()};
            break;
        case ResizeFrom::Custom: {
            auto set_idx = (odd_even_mode && page_index % 2 == 1) ? 1 : 0;
            const auto& src_edges = (set_idx == 0) ? config.source_set1 : config.source_set2;
            auto l = static_cast<std::int32_t>(std::round(to_pixels(src_edges.left, dpi_x)));
            auto t = static_cast<std::int32_t>(std::round(to_pixels(src_edges.top, dpi_y)));
            auto r = static_cast<std::int32_t>(std::round(to_pixels(src_edges.right, dpi_x)));
            auto b = static_cast<std::int32_t>(std::round(to_pixels(src_edges.bottom, dpi_y)));
            src_rect = {l, t, source.width() - r, source.height() - b};
            break;
        }
        case ResizeFrom::Smart:
            // Smart: use subimage if available, else full page.
            src_rect = subimage_bounds.empty()
                ? geometry::Rect{0, 0, source.width(), source.height()}
                : subimage_bounds;
            break;
    }

    if (src_rect.empty()) {
        src_rect = {0, 0, source.width(), source.height()};
    }

    // 2. Extract source content.
    auto content = source.crop(src_rect.left, src_rect.top,
                               src_rect.width(), src_rect.height());

    // 3. Resolve resize canvas.
    auto canvas = resolve_canvas(config.canvas, dpi_x, dpi_y,
                                  source.width(), source.height());

    // 4. Compute resize margins.
    auto set_idx = (odd_even_mode && page_index % 2 == 1) ? 1 : 0;
    const auto& margins = (set_idx == 0) ? config.margins_set1 : config.margins_set2;

    auto m_top = static_cast<std::int32_t>(std::round(to_pixels(margins.top, dpi_y)));
    auto m_left = static_cast<std::int32_t>(std::round(to_pixels(margins.left, dpi_x)));
    auto m_right = static_cast<std::int32_t>(std::round(to_pixels(margins.right, dpi_x)));
    auto m_bottom = static_cast<std::int32_t>(std::round(to_pixels(margins.bottom, dpi_y)));

    // Available space for content after margins.
    auto avail_w = canvas.width - m_left - m_right;
    auto avail_h = canvas.height - m_top - m_bottom;
    if (avail_w <= 0 || avail_h <= 0) return result;

    // 5. Compute scale factor to fit content into available space.
    double scale_w = static_cast<double>(avail_w) / content.width();
    double scale_h = static_cast<double>(avail_h) / content.height();
    double scale = std::min(scale_w, scale_h);  // Uniform scale, preserve aspect ratio.

    // Apply shrink/enlarge constraints.
    if (scale < 1.0 && !config.allow_shrink) scale = 1.0;
    if (scale > 1.0 && !config.allow_enlarge) scale = 1.0;

    // 6. Scale content.
    auto scaled_w = static_cast<std::int32_t>(std::round(content.width() * scale));
    auto scaled_h = static_cast<std::int32_t>(std::round(content.height() * scale));
    scaled_w = std::max(scaled_w, 1);
    scaled_h = std::max(scaled_h, 1);

    Image scaled;
    if (scaled_w != content.width() || scaled_h != content.height()) {
        if (config.anti_alias) {
            scaled = scale_bilinear(content, scaled_w, scaled_h);
        } else {
            scaled = scale_nearest(content, scaled_w, scaled_h);
        }
    } else {
        scaled = std::move(content);
    }

    // 7. Place scaled content on canvas with alignment.
    result.image = Image(canvas.width, canvas.height, source.format(),
                         source.dpi_x(), source.dpi_y());
    if (source.format() != PixelFormat::BW1) {
        result.image.fill(0xFF);  // White background.
    }

    std::int32_t dst_x = m_left;
    std::int32_t dst_y = m_top;

    // Horizontal alignment.
    switch (config.h_alignment) {
        case HAlignment::Center:
            dst_x = m_left + (avail_w - scaled_w) / 2;
            break;
        case HAlignment::Proportional:
            // Proportional: maintain relative position.
            if (src_rect.width() > 0) {
                double rel_x = static_cast<double>(src_rect.left) / source.width();
                dst_x = m_left + static_cast<std::int32_t>(rel_x * (avail_w - scaled_w));
            }
            break;
    }

    // Vertical alignment.
    switch (config.v_alignment) {
        case VAlignment::Top:
            dst_y = m_top;
            break;
        case VAlignment::Center:
            dst_y = m_top + (avail_h - scaled_h) / 2;
            break;
        case VAlignment::Bottom:
            dst_y = m_top + avail_h - scaled_h;
            break;
        case VAlignment::Proportional:
            if (src_rect.height() > 0) {
                double rel_y = static_cast<double>(src_rect.top) / source.height();
                dst_y = m_top + static_cast<std::int32_t>(rel_y * (avail_h - scaled_h));
            }
            break;
    }

    result.image.blit(scaled, dst_x, dst_y);
    result.content_rect = {dst_x, dst_y, dst_x + scaled_w, dst_y + scaled_h};

    return result;
}

// ---------------------------------------------------------------------------
// Skew angle detection via projection profile analysis
// ---------------------------------------------------------------------------

namespace {

constexpr double PI = 3.14159265358979323846;

/// Compute the variance of horizontal projection at a given angle.
/// Higher variance = sharper peaks = text lines are more horizontal.
double projection_variance(const Image& bw, double angle_deg) {
    double angle_rad = angle_deg * PI / 180.0;
    double cos_a = std::cos(angle_rad);
    double sin_a = std::sin(angle_rad);

    auto w = bw.width();
    auto h = bw.height();

    // Project each foreground pixel onto the vertical axis after rotation.
    // y' = -x * sin(a) + y * cos(a)
    // We need to find the range of y' to allocate the histogram.
    double y_min = 0, y_max = 0;
    double corners[4] = {
        0,
        -w * sin_a,
        h * cos_a,
        -w * sin_a + h * cos_a
    };
    for (double c : corners) {
        y_min = std::min(y_min, c);
        y_max = std::max(y_max, c);
    }

    int hist_size = static_cast<int>(y_max - y_min) + 2;
    if (hist_size <= 0 || hist_size > 100000) return 0;

    std::vector<int> hist(hist_size, 0);
    int total_pixels = 0;

    // Sample every Nth pixel for speed on large images.
    int x_step = std::max(1, w / 500);
    int y_step = std::max(1, h / 500);

    for (std::int32_t y = 0; y < h; y += y_step) {
        for (std::int32_t x = 0; x < w; x += x_step) {
            if (bw.get_bw_pixel(x, y)) {
                double yp = -x * sin_a + y * cos_a - y_min;
                int bin = static_cast<int>(yp);
                if (bin >= 0 && bin < hist_size) {
                    hist[bin]++;
                    total_pixels++;
                }
            }
        }
    }

    if (total_pixels == 0) return 0;

    // Compute variance of the histogram.
    double mean = static_cast<double>(total_pixels) / hist_size;
    double variance = 0;
    for (int count : hist) {
        double diff = count - mean;
        variance += diff * diff;
    }
    variance /= hist_size;

    return variance;
}

}  // namespace

double detect_skew_angle(const Image& image,
                         double min_angle, double max_angle, double step) {
    if (image.empty()) return 0;

    Image bw = (image.format() == PixelFormat::BW1)
                   ? image
                   : image.convert(PixelFormat::BW1);

    // Coarse search.
    double best_angle = 0;
    double best_variance = 0;

    for (double angle = min_angle; angle <= max_angle; angle += step) {
        double var = projection_variance(bw, angle);
        if (var > best_variance) {
            best_variance = var;
            best_angle = angle;
        }
    }

    // Fine search around the best angle.
    double fine_step = step / 10.0;
    double fine_min = best_angle - step;
    double fine_max = best_angle + step;

    for (double angle = fine_min; angle <= fine_max; angle += fine_step) {
        double var = projection_variance(bw, angle);
        if (var > best_variance) {
            best_variance = var;
            best_angle = angle;
        }
    }

    return best_angle;
}

// ---------------------------------------------------------------------------
// Arbitrary angle rotation
// ---------------------------------------------------------------------------

Image rotate_arbitrary(const Image& image, double angle) {
    if (image.empty()) return {};
    if (std::abs(angle) < 0.001) return image;  // No rotation needed.

    double angle_rad = angle * PI / 180.0;
    double cos_a = std::cos(angle_rad);
    double sin_a = std::sin(angle_rad);

    auto w = image.width();
    auto h = image.height();

    // Compute output dimensions to contain the full rotated image.
    double abs_cos = std::abs(cos_a);
    double abs_sin = std::abs(sin_a);
    auto new_w = static_cast<std::int32_t>(std::ceil(w * abs_cos + h * abs_sin));
    auto new_h = static_cast<std::int32_t>(std::ceil(w * abs_sin + h * abs_cos));

    Image result(new_w, new_h, image.format(), image.dpi_x(), image.dpi_y());
    if (image.format() != PixelFormat::BW1) {
        result.fill(0xFF);  // White background.
    }

    // Center of source and destination.
    double cx_src = w / 2.0;
    double cy_src = h / 2.0;
    double cx_dst = new_w / 2.0;
    double cy_dst = new_h / 2.0;

    if (image.format() == PixelFormat::BW1) {
        // Nearest-neighbor for BW1.
        for (std::int32_t dy = 0; dy < new_h; ++dy) {
            for (std::int32_t dx = 0; dx < new_w; ++dx) {
                // Map destination to source (inverse rotation).
                double rx = dx - cx_dst;
                double ry = dy - cy_dst;
                double sx = rx * cos_a + ry * sin_a + cx_src;
                double sy = -rx * sin_a + ry * cos_a + cy_src;

                auto ix = static_cast<std::int32_t>(std::round(sx));
                auto iy = static_cast<std::int32_t>(std::round(sy));

                if (ix >= 0 && ix < w && iy >= 0 && iy < h) {
                    result.set_bw_pixel(dx, dy, image.get_bw_pixel(ix, iy));
                }
            }
        }
    } else {
        // Bilinear interpolation for grayscale/color.
        std::size_t bpp = bytes_per_pixel(image.format());
        int channels = static_cast<int>(bpp);

        for (std::int32_t dy = 0; dy < new_h; ++dy) {
            auto* dst_row = result.row(dy);
            for (std::int32_t dx = 0; dx < new_w; ++dx) {
                double rx = dx - cx_dst;
                double ry = dy - cy_dst;
                double sx = rx * cos_a + ry * sin_a + cx_src;
                double sy = -rx * sin_a + ry * cos_a + cy_src;

                if (sx < 0 || sx >= w - 1 || sy < 0 || sy >= h - 1) continue;

                auto x0 = static_cast<std::int32_t>(sx);
                auto y0 = static_cast<std::int32_t>(sy);
                auto x1 = std::min(x0 + 1, w - 1);
                auto y1 = std::min(y0 + 1, h - 1);
                double fx = sx - x0;
                double fy = sy - y0;

                const auto* r0 = image.row(y0);
                const auto* r1 = image.row(y1);

                for (int c = 0; c < channels; ++c) {
                    double v00 = r0[static_cast<std::size_t>(x0) * bpp + c];
                    double v10 = r0[static_cast<std::size_t>(x1) * bpp + c];
                    double v01 = r1[static_cast<std::size_t>(x0) * bpp + c];
                    double v11 = r1[static_cast<std::size_t>(x1) * bpp + c];

                    double v = v00 * (1 - fx) * (1 - fy)
                             + v10 * fx * (1 - fy)
                             + v01 * (1 - fx) * fy
                             + v11 * fx * fy;

                    dst_row[static_cast<std::size_t>(dx) * bpp + c] =
                        static_cast<std::uint8_t>(std::clamp(v + 0.5, 0.0, 255.0));
                }
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Full deskew operation
// ---------------------------------------------------------------------------

DeskewResult apply_deskew(const Image& image, const DeskewConfig& config) {
    DeskewResult result;
    result.image = image;  // Default: return input unchanged.

    if (!config.enabled || image.empty()) return result;

    // Detect skew angle.
    result.angle = detect_skew_angle(
        image, -config.max_angle, config.max_angle, 0.1);

    // Compute confidence as ratio of detected variance at best angle
    // vs variance at zero angle.
    {
        Image bw = (image.format() == PixelFormat::BW1)
                       ? image
                       : image.convert(PixelFormat::BW1);
        double var_at_best = projection_variance(bw, result.angle);
        double var_at_zero = projection_variance(bw, 0);
        if (var_at_zero > 0) {
            result.confidence = std::clamp(
                (var_at_best - var_at_zero) / var_at_zero, 0.0, 1.0);
        }
    }

    // Check if angle is within the correction range.
    double abs_angle = std::abs(result.angle);
    if (abs_angle < config.min_angle) {
        // Skew is below minimum threshold — don't correct.
        return result;
    }
    if (abs_angle > config.max_angle) {
        // Shouldn't happen given search range, but guard.
        return result;
    }

    // Apply rotation to correct the skew.
    result.image = rotate_arbitrary(image, result.angle);
    result.corrected = true;

    return result;
}

// ---------------------------------------------------------------------------
// Morphological operations
// ---------------------------------------------------------------------------

void dilate(Image& image, StructuringElement element, int iterations) {
    if (image.empty() || image.format() != PixelFormat::BW1) return;

    auto w = image.width();
    auto h = image.height();

    for (int iter = 0; iter < iterations; ++iter) {
        Image temp(w, h, PixelFormat::BW1, image.dpi_x(), image.dpi_y());
        temp.fill(0);

        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                if (image.get_bw_pixel(x, y)) {
                    temp.set_bw_pixel(x, y, 1);
                    // Expand to neighbors.
                    if (x > 0)     temp.set_bw_pixel(x - 1, y, 1);
                    if (x < w - 1) temp.set_bw_pixel(x + 1, y, 1);
                    if (y > 0)     temp.set_bw_pixel(x, y - 1, 1);
                    if (y < h - 1) temp.set_bw_pixel(x, y + 1, 1);

                    if (element == StructuringElement::Square) {
                        if (x > 0 && y > 0)         temp.set_bw_pixel(x - 1, y - 1, 1);
                        if (x < w - 1 && y > 0)     temp.set_bw_pixel(x + 1, y - 1, 1);
                        if (x > 0 && y < h - 1)     temp.set_bw_pixel(x - 1, y + 1, 1);
                        if (x < w - 1 && y < h - 1) temp.set_bw_pixel(x + 1, y + 1, 1);
                    }
                }
            }
        }

        image = std::move(temp);
    }
}

void erode(Image& image, StructuringElement element, int iterations) {
    if (image.empty() || image.format() != PixelFormat::BW1) return;

    auto w = image.width();
    auto h = image.height();

    for (int iter = 0; iter < iterations; ++iter) {
        Image temp(w, h, PixelFormat::BW1, image.dpi_x(), image.dpi_y());
        temp.fill(0);

        for (std::int32_t y = 0; y < h; ++y) {
            for (std::int32_t x = 0; x < w; ++x) {
                if (!image.get_bw_pixel(x, y)) continue;

                bool keep = true;
                // Check all required neighbors.
                if (x == 0 || !image.get_bw_pixel(x - 1, y)) keep = false;
                if (keep && (x >= w - 1 || !image.get_bw_pixel(x + 1, y))) keep = false;
                if (keep && (y == 0 || !image.get_bw_pixel(x, y - 1))) keep = false;
                if (keep && (y >= h - 1 || !image.get_bw_pixel(x, y + 1))) keep = false;

                if (keep && element == StructuringElement::Square) {
                    if (x == 0 || y == 0 || !image.get_bw_pixel(x - 1, y - 1)) keep = false;
                    if (keep && (x >= w - 1 || y == 0 || !image.get_bw_pixel(x + 1, y - 1))) keep = false;
                    if (keep && (x == 0 || y >= h - 1 || !image.get_bw_pixel(x - 1, y + 1))) keep = false;
                    if (keep && (x >= w - 1 || y >= h - 1 || !image.get_bw_pixel(x + 1, y + 1))) keep = false;
                }

                if (keep) temp.set_bw_pixel(x, y, 1);
            }
        }

        image = std::move(temp);
    }
}

Image morph_open(const Image& image, StructuringElement element, int iterations) {
    if (image.empty() || image.format() != PixelFormat::BW1) return image;
    Image result = image;
    erode(result, element, iterations);
    dilate(result, element, iterations);
    return result;
}

Image morph_close(const Image& image, StructuringElement element, int iterations) {
    if (image.empty() || image.format() != PixelFormat::BW1) return image;
    Image result = image;
    dilate(result, element, iterations);
    erode(result, element, iterations);
    return result;
}

// ---------------------------------------------------------------------------
// Movement limit enforcement
// ---------------------------------------------------------------------------

MovementLimitResult check_movement_limit(
    std::int32_t original_x, std::int32_t original_y,
    std::int32_t placed_x, std::int32_t placed_y,
    const MovementLimitConfig& config,
    double dpi_x, double dpi_y) {

    MovementLimitResult result;
    result.original_dx = placed_x - original_x;
    result.original_dy = placed_y - original_y;
    result.dx = result.original_dx;
    result.dy = result.original_dy;

    if (!config.enabled) {
        return result;
    }

    result.max_dx = static_cast<std::int32_t>(std::round(to_pixels(config.max_horizontal, dpi_x)));
    result.max_dy = static_cast<std::int32_t>(std::round(to_pixels(config.max_vertical, dpi_y)));

    bool clamped = false;

    if (result.dx > result.max_dx) {
        result.dx = result.max_dx;
        clamped = true;
    } else if (result.dx < -result.max_dx) {
        result.dx = -result.max_dx;
        clamped = true;
    }

    if (result.dy > result.max_dy) {
        result.dy = result.max_dy;
        clamped = true;
    } else if (result.dy < -result.max_dy) {
        result.dy = -result.max_dy;
        clamped = true;
    }

    result.clamped = clamped;
    return result;
}

// ---------------------------------------------------------------------------
// Color dropout
// ---------------------------------------------------------------------------

Image color_dropout(const Image& image, const ColorDropoutConfig& config) {
    if (!config.enabled || config.color == DropoutColor::None) {
        return image;
    }

    // Only meaningful for color images.
    if (image.format() != PixelFormat::RGB24 && image.format() != PixelFormat::RGBA32) {
        return image;
    }

    Image result = image;
    const auto w = result.width();
    const auto h = result.height();
    const int bpp = (result.format() == PixelFormat::RGB24) ? 3 : 4;
    const auto threshold = static_cast<int>(config.threshold);

    // Channel indices: R=0, G=1, B=2.
    int target_idx = 0;
    switch (config.color) {
    case DropoutColor::Red:   target_idx = 0; break;
    case DropoutColor::Green: target_idx = 1; break;
    case DropoutColor::Blue:  target_idx = 2; break;
    default: return result;
    }
    const int other1 = (target_idx + 1) % 3;
    const int other2 = (target_idx + 2) % 3;

    for (std::int32_t y = 0; y < h; ++y) {
        auto* row = result.row(y);
        for (std::int32_t x = 0; x < w; ++x) {
            auto* px = row + x * bpp;
            const int t = px[target_idx];
            const int avg_other = (px[other1] + px[other2]) / 2;
            if (t - avg_other > threshold) {
                px[0] = 0xFF;
                px[1] = 0xFF;
                px[2] = 0xFF;
                if (bpp == 4) px[3] = 0xFF;
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------

double Histogram::mean() const noexcept {
    if (total_pixels == 0) return 0.0;
    double sum = 0;
    for (int i = 0; i < 256; ++i) {
        sum += static_cast<double>(i) * bins[i];
    }
    return sum / total_pixels;
}

std::uint8_t Histogram::median() const noexcept {
    if (total_pixels == 0) return 0;
    std::int32_t half = total_pixels / 2;
    std::int32_t cumulative = 0;
    for (int i = 0; i < 256; ++i) {
        cumulative += bins[i];
        if (cumulative > half) return static_cast<std::uint8_t>(i);
    }
    return 255;
}

std::uint8_t Histogram::min_value() const noexcept {
    for (int i = 0; i < 256; ++i) {
        if (bins[i] > 0) return static_cast<std::uint8_t>(i);
    }
    return 0;
}

std::uint8_t Histogram::max_value() const noexcept {
    for (int i = 255; i >= 0; --i) {
        if (bins[i] > 0) return static_cast<std::uint8_t>(i);
    }
    return 0;
}

Histogram compute_histogram(const Image& image) {
    Histogram hist;
    if (image.empty()) return hist;

    auto w = image.width();
    auto h = image.height();

    switch (image.format()) {
        case PixelFormat::BW1:
            for (std::int32_t y = 0; y < h; ++y) {
                for (std::int32_t x = 0; x < w; ++x) {
                    if (image.get_bw_pixel(x, y)) {
                        ++hist.bins[0];    // Foreground → black.
                    } else {
                        ++hist.bins[255];  // Background → white.
                    }
                }
            }
            break;

        case PixelFormat::Gray8:
            for (std::int32_t y = 0; y < h; ++y) {
                const auto* row = image.row(y);
                for (std::int32_t x = 0; x < w; ++x) {
                    ++hist.bins[row[x]];
                }
            }
            break;

        case PixelFormat::RGB24:
            for (std::int32_t y = 0; y < h; ++y) {
                const auto* row = image.row(y);
                for (std::int32_t x = 0; x < w; ++x) {
                    auto r = row[x * 3 + 0];
                    auto g = row[x * 3 + 1];
                    auto b = row[x * 3 + 2];
                    auto lum = static_cast<std::uint8_t>(
                        0.299 * r + 0.587 * g + 0.114 * b + 0.5);
                    ++hist.bins[lum];
                }
            }
            break;

        case PixelFormat::RGBA32:
            for (std::int32_t y = 0; y < h; ++y) {
                const auto* row = image.row(y);
                for (std::int32_t x = 0; x < w; ++x) {
                    auto r = row[x * 4 + 0];
                    auto g = row[x * 4 + 1];
                    auto b = row[x * 4 + 2];
                    auto lum = static_cast<std::uint8_t>(
                        0.299 * r + 0.587 * g + 0.114 * b + 0.5);
                    ++hist.bins[lum];
                }
            }
            break;
    }

    hist.total_pixels = w * h;
    return hist;
}

// ---------------------------------------------------------------------------
// Otsu's method
// ---------------------------------------------------------------------------

std::uint8_t otsu_threshold(const Histogram& hist) {
    if (hist.total_pixels == 0) return 128;

    double total = static_cast<double>(hist.total_pixels);

    // Compute total mean.
    double sum_total = 0;
    for (int i = 0; i < 256; ++i) {
        sum_total += static_cast<double>(i) * hist.bins[i];
    }

    double sum_bg = 0;
    double weight_bg = 0;
    double max_variance = 0;
    int best_threshold = 0;

    for (int t = 0; t < 256; ++t) {
        weight_bg += hist.bins[t];
        if (weight_bg == 0) continue;

        double weight_fg = total - weight_bg;
        if (weight_fg == 0) break;

        sum_bg += static_cast<double>(t) * hist.bins[t];

        double mean_bg = sum_bg / weight_bg;
        double mean_fg = (sum_total - sum_bg) / weight_fg;

        double diff = mean_bg - mean_fg;
        double variance = weight_bg * weight_fg * diff * diff;

        if (variance > max_variance) {
            max_variance = variance;
            best_threshold = t;
        }
    }

    return static_cast<std::uint8_t>(best_threshold);
}

// ---------------------------------------------------------------------------
// Binarize
// ---------------------------------------------------------------------------

Image binarize(const Image& image, std::uint8_t threshold) {
    if (image.empty()) return {};
    if (image.format() == PixelFormat::BW1) return image;

    auto w = image.width();
    auto h = image.height();
    Image result(w, h, PixelFormat::BW1, image.dpi_x(), image.dpi_y());
    result.fill(0);  // White background.

    for (std::int32_t y = 0; y < h; ++y) {
        const auto* src = image.row(y);

        for (std::int32_t x = 0; x < w; ++x) {
            std::uint8_t intensity = 255;

            switch (image.format()) {
                case PixelFormat::Gray8:
                    intensity = src[x];
                    break;
                case PixelFormat::RGB24: {
                    auto r = src[x * 3 + 0];
                    auto g = src[x * 3 + 1];
                    auto b = src[x * 3 + 2];
                    intensity = static_cast<std::uint8_t>(
                        0.299 * r + 0.587 * g + 0.114 * b + 0.5);
                    break;
                }
                case PixelFormat::RGBA32: {
                    auto r = src[x * 4 + 0];
                    auto g = src[x * 4 + 1];
                    auto b = src[x * 4 + 2];
                    intensity = static_cast<std::uint8_t>(
                        0.299 * r + 0.587 * g + 0.114 * b + 0.5);
                    break;
                }
                default:
                    break;
            }

            if (intensity <= threshold) {
                result.set_bw_pixel(x, y, 1);  // Dark → foreground.
            }
        }
    }

    return result;
}

Image binarize_otsu(const Image& image) {
    if (image.empty()) return {};
    if (image.format() == PixelFormat::BW1) return image;

    auto hist = compute_histogram(image);
    auto threshold = otsu_threshold(hist);
    return binarize(image, threshold);
}

} // namespace ppp::core::ops

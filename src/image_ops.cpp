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

} // namespace ppp::core::ops

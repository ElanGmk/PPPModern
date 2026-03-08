#include "ppp/core/processing_pipeline.h"

#include <sstream>

namespace ppp::core {

namespace {

// Helper: select odd or even config set based on page index.
// Index 0 = first page (odd), index 1 = second page (even), etc.
// If odd_even_mode is disabled, always use set 0.
std::size_t page_set(std::size_t page_index, bool odd_even_mode) {
    if (!odd_even_mode) return 0;
    return (page_index % 2 == 0) ? 0 : 1;  // 0-based: even index = odd page.
}

ProcessingStep step_rotate(Image& img, Rotation rotation) {
    ProcessingStep step{"rotate", false, ""};

    switch (rotation) {
        case Rotation::None:
            step.detail = "no rotation";
            return step;
        case Rotation::CW90:
            img = img.rotate_cw90();
            step.applied = true;
            step.detail = "rotated CW 90 degrees";
            break;
        case Rotation::CCW90:
            img = img.rotate_ccw90();
            step.applied = true;
            step.detail = "rotated CCW 90 degrees";
            break;
        case Rotation::R180:
            img = img.rotate_180();
            step.applied = true;
            step.detail = "rotated 180 degrees";
            break;
    }

    return step;
}

ProcessingStep step_edge_cleanup(Image& img, const EdgeCleanupConfig& config,
                                  std::size_t page_index, bool odd_even) {
    ProcessingStep step{"edge_cleanup", false, ""};

    if (!config.enabled) {
        step.detail = "disabled";
        return step;
    }

    auto set_idx = page_set(page_index, odd_even);
    const auto& edges = (set_idx == 0) ? config.set1 : config.set2;

    ops::edge_cleanup(img, edges, img.dpi_x(), img.dpi_y());
    step.applied = true;

    std::ostringstream oss;
    oss << "cleaned edges (set " << (set_idx + 1) << ")";
    step.detail = oss.str();

    return step;
}

ProcessingStep step_hole_cleanup(Image& img, const HoleCleanupConfig& config,
                                  std::size_t page_index, bool odd_even) {
    ProcessingStep step{"hole_cleanup", false, ""};

    if (!config.enabled) {
        step.detail = "disabled";
        return step;
    }

    // Need BW1 for hole cleanup.
    if (img.format() != PixelFormat::BW1) {
        step.detail = "skipped (not BW1)";
        return step;
    }

    auto set_idx = page_set(page_index, odd_even);
    const auto& edges = (set_idx == 0) ? config.set1 : config.set2;

    ops::hole_cleanup(img, edges, img.dpi_x(), img.dpi_y());
    step.applied = true;

    std::ostringstream oss;
    oss << "cleaned punch holes (set " << (set_idx + 1) << ")";
    step.detail = oss.str();

    return step;
}

ProcessingStep step_despeckle(Image& img, const DespeckleConfig& config) {
    ProcessingStep step{"despeckle", false, ""};

    if (config.mode == DespeckleMode::None) {
        step.detail = "disabled";
        return step;
    }

    if (img.format() != PixelFormat::BW1) {
        step.detail = "skipped (not BW1)";
        return step;
    }

    ops::despeckle(img, config);
    step.applied = true;

    if (config.mode == DespeckleMode::SinglePixel) {
        step.detail = "removed single-pixel noise";
    } else {
        std::ostringstream oss;
        oss << "removed objects " << config.object_min << "-" << config.object_max << " px";
        step.detail = oss.str();
    }

    return step;
}

ProcessingStep step_deskew(Image& img, const DeskewConfig& config) {
    ProcessingStep step{"deskew", false, ""};

    if (!config.enabled) {
        step.detail = "disabled";
        return step;
    }

    auto deskew_result = ops::apply_deskew(img, config);
    step.applied = deskew_result.corrected;

    std::ostringstream oss;
    oss << "angle=" << deskew_result.angle << " deg"
        << ", confidence=" << deskew_result.confidence;
    if (deskew_result.corrected) {
        oss << ", corrected";
        img = std::move(deskew_result.image);
    } else {
        oss << ", not corrected";
    }
    step.detail = oss.str();

    return step;
}

ProcessingStep step_blank_page(const Image& img, const BlankPageConfig& config,
                                ops::BlankPageResult& out_result) {
    ProcessingStep step{"blank_page", false, ""};

    if (!config.enabled) {
        step.detail = "disabled";
        return step;
    }

    out_result = ops::detect_blank_page(img, config, img.dpi_x(), img.dpi_y());
    step.applied = true;

    std::ostringstream oss;
    oss << (out_result.is_blank ? "BLANK" : "not blank")
        << " (" << out_result.foreground_percent << "% foreground"
        << ", " << out_result.foreground_pixels << "/"
        << out_result.total_pixels << " px";
    if (config.min_components > 0) {
        oss << ", " << out_result.component_count << " components";
    }
    oss << ")";
    step.detail = oss.str();

    return step;
}

ProcessingStep step_detect_subimage(const Image& img, const SubimageConfig& config,
                                     ops::SubimageResult& out_result) {
    ProcessingStep step{"detect_subimage", true, ""};

    out_result = ops::detect_subimage(img, config, img.dpi_x(), img.dpi_y());

    std::ostringstream oss;
    if (out_result.bounds.empty()) {
        oss << "no content detected";
    } else {
        oss << "content at (" << out_result.bounds.left << ","
            << out_result.bounds.top << ") "
            << out_result.bounds.width() << "x"
            << out_result.bounds.height() << " px";
        if (out_result.too_small) oss << " (too small)";
        oss << ", " << out_result.components.size() << " components";
    }
    step.detail = oss.str();

    return step;
}

ProcessingStep step_margins(Image& img, const geometry::Rect& subimage_bounds,
                             const ProcessingProfile& profile,
                             std::size_t page_index,
                             const ops::CanvasDimensions& canvas,
                             geometry::Rect& out_content_rect) {
    ProcessingStep step{"margins", false, ""};

    if (!profile.position_image) {
        step.detail = "positioning disabled";
        return step;
    }

    auto set_idx = page_set(page_index, profile.odd_even_mode);
    const auto& margins = profile.margins[set_idx];

    auto result = ops::apply_margins(
        img, subimage_bounds, margins,
        canvas.width, canvas.height,
        profile.keep_outside_subimage,
        img.dpi_x(), img.dpi_y());

    img = std::move(result.image);
    out_content_rect = result.content_rect;
    step.applied = true;

    std::ostringstream oss;
    oss << "positioned content at (" << out_content_rect.left << ","
        << out_content_rect.top << ") on "
        << canvas.width << "x" << canvas.height << " canvas";
    step.detail = oss.str();

    return step;
}

}  // namespace

// ---------------------------------------------------------------------------
// Full pipeline
// ---------------------------------------------------------------------------

ProcessingResult run_pipeline(const Image& image,
                              const ProcessingProfile& profile,
                              std::size_t page_index) {
    ProcessingResult result;

    if (image.empty()) {
        result.success = false;
        result.error = "empty source image";
        return result;
    }

    // Work on a mutable copy.
    Image img = image;

    // Ensure DPI is set (use detected or default 300).
    if (img.dpi_x() <= 0 || img.dpi_y() <= 0) {
        double dpi = (profile.detected_dpi > 0) ? static_cast<double>(profile.detected_dpi) : 300.0;
        img.set_dpi(dpi, dpi);
    }

    // 1. Rotation.
    result.steps.push_back(step_rotate(img, profile.rotation));

    // 1b. Color dropout.
    if (profile.color_dropout.enabled && profile.color_dropout.color != DropoutColor::None) {
        img = ops::color_dropout(img, profile.color_dropout);
        result.steps.push_back({"color_dropout", true,
            std::string("dropped ") + std::string(to_string(profile.color_dropout.color))});
    }

    // 2. Edge cleanup (before deskew).
    if (profile.edge_cleanup.order == EdgeCleanupOrder::BeforeDeskew) {
        result.steps.push_back(step_edge_cleanup(
            img, profile.edge_cleanup, page_index, profile.odd_even_mode));
    }

    // 3. Deskew.
    result.steps.push_back(step_deskew(img, profile.deskew));

    // 4. Hole cleanup.
    result.steps.push_back(step_hole_cleanup(
        img, profile.hole_cleanup, page_index, profile.odd_even_mode));

    // 5. Despeckle.
    result.steps.push_back(step_despeckle(img, profile.despeckle));

    // 6. Edge cleanup (after deskew).
    if (profile.edge_cleanup.order == EdgeCleanupOrder::AfterDeskew) {
        result.steps.push_back(step_edge_cleanup(
            img, profile.edge_cleanup, page_index, profile.odd_even_mode));
    }

    // 7. Subimage detection.
    ops::SubimageResult subimage_result;
    result.steps.push_back(step_detect_subimage(img, profile.subimage, subimage_result));
    result.subimage_bounds = subimage_result.bounds;

    // If no content detected, use the full image as the subimage.
    if (result.subimage_bounds.empty()) {
        result.subimage_bounds = {0, 0, img.width(), img.height()};
    }

    // 8. Blank page detection.
    ops::BlankPageResult blank_result;
    result.steps.push_back(step_blank_page(img, profile.blank_page, blank_result));
    result.is_blank = blank_result.is_blank;
    result.blank_page_result = blank_result;

    // 9. Canvas resolution.
    result.canvas = ops::resolve_canvas(
        profile.canvas, img.dpi_x(), img.dpi_y(), img.width(), img.height());

    // 9. Margin application / content positioning.
    geometry::Rect content_rect;
    result.steps.push_back(step_margins(
        img, result.subimage_bounds, profile, page_index,
        result.canvas, content_rect));

    // 9b. Movement limit enforcement.
    if (profile.movement_limit.enabled && !content_rect.empty()) {
        auto ml = ops::check_movement_limit(
            result.subimage_bounds.left, result.subimage_bounds.top,
            content_rect.left, content_rect.top,
            profile.movement_limit,
            img.dpi_x(), img.dpi_y());

        ProcessingStep step{"movement_limit", ml.clamped, ""};
        std::ostringstream oss;
        if (ml.clamped) {
            // Re-position: shift content back to clamped displacement.
            auto shift_x = ml.dx - ml.original_dx;
            auto shift_y = ml.dy - ml.original_dy;

            // Create new canvas and re-blit with adjusted position.
            Image reimg(img.width(), img.height(), img.format(),
                        img.dpi_x(), img.dpi_y());
            if (img.format() != PixelFormat::BW1) {
                reimg.fill(0xFF);
            }
            reimg.blit(img, shift_x, shift_y);
            img = std::move(reimg);

            content_rect.left += shift_x;
            content_rect.right += shift_x;
            content_rect.top += shift_y;
            content_rect.bottom += shift_y;

            oss << "clamped displacement from ("
                << ml.original_dx << "," << ml.original_dy << ") to ("
                << ml.dx << "," << ml.dy << "), max=("
                << ml.max_dx << "," << ml.max_dy << ")";
        } else {
            oss << "within limits (" << ml.original_dx << "," << ml.original_dy
                << "), max=(" << ml.max_dx << "," << ml.max_dy << ")";
        }
        step.detail = oss.str();
        result.steps.push_back(step);
    }

    // 10. Resize.
    if (profile.resize.enabled) {
        ProcessingStep step{"resize", false, ""};
        auto resize_result = ops::apply_resize(
            img, result.subimage_bounds, profile.resize,
            page_index, profile.odd_even_mode);

        if (!resize_result.image.empty()) {
            img = std::move(resize_result.image);
            step.applied = true;

            std::ostringstream oss;
            oss << "resized to " << img.width() << "x" << img.height()
                << ", content at (" << resize_result.content_rect.left
                << "," << resize_result.content_rect.top << ")";
            step.detail = oss.str();
        } else {
            step.detail = "resize produced empty result";
        }
        result.steps.push_back(step);
    }

    result.image = std::move(img);
    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// Single-step execution
// ---------------------------------------------------------------------------

ProcessingResult run_step(const Image& image,
                          const ProcessingProfile& profile,
                          const std::string& step_name,
                          std::size_t page_index) {
    ProcessingResult result;

    if (image.empty()) {
        result.success = false;
        result.error = "empty source image";
        return result;
    }

    Image img = image;
    if (img.dpi_x() <= 0 || img.dpi_y() <= 0) {
        double dpi = (profile.detected_dpi > 0) ? static_cast<double>(profile.detected_dpi) : 300.0;
        img.set_dpi(dpi, dpi);
    }

    if (step_name == "rotate") {
        result.steps.push_back(step_rotate(img, profile.rotation));
    } else if (step_name == "color_dropout") {
        if (profile.color_dropout.enabled && profile.color_dropout.color != DropoutColor::None) {
            img = ops::color_dropout(img, profile.color_dropout);
            result.steps.push_back({"color_dropout", true,
                std::string("dropped ") + std::string(to_string(profile.color_dropout.color))});
        } else {
            result.steps.push_back({"color_dropout", false, "disabled"});
        }
    } else if (step_name == "edge_cleanup") {
        result.steps.push_back(step_edge_cleanup(
            img, profile.edge_cleanup, page_index, profile.odd_even_mode));
    } else if (step_name == "hole_cleanup") {
        result.steps.push_back(step_hole_cleanup(
            img, profile.hole_cleanup, page_index, profile.odd_even_mode));
    } else if (step_name == "despeckle") {
        result.steps.push_back(step_despeckle(img, profile.despeckle));
    } else if (step_name == "deskew") {
        result.steps.push_back(step_deskew(img, profile.deskew));
    } else if (step_name == "blank_page") {
        ops::BlankPageResult blank_result;
        result.steps.push_back(step_blank_page(img, profile.blank_page, blank_result));
        result.is_blank = blank_result.is_blank;
        result.blank_page_result = blank_result;
    } else if (step_name == "detect_subimage") {
        ops::SubimageResult sub;
        result.steps.push_back(step_detect_subimage(img, profile.subimage, sub));
        result.subimage_bounds = sub.bounds;
    } else if (step_name == "margins") {
        // Need subimage detection first for margins.
        ops::SubimageResult sub;
        step_detect_subimage(img, profile.subimage, sub);
        result.subimage_bounds = sub.bounds;
        if (result.subimage_bounds.empty()) {
            result.subimage_bounds = {0, 0, img.width(), img.height()};
        }
        result.canvas = ops::resolve_canvas(
            profile.canvas, img.dpi_x(), img.dpi_y(), img.width(), img.height());
        geometry::Rect content_rect;
        result.steps.push_back(step_margins(
            img, result.subimage_bounds, profile, page_index,
            result.canvas, content_rect));
    } else if (step_name == "resize") {
        ops::SubimageResult sub;
        step_detect_subimage(img, profile.subimage, sub);
        result.subimage_bounds = sub.bounds;
        if (result.subimage_bounds.empty()) {
            result.subimage_bounds = {0, 0, img.width(), img.height()};
        }
        auto resize_result = ops::apply_resize(
            img, result.subimage_bounds, profile.resize,
            page_index, profile.odd_even_mode);
        if (!resize_result.image.empty()) {
            img = std::move(resize_result.image);
            result.steps.push_back({"resize", true, "resized"});
        } else {
            result.steps.push_back({"resize", false, "no resize applied"});
        }
    } else {
        result.success = false;
        result.error = "unknown step: " + step_name;
        return result;
    }

    result.image = std::move(img);
    result.success = true;
    return result;
}

// ---------------------------------------------------------------------------
// Batch processing
// ---------------------------------------------------------------------------

BatchResult run_batch(const std::vector<Image>& images,
                       const ProcessingProfile& profile,
                       BatchProgressCallback progress) {
    BatchResult batch;
    batch.total = static_cast<std::int32_t>(images.size());
    batch.pages.reserve(images.size());

    for (std::size_t i = 0; i < images.size(); ++i) {
        auto result = run_pipeline(images[i], profile, i);

        if (result.success) {
            ++batch.succeeded;
        } else {
            ++batch.failed;
            if (batch.error.empty()) {
                batch.error = "page " + std::to_string(i) + ": " + result.error;
            }
            batch.success = false;
        }

        if (result.is_blank) {
            ++batch.blank;
        }

        batch.pages.push_back(std::move(result));

        // Invoke progress callback.
        if (progress) {
            if (!progress(i, images.size(), batch.pages.back())) {
                // Cancelled by callback.
                batch.error = "cancelled at page " + std::to_string(i);
                batch.success = false;
                break;
            }
        }
    }

    return batch;
}

std::vector<Image> collect_images(const BatchResult& batch,
                                    bool include_blank) {
    std::vector<Image> images;
    images.reserve(batch.pages.size());

    for (const auto& page : batch.pages) {
        if (!page.success) continue;
        if (!include_blank && page.is_blank) continue;
        if (page.image.empty()) continue;
        images.push_back(page.image);
    }

    return images;
}

} // namespace ppp::core

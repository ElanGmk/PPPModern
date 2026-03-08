#pragma once

#include "ppp/core/geometry.h"
#include "ppp/core/image.h"
#include "ppp/core/image_ops.h"
#include "ppp/core/processing_config.h"

#include <optional>
#include <string>
#include <vector>

namespace ppp::core {

// ---------------------------------------------------------------------------
// Processing step log entry
// ---------------------------------------------------------------------------

/// Records what happened during a single processing step.
struct ProcessingStep {
    std::string name;           ///< Step name (e.g. "rotate", "edge_cleanup").
    bool applied{false};        ///< Whether the step actually modified the image.
    std::string detail;         ///< Human-readable detail (e.g. "rotated CW90").
};

// ---------------------------------------------------------------------------
// Processing result
// ---------------------------------------------------------------------------

/// Complete result of running a ProcessingProfile against an image.
struct ProcessingResult {
    Image image;                            ///< Final processed image.
    std::vector<ProcessingStep> steps;      ///< Log of what was done.
    geometry::Rect subimage_bounds;         ///< Detected content bounds.
    ops::CanvasDimensions canvas;           ///< Resolved canvas size.
    bool is_blank{false};                   ///< Whether blank page was detected.
    ops::BlankPageResult blank_page_result; ///< Blank page detection metrics.
    bool success{true};
    std::string error;                      ///< Error message if !success.
};

// ---------------------------------------------------------------------------
// Processing pipeline
// ---------------------------------------------------------------------------

/// Run the full PPP processing pipeline on an image.
///
/// Applies the following steps in order (each gated by its config):
///   1.  Rotation (CW90, CCW90, 180)
///   1b. Color dropout (remove a color channel)
///   2.  Edge cleanup (before deskew, if configured)
///   3.  Deskew (projection profile skew detection + rotation)
///   4.  Hole cleanup
///   5.  Despeckle
///   6.  Edge cleanup (after deskew, if configured)
///   7.  Subimage detection
///   8.  Blank page detection
///   9.  Canvas resolution
///   10. Margin application / content positioning
///   10b. Movement limit enforcement
///   11. Resize
///
/// @param image    Source image to process.
/// @param profile  Processing profile with all step configurations.
/// @param page_index  0-based page index (for odd/even margin selection).
/// @return Processing result with the output image and step log.
[[nodiscard]] ProcessingResult run_pipeline(
    const Image& image,
    const ProcessingProfile& profile,
    std::size_t page_index = 0);

/// Run a single named step of the pipeline.  Useful for testing or
/// preview of individual operations.
///
/// Supported step names: "rotate", "color_dropout", "edge_cleanup", "deskew",
/// "hole_cleanup", "despeckle", "blank_page", "detect_subimage",
/// "margins", "resize".
[[nodiscard]] ProcessingResult run_step(
    const Image& image,
    const ProcessingProfile& profile,
    const std::string& step_name,
    std::size_t page_index = 0);

} // namespace ppp::core

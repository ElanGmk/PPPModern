#pragma once

#include "ppp/core/geometry.h"
#include "ppp/core/image.h"
#include "ppp/core/processing_config.h"

#include <optional>
#include <vector>

namespace ppp::core::ops {

// ---------------------------------------------------------------------------
// Unit conversion
// ---------------------------------------------------------------------------

/// Convert a Measurement value to pixels given a DPI.
[[nodiscard]] double to_pixels(const Measurement& m, double dpi) noexcept;

/// Convert a pixel count to a Measurement in the given unit.
[[nodiscard]] Measurement from_pixels(double pixels, double dpi,
                                      MeasurementUnit unit) noexcept;

// ---------------------------------------------------------------------------
// Subimage detection — find content bounding box
// ---------------------------------------------------------------------------

/// Result of subimage detection.
struct SubimageResult {
    geometry::Rect bounds;          ///< Bounding rect of detected content.
    bool too_small{false};          ///< Content smaller than minimum size.
    std::vector<geometry::Component> components;  ///< Individual components found.
};

/// Detect the subimage (content region) in a BW1 image.
///
/// Converts non-BW1 images internally.  Uses connected-component labeling
/// to find foreground objects, filters by size, and returns the bounding
/// rectangle of all qualifying components.
///
/// @param image    Source image.
/// @param config   Subimage constraints (min size, max size).
/// @param dpi_x    Horizontal DPI (for unit conversion of max_width/height).
/// @param dpi_y    Vertical DPI.
/// @return Detection result with bounding rect and component list.
[[nodiscard]] SubimageResult detect_subimage(
    const Image& image,
    const SubimageConfig& config,
    double dpi_x,
    double dpi_y);

// ---------------------------------------------------------------------------
// Edge cleanup — erase border strips
// ---------------------------------------------------------------------------

/// Clear (set to background) a strip of pixels along each edge of a BW1 image.
///
/// Operates in-place.  The strip widths come from `EdgeValues`, converted
/// to pixels using the image DPI.
///
/// @param image    BW1 image to modify (in-place).
/// @param edges    Per-edge strip widths.
/// @param dpi_x    Horizontal DPI.
/// @param dpi_y    Vertical DPI.
void edge_cleanup(Image& image, const EdgeValues& edges,
                  double dpi_x, double dpi_y);

// ---------------------------------------------------------------------------
// Hole cleanup — remove punch-hole marks near edges
// ---------------------------------------------------------------------------

/// Remove connected components that lie within the specified edge margins
/// and are likely punch holes.
///
/// Operates on a BW1 image in-place.  Finds components within the edge
/// strips and removes (clears) those whose bounding box is roughly circular
/// (aspect ratio near 1:1) and within a plausible size range.
///
/// @param image    BW1 image to modify (in-place).
/// @param edges    Per-edge search strip widths.
/// @param dpi_x    Horizontal DPI.
/// @param dpi_y    Vertical DPI.
void hole_cleanup(Image& image, const EdgeValues& edges,
                  double dpi_x, double dpi_y);

// ---------------------------------------------------------------------------
// Despeckle — remove small noise components
// ---------------------------------------------------------------------------

/// Remove small connected components from a BW1 image.
///
/// @param image    BW1 image to modify (in-place).
/// @param config   Despeckle configuration (mode and size thresholds).
void despeckle(Image& image, const DespeckleConfig& config);

// ---------------------------------------------------------------------------
// Margin setting — position content within canvas
// ---------------------------------------------------------------------------

/// Result of margin application.
struct MarginResult {
    Image image;                    ///< Output image with margins applied.
    geometry::Rect content_rect;    ///< Where content ended up in the output.
};

/// Apply margins to position content within a canvas.
///
/// Creates a new image of `canvas_width` x `canvas_height` pixels and places
/// the content (defined by `subimage_bounds`) according to the margin config.
///
/// @param source           Source image.
/// @param subimage_bounds  Detected content bounds within source.
/// @param margins          Margin configuration (per-edge distances and modes).
/// @param canvas_width     Output canvas width in pixels.
/// @param canvas_height    Output canvas height in pixels.
/// @param keep_outside     If true, preserve pixels outside subimage bounds.
/// @param dpi_x            Horizontal DPI.
/// @param dpi_y            Vertical DPI.
/// @return Result with the output image and content placement.
[[nodiscard]] MarginResult apply_margins(
    const Image& source,
    const geometry::Rect& subimage_bounds,
    const MarginConfig& margins,
    std::int32_t canvas_width,
    std::int32_t canvas_height,
    bool keep_outside,
    double dpi_x,
    double dpi_y);

// ---------------------------------------------------------------------------
// Canvas sizing — resolve canvas dimensions from config
// ---------------------------------------------------------------------------

/// Resolve canvas dimensions in pixels from a CanvasConfig.
///
/// For presets (Letter, Legal, A4, etc.) returns the standard size at the
/// given DPI.  For Custom, converts the width/height measurements.
/// For Autodetect, returns the source image dimensions.
///
/// @param config   Canvas configuration.
/// @param dpi_x    Horizontal DPI.
/// @param dpi_y    Vertical DPI.
/// @param src_w    Source image width (used for Autodetect).
/// @param src_h    Source image height (used for Autodetect).
/// @return (width, height) in pixels.
struct CanvasDimensions {
    std::int32_t width;
    std::int32_t height;
};

[[nodiscard]] CanvasDimensions resolve_canvas(
    const CanvasConfig& config,
    double dpi_x,
    double dpi_y,
    std::int32_t src_w,
    std::int32_t src_h);

// ---------------------------------------------------------------------------
// Image scaling
// ---------------------------------------------------------------------------

/// Scale an image to new dimensions using nearest-neighbor interpolation.
[[nodiscard]] Image scale_nearest(const Image& image,
                                  std::int32_t new_width,
                                  std::int32_t new_height);

/// Scale an image to new dimensions using bilinear interpolation.
/// Only supports Gray8 and RGB24.  BW1 images are scaled with nearest-neighbor.
[[nodiscard]] Image scale_bilinear(const Image& image,
                                   std::int32_t new_width,
                                   std::int32_t new_height);

// ---------------------------------------------------------------------------
// Resize operation — full resize with alignment
// ---------------------------------------------------------------------------

/// Result of a resize operation.
struct ResizeResult {
    Image image;                    ///< Resized output image.
    geometry::Rect content_rect;    ///< Where content ended up in the output.
};

/// Apply the full resize operation as configured by ResizeConfig.
///
/// Determines the source region (subimage, full page, or custom), scales
/// it to fit the resize canvas (respecting allow_shrink/allow_enlarge),
/// and places it according to alignment settings.
///
/// @param source           Source image.
/// @param subimage_bounds  Detected content bounds (used when source=Subimage).
/// @param config           Resize configuration.
/// @param page_index       0-based page index (for odd/even set selection).
/// @param odd_even_mode    Whether odd/even page sets are active.
/// @return Resize result with output image.
[[nodiscard]] ResizeResult apply_resize(
    const Image& source,
    const geometry::Rect& subimage_bounds,
    const ResizeConfig& config,
    std::size_t page_index = 0,
    bool odd_even_mode = false);

} // namespace ppp::core::ops

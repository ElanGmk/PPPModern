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
// Blank page detection
// ---------------------------------------------------------------------------

/// Result of blank page detection.
struct BlankPageResult {
    bool is_blank{false};           ///< Whether the page is considered blank.
    double foreground_percent{0.0}; ///< Percentage of foreground pixels.
    std::int32_t component_count{0};///< Number of connected components found.
    std::int32_t total_pixels{0};   ///< Total pixels in the analysis region.
    std::int32_t foreground_pixels{0}; ///< Foreground pixel count.
};

/// Detect whether a page is blank based on foreground pixel density.
///
/// Converts non-BW1 images internally.  Optionally excludes an edge margin
/// from the analysis to ignore scanner artifacts.
///
/// @param image    Source image.
/// @param config   Blank page detection configuration.
/// @param dpi_x    Horizontal DPI (for edge margin conversion).
/// @param dpi_y    Vertical DPI.
/// @return Detection result with blank flag and metrics.
[[nodiscard]] BlankPageResult detect_blank_page(
    const Image& image,
    const BlankPageConfig& config,
    double dpi_x,
    double dpi_y);

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

// ---------------------------------------------------------------------------
// Deskew — detect and correct page skew
// ---------------------------------------------------------------------------

/// Result of skew angle detection.
struct DeskewResult {
    double angle{0.0};      ///< Detected skew angle in degrees (positive = CW).
    double confidence{0.0}; ///< Detection confidence (0.0 = no signal, 1.0 = strong).
    bool corrected{false};  ///< Whether the image was actually rotated.
    Image image;            ///< Corrected image (or copy of input if not corrected).
};

/// Detect the skew angle of a document image using projection profile analysis.
///
/// Projects foreground pixels at various angles and finds the angle that
/// produces the sharpest horizontal projection (highest variance), which
/// corresponds to text lines being perfectly horizontal.
///
/// @param image        Source image (converts to BW1 internally if needed).
/// @param min_angle    Minimum angle to search (degrees, default -5.0).
/// @param max_angle    Maximum angle to search (degrees, default +5.0).
/// @param step         Angle search step (degrees, default 0.1).
/// @return Detected skew angle in degrees.
[[nodiscard]] double detect_skew_angle(
    const Image& image,
    double min_angle = -5.0,
    double max_angle = 5.0,
    double step = 0.1);

/// Rotate an image by an arbitrary angle (degrees, positive = clockwise).
///
/// Uses bilinear interpolation for Gray8/RGB24/RGBA32 and nearest-neighbor
/// for BW1.  The output image is large enough to contain the full rotated
/// content (no clipping).  Background is filled with white (0xFF for
/// grayscale/color, 0 for BW1).
///
/// @param image    Source image.
/// @param angle    Rotation angle in degrees.
/// @return Rotated image.
[[nodiscard]] Image rotate_arbitrary(const Image& image, double angle);

/// Detect and correct page skew according to DeskewConfig.
///
/// @param image    Source image.
/// @param config   Deskew configuration (min/max angle, enabled flag, etc.).
/// @return Deskew result with corrected image and detected angle.
[[nodiscard]] DeskewResult apply_deskew(const Image& image,
                                        const DeskewConfig& config);

// ---------------------------------------------------------------------------
// Morphological operations (BW1 images)
// ---------------------------------------------------------------------------

/// Structuring element shape for morphological operations.
enum class StructuringElement {
    Cross,      ///< 3x3 cross (4-connected).
    Square,     ///< 3x3 square (8-connected).
};

/// Dilate a BW1 image — grow foreground regions.
///
/// Each foreground pixel expands into its neighbors defined by the
/// structuring element.  Operates in-place.
///
/// @param image    BW1 image to modify.
/// @param element  Structuring element shape (Cross or Square).
/// @param iterations  Number of dilation passes (default 1).
void dilate(Image& image, StructuringElement element = StructuringElement::Square,
            int iterations = 1);

/// Erode a BW1 image — shrink foreground regions.
///
/// A foreground pixel is kept only if all its neighbors (defined by the
/// structuring element) are also foreground.  Operates in-place.
///
/// @param image    BW1 image to modify.
/// @param element  Structuring element shape (Cross or Square).
/// @param iterations  Number of erosion passes (default 1).
void erode(Image& image, StructuringElement element = StructuringElement::Square,
           int iterations = 1);

/// Morphological opening (erode then dilate) — removes small foreground noise.
[[nodiscard]] Image morph_open(const Image& image,
                                StructuringElement element = StructuringElement::Square,
                                int iterations = 1);

/// Morphological closing (dilate then erode) — fills small holes in foreground.
[[nodiscard]] Image morph_close(const Image& image,
                                 StructuringElement element = StructuringElement::Square,
                                 int iterations = 1);

// ---------------------------------------------------------------------------
// Color dropout — remove a color channel before binarization
// ---------------------------------------------------------------------------

/// Apply color dropout to an RGB/RGBA image.
///
/// For each pixel, if the target color channel dominates (its value minus the
/// average of the other two channels exceeds the threshold), the pixel is set
/// to white.  This removes pre-printed colored form lines/backgrounds while
/// preserving black content.
///
/// Returns the modified image.  BW1/Gray8 inputs are returned unchanged.
///
/// @param image    Source image (RGB24 or RGBA32 for meaningful results).
/// @param config   Color dropout configuration.
/// @return Image with the specified color removed.
[[nodiscard]] Image color_dropout(const Image& image,
                                   const ColorDropoutConfig& config);

// ---------------------------------------------------------------------------
// Histogram and auto-threshold
// ---------------------------------------------------------------------------

/// 256-bin histogram for grayscale images.
struct Histogram {
    std::array<std::int32_t, 256> bins{};   ///< Pixel count per intensity level.
    std::int32_t total_pixels{0};           ///< Sum of all bins.

    /// Normalized frequency for a given bin (0.0–1.0).
    [[nodiscard]] double frequency(int bin) const noexcept {
        if (total_pixels == 0 || bin < 0 || bin > 255) return 0.0;
        return static_cast<double>(bins[bin]) / static_cast<double>(total_pixels);
    }

    /// Mean intensity (0.0–255.0).
    [[nodiscard]] double mean() const noexcept;

    /// Median intensity.
    [[nodiscard]] std::uint8_t median() const noexcept;

    /// Minimum non-zero bin.
    [[nodiscard]] std::uint8_t min_value() const noexcept;

    /// Maximum non-zero bin.
    [[nodiscard]] std::uint8_t max_value() const noexcept;
};

/// Compute a grayscale histogram from an image.
///
/// For Gray8, uses pixel values directly.  For RGB24/RGBA32, converts to
/// luminance (0.299R + 0.587G + 0.114B).  For BW1, produces a two-peak
/// histogram at bins 0 and 255.
///
/// @param image  Source image.
/// @return Histogram with 256 bins.
[[nodiscard]] Histogram compute_histogram(const Image& image);

/// Compute the optimal binarization threshold using Otsu's method.
///
/// Finds the threshold that minimizes intra-class variance (equivalently
/// maximizes inter-class variance) between foreground and background.
///
/// @param hist  Grayscale histogram.
/// @return Optimal threshold (0–255).
[[nodiscard]] std::uint8_t otsu_threshold(const Histogram& hist);

/// Binarize a grayscale image using a given threshold.
///
/// Pixels with intensity <= threshold become foreground (1), others
/// become background (0).  Returns a BW1 image.
///
/// @param image      Source image (Gray8, RGB24, or RGBA32).
/// @param threshold  Binarization threshold (0–255).
/// @return BW1 image.
[[nodiscard]] Image binarize(const Image& image, std::uint8_t threshold);

/// Binarize using Otsu's automatic threshold.
///
/// Convenience function that computes the histogram, finds the Otsu
/// threshold, and binarizes in one call.
///
/// @param image  Source image.
/// @return BW1 image.
[[nodiscard]] Image binarize_otsu(const Image& image);

} // namespace ppp::core::ops

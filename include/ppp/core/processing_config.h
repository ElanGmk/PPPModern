#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ppp::core {

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Measurement unit for margins, offsets, and cleanup distances.
enum class MeasurementUnit : std::uint8_t {
    Inches = 0,
    Pixels = 1,
    Millimeters = 2
};

/// How a margin value is applied.
enum class MarginMode : std::uint8_t {
    Set = 0,   ///< Force this margin value.
    Check = 1  ///< Validate/adjust if needed.
};

/// Despeckle processing mode.
enum class DespeckleMode : std::uint8_t {
    None = 0,
    SinglePixel = 1,
    Object = 2
};

/// Image rotation applied before processing.
enum class Rotation : std::uint8_t {
    None = 0,
    CW90 = 1,   ///< Clockwise 90 degrees.
    CCW90 = 2,  ///< Counter-clockwise 90 degrees.
    R180 = 3    ///< 180 degrees.
};

/// Predefined canvas/page sizes.
enum class CanvasPreset : std::uint8_t {
    Autodetect = 0,
    Letter = 1,
    Legal = 2,
    Tabloid = 3,
    A4 = 4,
    A3 = 5,
    Custom = 6
};

/// Page orientation.
enum class Orientation : std::uint8_t {
    Portrait = 0,
    Landscape = 1
};

/// When edge cleanup is applied relative to deskew.
enum class EdgeCleanupOrder : std::uint8_t {
    BeforeDeskew = 0,
    AfterDeskew = 1
};

/// Raster output compression format.
enum class RasterFormat : std::uint8_t {
    Raw = 0,
    Group4 = 1,
    LZW = 2,
    JPEG = 3
};

/// Source region for resize operations.
enum class ResizeFrom : std::uint8_t {
    Subimage = 0,
    FullPage = 1,
    Custom = 2,
    Smart = 3
};

/// Vertical alignment for resized content.
enum class VAlignment : std::uint8_t {
    Top = 1,
    Center = 2,
    Bottom = 3,
    Proportional = 4
};

/// Horizontal alignment for resized content.
enum class HAlignment : std::uint8_t {
    Center = 2,
    Proportional = 4
};

/// How to handle output file conflicts.
enum class ConflictPolicy : std::uint8_t {
    Report = 0,
    Overwrite = 1
};

/// Path encoding mode for output paths.
enum class PathMode : std::uint8_t {
    Absolute = 0,
    Portable = 1  ///< Network-safe relative paths.
};

// ---------------------------------------------------------------------------
// Value-with-unit helper
// ---------------------------------------------------------------------------

/// A measured value paired with its unit.
struct Measurement {
    double value{0.0};
    MeasurementUnit unit{MeasurementUnit::Inches};
};

// ---------------------------------------------------------------------------
// Per-edge value sets (used for margins, edge cleanup, hole cleanup, resize)
// ---------------------------------------------------------------------------

/// Four-edge measurement (top, left, right, bottom).
struct EdgeValues {
    Measurement top;
    Measurement left;
    Measurement right;
    Measurement bottom;
};

// ---------------------------------------------------------------------------
// Margin configuration (one set per odd/even page)
// ---------------------------------------------------------------------------

/// Per-edge margin with mode (set vs. check) and centering options.
struct MarginEdge {
    Measurement distance;
    MarginMode mode{MarginMode::Set};
};

struct MarginConfig {
    MarginEdge top;
    MarginEdge left{{0.0, MeasurementUnit::Inches}, MarginMode::Set};
    MarginEdge right{{0.0, MeasurementUnit::Inches}, MarginMode::Check};
    MarginEdge bottom{{0.0, MeasurementUnit::Inches}, MarginMode::Check};

    bool center_horizontal{false};
    bool center_vertical{false};

    bool keep_horizontal{false};
    bool keep_vertical{false};
    Measurement keep_x;
    Measurement keep_y;
    bool keep_h_center{false};
    bool keep_v_center{false};
};

// ---------------------------------------------------------------------------
// Canvas / output page size
// ---------------------------------------------------------------------------

struct CanvasConfig {
    CanvasPreset preset{CanvasPreset::Letter};
    Measurement width{8.5, MeasurementUnit::Inches};
    Measurement height{11.0, MeasurementUnit::Inches};
    Orientation orientation{Orientation::Portrait};
};

// ---------------------------------------------------------------------------
// Deskew configuration
// ---------------------------------------------------------------------------

struct DeskewConfig {
    bool enabled{false};
    std::int32_t detect_mode{2};
    double min_angle{0.05};       ///< Minimum skew to correct (degrees).
    double max_angle{3.0};        ///< Maximum skew to correct (degrees).
    bool fast{false};
    bool border_protect{false};
    bool interpolate{false};
    bool character_protect{false};
    double char_protect_below{1.0};  ///< Max skew for character protection.
    std::int32_t algorithm{0};
    bool report_no_skew{false};
};

// ---------------------------------------------------------------------------
// Despeckle configuration
// ---------------------------------------------------------------------------

struct DespeckleConfig {
    DespeckleMode mode{DespeckleMode::None};
    std::int32_t object_min{1};  ///< Min object size (Object mode).
    std::int32_t object_max{1};  ///< Max object size (Object mode).
};

// ---------------------------------------------------------------------------
// Edge cleanup configuration (supports odd/even page sets)
// ---------------------------------------------------------------------------

struct EdgeCleanupConfig {
    bool enabled{false};
    EdgeCleanupOrder order{EdgeCleanupOrder::BeforeDeskew};
    EdgeValues set1;  ///< Odd pages (or all pages if odd_even disabled).
    EdgeValues set2;  ///< Even pages.
};

// ---------------------------------------------------------------------------
// Hole (punch) cleanup configuration
// ---------------------------------------------------------------------------

struct HoleCleanupConfig {
    bool enabled{false};
    EdgeValues set1;
    EdgeValues set2;
};

// ---------------------------------------------------------------------------
// Subimage constraints
// ---------------------------------------------------------------------------

struct SubimageConfig {
    Measurement max_width;
    Measurement max_height;
    bool report_small{false};
    std::int32_t min_width_px{20};
    std::int32_t min_height_px{20};
};

// ---------------------------------------------------------------------------
// Blank page detection
// ---------------------------------------------------------------------------

struct BlankPageConfig {
    bool enabled{false};
    /// Foreground pixel percentage threshold.  If the percentage of foreground
    /// pixels is below this value, the page is considered blank.
    double threshold_percent{0.5};
    /// Minimum number of connected components to consider the page non-blank.
    /// A page with fewer components than this (even if above the pixel
    /// threshold) may still be marked blank if the components are noise.
    std::int32_t min_components{0};
    /// Edge margin to ignore when computing blank page metrics.  Helps
    /// exclude scanner artifacts at the edges.
    Measurement edge_margin{0.0, MeasurementUnit::Inches};
};

// ---------------------------------------------------------------------------
// Movement limits
// ---------------------------------------------------------------------------

struct MovementLimitConfig {
    bool enabled{false};
    Measurement max_vertical{6.0, MeasurementUnit::Inches};
    Measurement max_horizontal{4.0, MeasurementUnit::Inches};
};

// ---------------------------------------------------------------------------
// Offset / positioning (supports odd/even page sets)
// ---------------------------------------------------------------------------

struct OffsetConfig {
    Measurement x;
    Measurement y;
};

// ---------------------------------------------------------------------------
// Resize configuration
// ---------------------------------------------------------------------------

struct ResizeConfig {
    bool enabled{false};

    EdgeValues margins_set1;       ///< Resize margins for odd pages.
    EdgeValues margins_set2;       ///< Resize margins for even pages.

    ResizeFrom source{ResizeFrom::Subimage};
    EdgeValues source_set1;        ///< "Resize from" margins (odd).
    EdgeValues source_set2;        ///< "Resize from" margins (even).

    bool anti_alias{false};
    bool keep_size{true};

    CanvasConfig canvas;           ///< Resize output canvas.

    VAlignment v_alignment{VAlignment::Center};
    HAlignment h_alignment{HAlignment::Center};

    bool allow_shrink{true};
    bool allow_enlarge{true};

    std::string output_path;
    bool merge_tiff{false};
    bool merge_pdf{false};
    std::string merge_tiff_name;
    std::string merge_pdf_name;
};

// ---------------------------------------------------------------------------
// Output format
// ---------------------------------------------------------------------------

struct OutputConfig {
    bool tiff_output{true};
    bool pdf_output{false};
    RasterFormat raster_format{RasterFormat::JPEG};
    std::int32_t jpeg_quality{75};
    std::string new_extension;

    bool save_to_different_dir{true};
    std::string output_directory;
    ConflictPolicy conflict_policy{ConflictPolicy::Overwrite};
    PathMode path_mode{PathMode::Portable};
};

// ---------------------------------------------------------------------------
// Scan parameters
// ---------------------------------------------------------------------------

struct ScanConfig {
    std::string prefix{"01_"};
    std::int32_t start_at{1};
    std::int32_t increment{1};
    std::string extension{"tif"};
    bool crop{false};
    bool verso_recto{false};
    bool invert{false};
    std::int32_t transfer_mode{0};
    std::int32_t compression{0};
    std::int32_t force_resolution{-1};  ///< -1 = auto-detect.

    EdgeValues crop_verso;
    EdgeValues crop_recto;
};

// ---------------------------------------------------------------------------
// Print parameters
// ---------------------------------------------------------------------------

struct PrintConfig {
    std::int32_t page_use_mode{0};  ///< 0 = printable area, 1 = whole page.
    std::int32_t scale_mode{0};     ///< 0 = 1:1, 1 = fit page, 2 = custom.
    double scale_x{100.0};
    double scale_y{100.0};
};

// ---------------------------------------------------------------------------
// Page detection parameters (per-resolution preset)
// ---------------------------------------------------------------------------

struct PageDetectionParams {
    std::int32_t resolution{300};
    std::int32_t max_small_object_size{12};
    std::int32_t max_pixel_object_distance{10};
    std::int32_t max_small_object_distance{5};
    std::int32_t height_count{20};
    std::vector<std::int32_t> height_list;
};

/// Named page detection style sheet containing params for multiple resolutions.
struct PageDetectionProfile {
    std::string name{"Factory defaults"};
    std::vector<PageDetectionParams> params;  ///< One entry per supported DPI.
};

// ---------------------------------------------------------------------------
// Top-level processing profile (replaces TProjectSettings)
// ---------------------------------------------------------------------------

/// Complete processing configuration for a PPP job.
/// This is the central domain object that drives all image processing decisions.
struct ProcessingProfile {
    std::string name;

    // Image detection
    MeasurementUnit working_unit{MeasurementUnit::Inches};
    std::int32_t detected_dpi{-1};
    std::int32_t detected_width{-1};
    std::int32_t detected_height{-1};

    // Page setup
    bool position_image{true};
    bool keep_outside_subimage{false};
    bool keep_original_size{true};
    bool odd_even_mode{false};
    Rotation rotation{Rotation::None};
    CanvasConfig canvas;

    // Margins (index 0 = odd/all, index 1 = even)
    std::array<MarginConfig, 2> margins;

    // Offsets (index 0 = odd/all, index 1 = even)
    std::array<OffsetConfig, 2> offsets;

    // Processing steps
    DeskewConfig deskew;
    DespeckleConfig despeckle;
    EdgeCleanupConfig edge_cleanup;
    HoleCleanupConfig hole_cleanup;
    SubimageConfig subimage;
    BlankPageConfig blank_page;
    MovementLimitConfig movement_limit;

    // Resize
    ResizeConfig resize;

    // Output
    OutputConfig output;

    // Scan
    ScanConfig scan;

    // Print
    PrintConfig print;

    // Page detection
    std::string page_detection_style_sheet{"Factory defaults"};
};

// ---------------------------------------------------------------------------
// String conversions
// ---------------------------------------------------------------------------

[[nodiscard]] std::string_view to_string(MeasurementUnit unit) noexcept;
[[nodiscard]] std::string_view to_string(MarginMode mode) noexcept;
[[nodiscard]] std::string_view to_string(DespeckleMode mode) noexcept;
[[nodiscard]] std::string_view to_string(Rotation rotation) noexcept;
[[nodiscard]] std::string_view to_string(CanvasPreset preset) noexcept;
[[nodiscard]] std::string_view to_string(Orientation orientation) noexcept;
[[nodiscard]] std::string_view to_string(RasterFormat format) noexcept;
[[nodiscard]] std::string_view to_string(ResizeFrom source) noexcept;
[[nodiscard]] std::string_view to_string(VAlignment alignment) noexcept;
[[nodiscard]] std::string_view to_string(HAlignment alignment) noexcept;

[[nodiscard]] std::optional<MeasurementUnit> measurement_unit_from_string(std::string_view s) noexcept;
[[nodiscard]] std::optional<CanvasPreset> canvas_preset_from_string(std::string_view s) noexcept;
[[nodiscard]] std::optional<RasterFormat> raster_format_from_string(std::string_view s) noexcept;

} // namespace ppp::core

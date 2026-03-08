#include "ppp/core/processing_config.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace ppp::core {

namespace {
[[nodiscard]] std::string normalized(std::string_view value) {
    std::string result(value.begin(), value.end());
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}
} // namespace

// ---------------------------------------------------------------------------
// to_string implementations
// ---------------------------------------------------------------------------

std::string_view to_string(MeasurementUnit unit) noexcept {
    switch (unit) {
    case MeasurementUnit::Inches:
        return "inches";
    case MeasurementUnit::Pixels:
        return "pixels";
    case MeasurementUnit::Millimeters:
        return "millimeters";
    }
    return "unknown";
}

std::string_view to_string(MarginMode mode) noexcept {
    switch (mode) {
    case MarginMode::Set:
        return "set";
    case MarginMode::Check:
        return "check";
    }
    return "unknown";
}

std::string_view to_string(DespeckleMode mode) noexcept {
    switch (mode) {
    case DespeckleMode::None:
        return "none";
    case DespeckleMode::SinglePixel:
        return "single_pixel";
    case DespeckleMode::Object:
        return "object";
    }
    return "unknown";
}

std::string_view to_string(Rotation rotation) noexcept {
    switch (rotation) {
    case Rotation::None:
        return "none";
    case Rotation::CW90:
        return "cw90";
    case Rotation::CCW90:
        return "ccw90";
    case Rotation::R180:
        return "r180";
    }
    return "unknown";
}

std::string_view to_string(CanvasPreset preset) noexcept {
    switch (preset) {
    case CanvasPreset::Autodetect:
        return "autodetect";
    case CanvasPreset::Letter:
        return "letter";
    case CanvasPreset::Legal:
        return "legal";
    case CanvasPreset::Tabloid:
        return "tabloid";
    case CanvasPreset::A4:
        return "a4";
    case CanvasPreset::A3:
        return "a3";
    case CanvasPreset::Custom:
        return "custom";
    }
    return "unknown";
}

std::string_view to_string(Orientation orientation) noexcept {
    switch (orientation) {
    case Orientation::Portrait:
        return "portrait";
    case Orientation::Landscape:
        return "landscape";
    }
    return "unknown";
}

std::string_view to_string(RasterFormat format) noexcept {
    switch (format) {
    case RasterFormat::Raw:
        return "raw";
    case RasterFormat::Group4:
        return "group4";
    case RasterFormat::LZW:
        return "lzw";
    case RasterFormat::JPEG:
        return "jpeg";
    }
    return "unknown";
}

std::string_view to_string(ResizeFrom source) noexcept {
    switch (source) {
    case ResizeFrom::Subimage:
        return "subimage";
    case ResizeFrom::FullPage:
        return "full_page";
    case ResizeFrom::Custom:
        return "custom";
    case ResizeFrom::Smart:
        return "smart";
    }
    return "unknown";
}

std::string_view to_string(VAlignment alignment) noexcept {
    switch (alignment) {
    case VAlignment::Top:
        return "top";
    case VAlignment::Center:
        return "center";
    case VAlignment::Bottom:
        return "bottom";
    case VAlignment::Proportional:
        return "proportional";
    }
    return "unknown";
}

std::string_view to_string(HAlignment alignment) noexcept {
    switch (alignment) {
    case HAlignment::Center:
        return "center";
    case HAlignment::Proportional:
        return "proportional";
    }
    return "unknown";
}

std::string_view to_string(EdgeCleanupOrder order) noexcept {
    switch (order) {
    case EdgeCleanupOrder::BeforeDeskew:
        return "before_deskew";
    case EdgeCleanupOrder::AfterDeskew:
        return "after_deskew";
    }
    return "unknown";
}

std::string_view to_string(ConflictPolicy policy) noexcept {
    switch (policy) {
    case ConflictPolicy::Report:
        return "report";
    case ConflictPolicy::Overwrite:
        return "overwrite";
    }
    return "unknown";
}

std::string_view to_string(PathMode mode) noexcept {
    switch (mode) {
    case PathMode::Absolute:
        return "absolute";
    case PathMode::Portable:
        return "portable";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// from_string implementations
// ---------------------------------------------------------------------------

std::optional<MeasurementUnit> measurement_unit_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "inches" || v == "in") return MeasurementUnit::Inches;
    if (v == "pixels" || v == "px") return MeasurementUnit::Pixels;
    if (v == "millimeters" || v == "mm") return MeasurementUnit::Millimeters;
    return std::nullopt;
}

std::optional<CanvasPreset> canvas_preset_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "autodetect" || v == "auto") return CanvasPreset::Autodetect;
    if (v == "letter") return CanvasPreset::Letter;
    if (v == "legal") return CanvasPreset::Legal;
    if (v == "tabloid") return CanvasPreset::Tabloid;
    if (v == "a4") return CanvasPreset::A4;
    if (v == "a3") return CanvasPreset::A3;
    if (v == "custom") return CanvasPreset::Custom;
    return std::nullopt;
}

std::optional<RasterFormat> raster_format_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "raw") return RasterFormat::Raw;
    if (v == "group4" || v == "g4") return RasterFormat::Group4;
    if (v == "lzw") return RasterFormat::LZW;
    if (v == "jpeg" || v == "jpg") return RasterFormat::JPEG;
    return std::nullopt;
}

std::optional<MarginMode> margin_mode_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "set") return MarginMode::Set;
    if (v == "check") return MarginMode::Check;
    return std::nullopt;
}

std::optional<DespeckleMode> despeckle_mode_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "none") return DespeckleMode::None;
    if (v == "single_pixel") return DespeckleMode::SinglePixel;
    if (v == "object") return DespeckleMode::Object;
    return std::nullopt;
}

std::optional<Rotation> rotation_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "none") return Rotation::None;
    if (v == "cw90") return Rotation::CW90;
    if (v == "ccw90") return Rotation::CCW90;
    if (v == "r180") return Rotation::R180;
    return std::nullopt;
}

std::optional<Orientation> orientation_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "portrait") return Orientation::Portrait;
    if (v == "landscape") return Orientation::Landscape;
    return std::nullopt;
}

std::optional<ResizeFrom> resize_from_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "subimage") return ResizeFrom::Subimage;
    if (v == "full_page") return ResizeFrom::FullPage;
    if (v == "custom") return ResizeFrom::Custom;
    if (v == "smart") return ResizeFrom::Smart;
    return std::nullopt;
}

std::optional<VAlignment> v_alignment_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "top") return VAlignment::Top;
    if (v == "center") return VAlignment::Center;
    if (v == "bottom") return VAlignment::Bottom;
    if (v == "proportional") return VAlignment::Proportional;
    return std::nullopt;
}

std::optional<HAlignment> h_alignment_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "center") return HAlignment::Center;
    if (v == "proportional") return HAlignment::Proportional;
    return std::nullopt;
}

std::optional<EdgeCleanupOrder> edge_cleanup_order_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "before_deskew") return EdgeCleanupOrder::BeforeDeskew;
    if (v == "after_deskew") return EdgeCleanupOrder::AfterDeskew;
    return std::nullopt;
}

std::optional<ConflictPolicy> conflict_policy_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "report") return ConflictPolicy::Report;
    if (v == "overwrite") return ConflictPolicy::Overwrite;
    return std::nullopt;
}

std::optional<PathMode> path_mode_from_string(std::string_view s) noexcept {
    const auto v = normalized(s);
    if (v == "absolute") return PathMode::Absolute;
    if (v == "portable") return PathMode::Portable;
    return std::nullopt;
}

} // namespace ppp::core

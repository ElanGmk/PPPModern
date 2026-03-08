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

} // namespace ppp::core

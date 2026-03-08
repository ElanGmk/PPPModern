#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ppp::core::geometry {

// ---------------------------------------------------------------------------
// Axis-aligned rectangle (integer coordinates, pixel space)
// ---------------------------------------------------------------------------

struct Rect {
    std::int32_t left{0};
    std::int32_t top{0};
    std::int32_t right{0};
    std::int32_t bottom{0};

    [[nodiscard]] std::int32_t width() const noexcept { return right - left; }
    [[nodiscard]] std::int32_t height() const noexcept { return bottom - top; }
    [[nodiscard]] std::int64_t area() const noexcept {
        return static_cast<std::int64_t>(width()) * height();
    }
    [[nodiscard]] bool empty() const noexcept {
        return left >= right || top >= bottom;
    }

    [[nodiscard]] bool contains(std::int32_t x, std::int32_t y) const noexcept {
        return x >= left && x < right && y >= top && y < bottom;
    }
    [[nodiscard]] bool contains(const Rect& other) const noexcept {
        return other.left >= left && other.right <= right &&
               other.top >= top && other.bottom <= bottom;
    }

    [[nodiscard]] bool intersects(const Rect& other) const noexcept {
        return left < other.right && right > other.left &&
               top < other.bottom && bottom > other.top;
    }

    [[nodiscard]] Rect intersection(const Rect& other) const noexcept {
        Rect r{std::max(left, other.left), std::max(top, other.top),
               std::min(right, other.right), std::min(bottom, other.bottom)};
        if (r.left >= r.right || r.top >= r.bottom) return {};
        return r;
    }

    [[nodiscard]] Rect united(const Rect& other) const noexcept {
        if (empty()) return other;
        if (other.empty()) return *this;
        return {std::min(left, other.left), std::min(top, other.top),
                std::max(right, other.right), std::max(bottom, other.bottom)};
    }

    void inflate(std::int32_t dx, std::int32_t dy) noexcept {
        left -= dx;
        top -= dy;
        right += dx;
        bottom += dy;
    }

    void offset(std::int32_t dx, std::int32_t dy) noexcept {
        left += dx;
        top += dy;
        right += dx;
        bottom += dy;
    }

    [[nodiscard]] bool operator==(const Rect& o) const noexcept = default;
};

// ---------------------------------------------------------------------------
// Combinability test (zone merging criterion)
// ---------------------------------------------------------------------------

/// Two rects are combinable if the union has no more area than the sum of
/// the individual areas plus `tolerance` pixels.  This is the PPP "zone
/// merging" heuristic used to decide whether adjacent zones should merge.
[[nodiscard]] inline bool combinable(const Rect& a, const Rect& b,
                                     std::int64_t tolerance = 0) noexcept {
    auto u = a.united(b);
    return u.area() <= a.area() + b.area() + tolerance;
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

/// Sort axis for rect ordering.
enum class SortAxis : std::uint8_t {
    TopToBottom = 0,
    LeftToRight = 1,
    BottomToTop = 2,
    RightToLeft = 3
};

[[nodiscard]] std::string_view to_string(SortAxis axis) noexcept;

/// Sort a vector of rects along the given axis (stable sort).
void sort_rects(std::vector<Rect>& rects, SortAxis axis);

// ---------------------------------------------------------------------------
// Banding — group rects into horizontal or vertical bands
// ---------------------------------------------------------------------------

/// Band direction.
enum class BandDirection : std::uint8_t {
    Horizontal = 0,  ///< Rows of rects (grouped by Y overlap).
    Vertical = 1     ///< Columns of rects (grouped by X overlap).
};

/// Group rectangles into bands.  Rects that overlap on the band axis are
/// placed in the same band.  Each band is sorted by the cross-axis.
/// Returns a vector of bands, each band being a vector of Rect.
[[nodiscard]] std::vector<std::vector<Rect>> band_rects(
    const std::vector<Rect>& rects, BandDirection direction);

// ---------------------------------------------------------------------------
// RLE span — run-length-encoded row segment for binary images
// ---------------------------------------------------------------------------

/// One horizontal run of foreground pixels in a binary image row.
struct Span {
    std::int32_t row{0};
    std::int32_t col_start{0};  ///< First foreground column (inclusive).
    std::int32_t col_end{0};    ///< Last foreground column (exclusive).

    [[nodiscard]] std::int32_t length() const noexcept { return col_end - col_start; }
};

// ---------------------------------------------------------------------------
// Connected-component labeling on RLE-encoded binary images
// ---------------------------------------------------------------------------

/// A connected component found by `find_components`.
struct Component {
    Rect bounds;                ///< Bounding rectangle.
    std::int64_t pixel_count;   ///< Number of foreground pixels.
};

/// Find connected components in a binary image represented as RLE spans.
///
/// Uses single-pass union-find on the span list.  Spans must be sorted by
/// row (ascending) then by col_start (ascending) within each row.
///
/// @param spans        RLE-encoded foreground spans.
/// @param roi          Region of interest (only spans inside roi are used).
///                     Pass a large rect to include everything.
/// @param connectivity 4 or 8 (default 4).
/// @return Vector of connected components.
[[nodiscard]] std::vector<Component> find_components(
    const std::vector<Span>& spans,
    const Rect& roi,
    int connectivity = 4);

/// Convenience: extract RLE spans from a packed 1-bit-per-pixel binary image.
/// `data` points to a row-major bitmap where bit 0 = leftmost pixel of the
/// first byte.  `stride` is the number of bytes per row (may include padding).
/// Foreground pixels have value `fg_bit` (0 or 1; default 1).
[[nodiscard]] std::vector<Span> spans_from_bitmap(
    const std::uint8_t* data,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride,
    int fg_bit = 1);

} // namespace ppp::core::geometry

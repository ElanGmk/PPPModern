#include "ppp/core/geometry.h"

#include <algorithm>
#include <cassert>
#include <numeric>

namespace ppp::core::geometry {

// ---------------------------------------------------------------------------
// SortAxis to_string
// ---------------------------------------------------------------------------

std::string_view to_string(SortAxis axis) noexcept {
    switch (axis) {
        case SortAxis::TopToBottom: return "top_to_bottom";
        case SortAxis::LeftToRight: return "left_to_right";
        case SortAxis::BottomToTop: return "bottom_to_top";
        case SortAxis::RightToLeft: return "right_to_left";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

void sort_rects(std::vector<Rect>& rects, SortAxis axis) {
    switch (axis) {
        case SortAxis::TopToBottom:
            std::stable_sort(rects.begin(), rects.end(),
                             [](const Rect& a, const Rect& b) {
                                 return a.top < b.top || (a.top == b.top && a.left < b.left);
                             });
            break;
        case SortAxis::LeftToRight:
            std::stable_sort(rects.begin(), rects.end(),
                             [](const Rect& a, const Rect& b) {
                                 return a.left < b.left || (a.left == b.left && a.top < b.top);
                             });
            break;
        case SortAxis::BottomToTop:
            std::stable_sort(rects.begin(), rects.end(),
                             [](const Rect& a, const Rect& b) {
                                 return a.bottom > b.bottom ||
                                        (a.bottom == b.bottom && a.left < b.left);
                             });
            break;
        case SortAxis::RightToLeft:
            std::stable_sort(rects.begin(), rects.end(),
                             [](const Rect& a, const Rect& b) {
                                 return a.right > b.right ||
                                        (a.right == b.right && a.top < b.top);
                             });
            break;
    }
}

// ---------------------------------------------------------------------------
// Banding
// ---------------------------------------------------------------------------

std::vector<std::vector<Rect>> band_rects(const std::vector<Rect>& rects,
                                          BandDirection direction) {
    if (rects.empty()) return {};

    // Sort by primary axis first.
    std::vector<Rect> sorted = rects;
    if (direction == BandDirection::Horizontal) {
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const Rect& a, const Rect& b) { return a.top < b.top; });
    } else {
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const Rect& a, const Rect& b) { return a.left < b.left; });
    }

    std::vector<std::vector<Rect>> bands;
    bands.push_back({sorted[0]});

    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const auto& r = sorted[i];
        auto& current_band = bands.back();

        // Compute the band extent on the primary axis.
        bool overlaps = false;
        if (direction == BandDirection::Horizontal) {
            std::int32_t band_top = current_band[0].top;
            std::int32_t band_bottom = current_band[0].bottom;
            for (const auto& b : current_band) {
                band_top = std::min(band_top, b.top);
                band_bottom = std::max(band_bottom, b.bottom);
            }
            overlaps = r.top < band_bottom && r.bottom > band_top;
        } else {
            std::int32_t band_left = current_band[0].left;
            std::int32_t band_right = current_band[0].right;
            for (const auto& b : current_band) {
                band_left = std::min(band_left, b.left);
                band_right = std::max(band_right, b.right);
            }
            overlaps = r.left < band_right && r.right > band_left;
        }

        if (overlaps) {
            current_band.push_back(r);
        } else {
            bands.push_back({r});
        }
    }

    // Sort each band by the cross-axis.
    for (auto& band : bands) {
        if (direction == BandDirection::Horizontal) {
            std::stable_sort(band.begin(), band.end(),
                             [](const Rect& a, const Rect& b) { return a.left < b.left; });
        } else {
            std::stable_sort(band.begin(), band.end(),
                             [](const Rect& a, const Rect& b) { return a.top < b.top; });
        }
    }

    return bands;
}

// ---------------------------------------------------------------------------
// RLE span extraction from 1-bpp bitmap
// ---------------------------------------------------------------------------

std::vector<Span> spans_from_bitmap(const std::uint8_t* data,
                                    std::int32_t width,
                                    std::int32_t height,
                                    std::int32_t stride,
                                    int fg_bit) {
    assert(fg_bit == 0 || fg_bit == 1);
    std::vector<Span> spans;

    for (std::int32_t row = 0; row < height; ++row) {
        const std::uint8_t* row_data = data + static_cast<std::ptrdiff_t>(row) * stride;
        std::int32_t col = 0;

        while (col < width) {
            // Find start of foreground run.
            while (col < width) {
                int byte_idx = col >> 3;
                int bit_idx = 7 - (col & 7);  // MSB-first within byte.
                int bit = (row_data[byte_idx] >> bit_idx) & 1;
                if (bit == fg_bit) break;
                ++col;
            }
            if (col >= width) break;

            std::int32_t start = col;

            // Find end of foreground run.
            while (col < width) {
                int byte_idx = col >> 3;
                int bit_idx = 7 - (col & 7);
                int bit = (row_data[byte_idx] >> bit_idx) & 1;
                if (bit != fg_bit) break;
                ++col;
            }

            spans.push_back({row, start, col});
        }
    }

    return spans;
}

// ---------------------------------------------------------------------------
// Union-Find for connected-component labeling
// ---------------------------------------------------------------------------

namespace {

class UnionFind {
public:
    explicit UnionFind(std::size_t n) : parent_(n), rank_(n, 0) {
        std::iota(parent_.begin(), parent_.end(), std::size_t{0});
    }

    std::size_t find(std::size_t x) {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];  // Path halving.
            x = parent_[x];
        }
        return x;
    }

    void unite(std::size_t a, std::size_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }

private:
    std::vector<std::size_t> parent_;
    std::vector<std::size_t> rank_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Connected-component labeling
// ---------------------------------------------------------------------------

std::vector<Component> find_components(const std::vector<Span>& spans,
                                       const Rect& roi,
                                       int connectivity) {
    assert(connectivity == 4 || connectivity == 8);

    // Filter spans to ROI.
    std::vector<Span> filtered;
    filtered.reserve(spans.size());
    for (const auto& s : spans) {
        if (s.row < roi.top || s.row >= roi.bottom) continue;
        std::int32_t cs = std::max(s.col_start, roi.left);
        std::int32_t ce = std::min(s.col_end, roi.right);
        if (cs < ce) {
            filtered.push_back({s.row, cs, ce});
        }
    }

    if (filtered.empty()) return {};

    const std::size_t n = filtered.size();
    UnionFind uf(n);

    // For each span, check adjacency with spans in the previous row.
    // Spans are sorted by (row, col_start).
    std::size_t prev_row_start = 0;
    std::size_t prev_row_end = 0;

    for (std::size_t i = 0; i < n; ++i) {
        const auto& cur = filtered[i];

        // Advance prev_row window: find spans in the immediately preceding row.
        if (i == 0 || filtered[i].row != filtered[i - 1].row) {
            // Starting a new row — find all spans from row - 1.
            prev_row_start = prev_row_end;  // Skip forward.
            // But we need to find spans in cur.row - 1.
            // Scan backward from current position to find the start of previous row spans.
        }

        // Simple approach: for each span, scan previous-row spans for adjacency.
        for (std::size_t j = 0; j < i; ++j) {
            const auto& prev = filtered[j];
            if (prev.row < cur.row - 1) continue;
            if (prev.row >= cur.row) break;  // Same row or later.

            // prev is in cur.row - 1.  Check horizontal overlap.
            bool adjacent;
            if (connectivity == 4) {
                // 4-connected: spans must overlap horizontally.
                adjacent = prev.col_start < cur.col_end && prev.col_end > cur.col_start;
            } else {
                // 8-connected: spans can touch diagonally (expand by 1 pixel).
                adjacent = prev.col_start < cur.col_end + 1 && prev.col_end > cur.col_start - 1;
            }

            if (adjacent) {
                uf.unite(i, j);
            }
        }
    }

    // Collect components.
    std::vector<std::size_t> roots(n);
    for (std::size_t i = 0; i < n; ++i) {
        roots[i] = uf.find(i);
    }

    // Map root → component index.
    std::vector<std::size_t> comp_map(n, SIZE_MAX);
    std::vector<Component> components;

    for (std::size_t i = 0; i < n; ++i) {
        std::size_t root = roots[i];
        if (comp_map[root] == SIZE_MAX) {
            comp_map[root] = components.size();
            components.push_back({{filtered[i].col_start, filtered[i].row,
                                   filtered[i].col_end, filtered[i].row + 1},
                                  0});
        }

        auto& comp = components[comp_map[root]];
        comp.bounds = comp.bounds.united(
            {filtered[i].col_start, filtered[i].row,
             filtered[i].col_end, filtered[i].row + 1});
        comp.pixel_count += filtered[i].length();
    }

    return components;
}

} // namespace ppp::core::geometry

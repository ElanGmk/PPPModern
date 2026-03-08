#include "ppp/core/bmp.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace ppp::core::bmp {

namespace {

// ---------------------------------------------------------------------------
// BMP file structure constants
// ---------------------------------------------------------------------------

constexpr std::uint16_t BMP_SIGNATURE = 0x4D42;  // 'BM' little-endian.

// Compression types.
constexpr std::uint32_t BI_RGB  = 0;
constexpr std::uint32_t BI_RLE8 = 1;
constexpr std::uint32_t BI_BITFIELDS = 3;

// BITMAPINFOHEADER size variants.
constexpr std::uint32_t BITMAPINFOHEADER_SIZE = 40;
constexpr std::uint32_t BITMAPV4HEADER_SIZE   = 108;
constexpr std::uint32_t BITMAPV5HEADER_SIZE   = 124;
constexpr std::uint32_t OS2_HEADER_SIZE       = 12;

// ---------------------------------------------------------------------------
// Little-endian read helpers
// ---------------------------------------------------------------------------

std::uint16_t read_u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(
        p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

std::int32_t read_i32(const std::uint8_t* p) {
    auto u = read_u32(p);
    std::int32_t v;
    std::memcpy(&v, &u, 4);
    return v;
}

// ---------------------------------------------------------------------------
// Little-endian write helpers
// ---------------------------------------------------------------------------

void write_u16(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void write_u32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void write_i32(std::vector<std::uint8_t>& buf, std::int32_t v) {
    std::uint32_t u;
    std::memcpy(&u, &v, 4);
    write_u32(buf, u);
}

// ---------------------------------------------------------------------------
// DPI ↔ pixels-per-meter conversion
// ---------------------------------------------------------------------------

double ppm_to_dpi(std::int32_t ppm) {
    if (ppm <= 0) return 96.0;
    return ppm / 39.3701;  // 1 inch = 0.0254 m → 1 m = 39.3701 inches.
}

std::int32_t dpi_to_ppm(double dpi) {
    if (dpi <= 0) dpi = 96.0;
    return static_cast<std::int32_t>(std::round(dpi * 39.3701));
}

// ---------------------------------------------------------------------------
// BMP stride — rows are padded to 4-byte boundaries
// ---------------------------------------------------------------------------

std::uint32_t bmp_stride(std::int32_t width, std::uint16_t bits_per_pixel) {
    std::uint32_t row_bits = static_cast<std::uint32_t>(width) * bits_per_pixel;
    std::uint32_t row_bytes = (row_bits + 7) / 8;
    return (row_bytes + 3) & ~3u;
}

// ---------------------------------------------------------------------------
// RLE8 decoder
// ---------------------------------------------------------------------------

bool decode_rle8(const std::uint8_t* src, std::size_t src_size,
                 std::uint8_t* dst, std::int32_t width, std::int32_t height,
                 std::uint32_t dst_stride) {
    std::size_t pos = 0;
    int x = 0, y = 0;

    while (pos + 1 < src_size && y < height) {
        std::uint8_t count = src[pos++];
        std::uint8_t value = src[pos++];

        if (count > 0) {
            // Encoded run.
            for (int i = 0; i < count && x < width; ++i) {
                dst[y * dst_stride + x] = value;
                ++x;
            }
        } else {
            // Escape.
            switch (value) {
                case 0:  // End of line.
                    x = 0;
                    ++y;
                    break;
                case 1:  // End of bitmap.
                    return true;
                case 2:  // Delta.
                    if (pos + 1 >= src_size) return false;
                    x += src[pos++];
                    y += src[pos++];
                    break;
                default: {
                    // Absolute mode: `value` literal bytes follow.
                    int n = value;
                    for (int i = 0; i < n && pos < src_size && x < width; ++i) {
                        dst[y * dst_stride + x] = src[pos++];
                        ++x;
                    }
                    // Absolute runs are word-aligned.
                    if (n & 1) {
                        if (pos < src_size) ++pos;
                    }
                    break;
                }
            }
        }
    }

    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// BMP reader
// ---------------------------------------------------------------------------

Image read_bmp(const std::uint8_t* data, std::size_t size) {
    // Minimum BMP: 14-byte file header + 12-byte OS/2 info header.
    if (!data || size < 26) return {};

    // --- File header (14 bytes) ---
    if (read_u16(data) != BMP_SIGNATURE) return {};

    // std::uint32_t file_size = read_u32(data + 2);  // Not always reliable.
    std::uint32_t pixel_offset = read_u32(data + 10);

    // --- Info header ---
    std::uint32_t header_size = read_u32(data + 14);
    if (14 + header_size > size) return {};

    std::int32_t  width         = 0;
    std::int32_t  height_signed = 0;
    std::uint16_t planes        = 0;
    std::uint16_t bpp           = 0;
    std::uint32_t compression   = BI_RGB;
    std::int32_t  x_ppm         = 0;
    std::int32_t  y_ppm         = 0;
    std::uint32_t colors_used   = 0;

    if (header_size == OS2_HEADER_SIZE) {
        // OS/2 BITMAPCOREHEADER (12 bytes).
        if (size < 14 + 12) return {};
        width         = static_cast<std::int32_t>(read_u16(data + 18));
        height_signed = static_cast<std::int32_t>(read_u16(data + 20));
        planes        = read_u16(data + 22);
        bpp           = read_u16(data + 24);
    } else if (header_size >= BITMAPINFOHEADER_SIZE) {
        // Windows BITMAPINFOHEADER (40+).
        if (size < 14 + 40) return {};
        width         = read_i32(data + 18);
        height_signed = read_i32(data + 22);
        planes        = read_u16(data + 26);
        bpp           = read_u16(data + 28);
        compression   = read_u32(data + 30);
        x_ppm         = read_i32(data + 38);
        y_ppm         = read_i32(data + 42);
        colors_used   = read_u32(data + 46);
    } else {
        return {};  // Unknown header variant.
    }

    if (width <= 0 || planes != 1) return {};

    bool top_down = (height_signed < 0);
    std::int32_t height = top_down ? -height_signed : height_signed;
    if (height <= 0) return {};

    // Determine pixel format.
    PixelFormat fmt;
    switch (bpp) {
        case 1:  fmt = PixelFormat::BW1;    break;
        case 8:  fmt = PixelFormat::Gray8;  break;
        case 24: fmt = PixelFormat::RGB24;  break;
        case 32: fmt = PixelFormat::RGBA32; break;
        default: return {};  // Unsupported bit depth.
    }

    // Only support BI_RGB, BI_RLE8, and BI_BITFIELDS.
    if (compression != BI_RGB && compression != BI_RLE8 &&
        compression != BI_BITFIELDS) {
        return {};
    }
    if (compression == BI_RLE8 && bpp != 8) return {};
    if (compression == BI_BITFIELDS && bpp != 16 && bpp != 32) return {};

    // Color table offset (immediately after info header).
    std::size_t palette_offset = 14 + header_size;

    // For BI_BITFIELDS, skip the 12-byte mask entries.
    if (compression == BI_BITFIELDS && header_size == BITMAPINFOHEADER_SIZE) {
        palette_offset += 12;
    }

    // Validate pixel data offset.
    if (pixel_offset >= size) return {};

    double dpi_x = ppm_to_dpi(x_ppm);
    double dpi_y = ppm_to_dpi(y_ppm);

    Image img(width, height, fmt, dpi_x, dpi_y);
    std::uint32_t src_stride = bmp_stride(width, bpp);

    if (compression == BI_RLE8) {
        // RLE8 decompression.
        img.fill(0);  // Clear to background.
        if (!decode_rle8(data + pixel_offset, size - pixel_offset,
                         img.row(0), width, height, img.stride())) {
            return {};
        }
        // RLE8 is always bottom-up; flip if needed.
        if (!top_down) {
            for (int y = 0; y < height / 2; ++y) {
                auto* row_a = img.row(y);
                auto* row_b = img.row(height - 1 - y);
                for (std::int32_t x = 0; x < static_cast<std::int32_t>(img.stride()); ++x) {
                    std::swap(row_a[x], row_b[x]);
                }
            }
        }
        return img;
    }

    // Uncompressed: copy row by row.
    for (std::int32_t y = 0; y < height; ++y) {
        // BMP bottom-up: row 0 in file is the bottom row.
        std::int32_t src_row = top_down ? y : (height - 1 - y);
        std::size_t src_offset = pixel_offset +
            static_cast<std::size_t>(src_row) * src_stride;

        if (src_offset + src_stride > size) return {};

        auto* dst = img.row(y);
        const auto* src = data + src_offset;

        if (bpp == 1) {
            // BMP 1-bit: palette entry 0 = background, entry 1 = foreground.
            // Our BW1: bit 1 = foreground.  BMP may have inverted palette.
            // Check palette to determine if we need to invert.
            // For now, copy raw — both use MSB-first packed bits.
            std::uint32_t copy_bytes = (static_cast<std::uint32_t>(width) + 7) / 8;
            std::memcpy(dst, src, copy_bytes);
        } else if (bpp == 8) {
            // 8-bit: might be paletted.  For grayscale, palette is identity.
            // Check if palette is grayscale or just copy indices.
            std::memcpy(dst, src, static_cast<std::size_t>(width));
        } else if (bpp == 24) {
            // BMP stores BGR; we need RGB.
            for (std::int32_t x = 0; x < width; ++x) {
                dst[x * 3 + 0] = src[x * 3 + 2];  // R.
                dst[x * 3 + 1] = src[x * 3 + 1];  // G.
                dst[x * 3 + 2] = src[x * 3 + 0];  // B.
            }
        } else if (bpp == 32) {
            // BMP stores BGRA; we need RGBA.
            for (std::int32_t x = 0; x < width; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];  // R.
                dst[x * 4 + 1] = src[x * 4 + 1];  // G.
                dst[x * 4 + 2] = src[x * 4 + 0];  // B.
                dst[x * 4 + 3] = src[x * 4 + 3];  // A.
            }
        }
    }

    // For 1-bit BMPs, check if palette inverts black/white.
    if (bpp == 1 && palette_offset + 8 <= size) {
        // BMP palette entries are BGRA (4 bytes each) or BGR (3 bytes for OS/2).
        std::size_t entry_size = (header_size == OS2_HEADER_SIZE) ? 3 : 4;
        if (palette_offset + 2 * entry_size <= size) {
            const auto* pal0 = data + palette_offset;
            // If palette[0] is white (0xFF) and palette[1] is black (0x00),
            // then BMP bit=0 means white and bit=1 means black.
            // That matches our BW1 convention (1 = foreground/black).
            // If palette is reversed, invert all bits.
            bool pal0_is_black = (pal0[0] == 0 && pal0[1] == 0 && pal0[2] == 0);
            if (pal0_is_black) {
                // palette[0] = black, palette[1] = white.
                // BMP bit=0 means black, but our BW1 bit=0 means white.
                // Need to invert.
                img.invert();
            }
            // If pal0 is white: bit=0=white, bit=1=black → matches our convention.
        }
    }

    return img;
}

Image read_bmp_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};

    f.seekg(0, std::ios::end);
    auto file_size = f.tellg();
    if (file_size <= 0) return {};
    f.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> buf(static_cast<std::size_t>(file_size));
    f.read(reinterpret_cast<char*>(buf.data()), file_size);
    if (!f) return {};

    return read_bmp(buf.data(), buf.size());
}

// ---------------------------------------------------------------------------
// BMP writer
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> write_bmp_to_memory(const Image& image,
                                                const WriteOptions& options) {
    if (image.empty()) return {};

    std::int32_t w = image.width();
    std::int32_t h = image.height();
    PixelFormat fmt = image.format();

    std::uint16_t bpp = 0;
    switch (fmt) {
        case PixelFormat::BW1:    bpp = 1;  break;
        case PixelFormat::Gray8:  bpp = 8;  break;
        case PixelFormat::RGB24:  bpp = 24; break;
        case PixelFormat::RGBA32: bpp = 32; break;
    }

    std::uint32_t row_stride = bmp_stride(w, bpp);
    std::uint32_t pixel_data_size = row_stride * static_cast<std::uint32_t>(h);

    // Palette size.
    std::uint32_t palette_entries = 0;
    if (bpp == 1)  palette_entries = 2;
    if (bpp == 8)  palette_entries = 256;
    std::uint32_t palette_size = palette_entries * 4;  // BGRA entries.

    std::uint32_t header_total = 14 + BITMAPINFOHEADER_SIZE + palette_size;
    std::uint32_t file_size = header_total + pixel_data_size;

    std::vector<std::uint8_t> buf;
    buf.reserve(file_size);

    // DPI.
    double dpi_x = (options.dpi_x > 0) ? options.dpi_x :
                   (image.dpi_x() > 0 ? image.dpi_x() : 96.0);
    double dpi_y = (options.dpi_y > 0) ? options.dpi_y :
                   (image.dpi_y() > 0 ? image.dpi_y() : 96.0);

    // --- File header (14 bytes) ---
    write_u16(buf, BMP_SIGNATURE);
    write_u32(buf, file_size);
    write_u16(buf, 0);  // Reserved1.
    write_u16(buf, 0);  // Reserved2.
    write_u32(buf, header_total);  // Pixel data offset.

    // --- Info header (40 bytes) ---
    write_u32(buf, BITMAPINFOHEADER_SIZE);
    write_i32(buf, w);
    write_i32(buf, h);              // Positive = bottom-up.
    write_u16(buf, 1);              // Planes.
    write_u16(buf, bpp);
    write_u32(buf, BI_RGB);         // Compression.
    write_u32(buf, pixel_data_size);
    write_i32(buf, dpi_to_ppm(dpi_x));
    write_i32(buf, dpi_to_ppm(dpi_y));
    write_u32(buf, palette_entries); // Colors used.
    write_u32(buf, 0);              // Important colors (0 = all).

    // --- Palette ---
    if (bpp == 1) {
        // Entry 0 = white (background), entry 1 = black (foreground).
        // Matches our BW1 convention: bit 0 = white, bit 1 = black.
        buf.push_back(0xFF); buf.push_back(0xFF);
        buf.push_back(0xFF); buf.push_back(0x00);  // White.
        buf.push_back(0x00); buf.push_back(0x00);
        buf.push_back(0x00); buf.push_back(0x00);  // Black.
    } else if (bpp == 8) {
        // Grayscale palette: entry i = (i, i, i, 0).
        for (int i = 0; i < 256; ++i) {
            auto v = static_cast<std::uint8_t>(i);
            buf.push_back(v);    // B.
            buf.push_back(v);    // G.
            buf.push_back(v);    // R.
            buf.push_back(0x00); // Reserved.
        }
    }

    // --- Pixel data (bottom-up) ---
    // Pad row buffer.
    std::vector<std::uint8_t> row_buf(row_stride, 0);

    for (std::int32_t y = h - 1; y >= 0; --y) {
        const auto* src = image.row(y);
        std::fill(row_buf.begin(), row_buf.end(), static_cast<std::uint8_t>(0));

        if (bpp == 1) {
            std::uint32_t copy_bytes = (static_cast<std::uint32_t>(w) + 7) / 8;
            std::memcpy(row_buf.data(), src, copy_bytes);
        } else if (bpp == 8) {
            std::memcpy(row_buf.data(), src, static_cast<std::size_t>(w));
        } else if (bpp == 24) {
            // RGB → BGR.
            for (std::int32_t x = 0; x < w; ++x) {
                row_buf[x * 3 + 0] = src[x * 3 + 2];  // B.
                row_buf[x * 3 + 1] = src[x * 3 + 1];  // G.
                row_buf[x * 3 + 2] = src[x * 3 + 0];  // R.
            }
        } else if (bpp == 32) {
            // RGBA → BGRA.
            for (std::int32_t x = 0; x < w; ++x) {
                row_buf[x * 4 + 0] = src[x * 4 + 2];  // B.
                row_buf[x * 4 + 1] = src[x * 4 + 1];  // G.
                row_buf[x * 4 + 2] = src[x * 4 + 0];  // R.
                row_buf[x * 4 + 3] = src[x * 4 + 3];  // A.
            }
        }

        buf.insert(buf.end(), row_buf.begin(), row_buf.end());
    }

    return buf;
}

bool write_bmp(const Image& image,
               const std::filesystem::path& path,
               const WriteOptions& options) {
    auto buf = write_bmp_to_memory(image, options);
    if (buf.empty()) return false;

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return f.good();
}

} // namespace ppp::core::bmp

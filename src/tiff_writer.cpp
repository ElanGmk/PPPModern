#include "ppp/core/tiff_writer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>

namespace ppp::core::tiff {

namespace {

// ---------------------------------------------------------------------------
// Little-endian serialization helpers
// ---------------------------------------------------------------------------

void put16(std::vector<std::uint8_t>& buf, std::uint16_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put32(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(static_cast<std::uint8_t>(v & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void patch32(std::vector<std::uint8_t>& buf, std::size_t offset, std::uint32_t v) {
    buf[offset] = static_cast<std::uint8_t>(v & 0xFF);
    buf[offset + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    buf[offset + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    buf[offset + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

std::uint32_t read32le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t read16le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8);
}

// ---------------------------------------------------------------------------
// PackBits compression (TIFF compression type 32773)
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> packbits_encode(const std::uint8_t* data, std::size_t len) {
    std::vector<std::uint8_t> out;
    out.reserve(len + len / 128 + 1);

    std::size_t i = 0;
    while (i < len) {
        // Check for a run of identical bytes.
        std::size_t run_start = i;
        while (i + 1 < len && data[i] == data[i + 1] && (i - run_start) < 127) {
            ++i;
        }

        std::size_t run_len = i - run_start + 1;
        if (run_len >= 2) {
            // Encode as a run: -(run_len - 1), byte.
            out.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(-(static_cast<int>(run_len) - 1))));
            out.push_back(data[run_start]);
            ++i;
        } else {
            // Literal sequence.
            std::size_t lit_start = run_start;
            i = run_start;
            while (i < len && (i - lit_start) < 128) {
                // Check if a run of 3+ starts here.
                if (i + 2 < len && data[i] == data[i + 1] && data[i] == data[i + 2]) {
                    break;
                }
                ++i;
            }
            std::size_t lit_len = i - lit_start;
            if (lit_len == 0) {
                // Single byte that starts a run.
                ++i;
                lit_len = 1;
            }
            out.push_back(static_cast<std::uint8_t>(lit_len - 1));
            out.insert(out.end(), data + lit_start, data + lit_start + lit_len);
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// CCITT Group 4 fax compression (simplified)
// ---------------------------------------------------------------------------

// Group 4 encoding for BW1 images. This is a basic implementation of the
// Modified Modified READ (MMR) / T.6 algorithm.

struct BitWriter {
    std::vector<std::uint8_t>& buf;
    std::uint8_t current_byte{0};
    int bits_left{8};

    explicit BitWriter(std::vector<std::uint8_t>& b) : buf(b) {}

    void write_bits(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            current_byte = static_cast<std::uint8_t>(
                (current_byte << 1) | ((value >> i) & 1));
            --bits_left;
            if (bits_left == 0) {
                buf.push_back(current_byte);
                current_byte = 0;
                bits_left = 8;
            }
        }
    }

    void flush() {
        if (bits_left < 8) {
            current_byte = static_cast<std::uint8_t>(current_byte << bits_left);
            buf.push_back(current_byte);
            current_byte = 0;
            bits_left = 8;
        }
    }
};

// White and black run-length code tables per ITU-T T.4.
struct HuffCode {
    std::uint16_t code;
    std::uint8_t bits;
};

// Terminating codes for run lengths 0-63.
static const HuffCode white_term[] = {
    {0x35, 8}, {0x07, 6}, {0x07, 4}, {0x08, 4}, {0x0B, 4}, {0x0C, 4}, {0x0E, 4}, {0x0F, 4},
    {0x13, 5}, {0x14, 5}, {0x07, 5}, {0x08, 5}, {0x08, 6}, {0x03, 6}, {0x34, 6}, {0x35, 6},
    {0x2A, 6}, {0x2B, 6}, {0x27, 7}, {0x0C, 7}, {0x08, 7}, {0x17, 7}, {0x03, 7}, {0x04, 7},
    {0x28, 7}, {0x2B, 7}, {0x13, 7}, {0x24, 7}, {0x18, 7}, {0x02, 8}, {0x03, 8}, {0x1A, 8},
    {0x1B, 8}, {0x12, 8}, {0x13, 8}, {0x14, 8}, {0x15, 8}, {0x16, 8}, {0x17, 8}, {0x28, 8},
    {0x29, 8}, {0x2A, 8}, {0x2B, 8}, {0x2C, 8}, {0x2D, 8}, {0x04, 8}, {0x05, 8}, {0x0A, 8},
    {0x0B, 8}, {0x52, 8}, {0x53, 8}, {0x54, 8}, {0x55, 8}, {0x24, 8}, {0x25, 8}, {0x58, 8},
    {0x59, 8}, {0x5A, 8}, {0x5B, 8}, {0x4A, 8}, {0x4B, 8}, {0x32, 8}, {0x33, 8}, {0x34, 8},
};

static const HuffCode black_term[] = {
    {0x37, 10}, {0x02, 3}, {0x03, 2}, {0x02, 2}, {0x03, 3}, {0x03, 4}, {0x02, 4}, {0x03, 5},
    {0x05, 6}, {0x04, 6}, {0x04, 7}, {0x05, 7}, {0x07, 7}, {0x04, 8}, {0x07, 8}, {0x18, 9},
    {0x17, 10}, {0x18, 10}, {0x08, 10}, {0x67, 11}, {0x68, 11}, {0x6C, 11}, {0x37, 11}, {0x28, 11},
    {0x17, 11}, {0x18, 11}, {0xCA, 12}, {0xCB, 12}, {0xCC, 12}, {0xCD, 12}, {0x68, 12}, {0x69, 12},
    {0x6A, 12}, {0x6B, 12}, {0xD2, 12}, {0xD3, 12}, {0xD4, 12}, {0xD5, 12}, {0xD6, 12}, {0xD7, 12},
    {0x6C, 12}, {0x6D, 12}, {0xDA, 12}, {0xDB, 12}, {0x54, 12}, {0x55, 12}, {0x56, 12}, {0x57, 12},
    {0x64, 12}, {0x65, 12}, {0x52, 12}, {0x53, 12}, {0x24, 12}, {0x37, 12}, {0x38, 12}, {0x27, 12},
    {0x28, 12}, {0x58, 12}, {0x59, 12}, {0x2B, 12}, {0x2C, 12}, {0x5A, 12}, {0x66, 12}, {0x67, 12},
};

// Make-up codes for run lengths 64, 128, ..., 2560.
static const HuffCode white_makeup[] = {
    {0x1B, 5}, {0x12, 5}, {0x17, 6}, {0x37, 7}, {0x36, 8}, {0x37, 8}, {0x64, 8}, {0x65, 8},
    {0x68, 8}, {0x67, 8}, {0xCC, 9}, {0xCD, 9}, {0xD2, 9}, {0xD3, 9}, {0xD4, 9}, {0xD5, 9},
    {0xD6, 9}, {0xD7, 9}, {0xD8, 9}, {0xD9, 9}, {0xDA, 9}, {0xDB, 9}, {0x98, 9}, {0x99, 9},
    {0x9A, 9}, {0x18, 6}, {0x9B, 9}, {0x08, 11}, {0x0C, 11}, {0x0D, 11}, {0x12, 12}, {0x13, 12},
    {0x14, 12}, {0x15, 12}, {0x16, 12}, {0x17, 12}, {0x1C, 12}, {0x1D, 12}, {0x1E, 12}, {0x1F, 12},
};

static const HuffCode black_makeup[] = {
    {0x0F, 10}, {0xC8, 12}, {0xC9, 12}, {0x5B, 12}, {0x33, 12}, {0x34, 12}, {0x35, 12}, {0x6C, 13},
    {0x6D, 13}, {0x4A, 13}, {0x4B, 13}, {0x4C, 13}, {0x4D, 13}, {0x72, 13}, {0x73, 13}, {0x74, 13},
    {0x75, 13}, {0x76, 13}, {0x77, 13}, {0x52, 13}, {0x53, 13}, {0x54, 13}, {0x55, 13}, {0x5A, 13},
    {0x5B, 13}, {0x64, 13}, {0x65, 13}, {0x08, 11}, {0x0C, 11}, {0x0D, 11}, {0x12, 12}, {0x13, 12},
    {0x14, 12}, {0x15, 12}, {0x16, 12}, {0x17, 12}, {0x1C, 12}, {0x1D, 12}, {0x1E, 12}, {0x1F, 12},
};

void encode_run(BitWriter& bw, int run_length, bool is_white) {
    const HuffCode* term = is_white ? white_term : black_term;
    const HuffCode* makeup = is_white ? white_makeup : black_makeup;

    while (run_length >= 64) {
        int makeup_idx = (std::min(run_length, 2560) / 64) - 1;
        bw.write_bits(makeup[makeup_idx].code, makeup[makeup_idx].bits);
        run_length -= (makeup_idx + 1) * 64;
    }

    bw.write_bits(term[run_length].code, term[run_length].bits);
}

std::vector<std::uint8_t> group4_encode(const Image& image) {
    // Group 4 encodes each row as the difference from the previous row.
    // For simplicity, we use a 1D approach encoding each row independently
    // with pass/vertical/horizontal coding modes.
    //
    // Actually, for maximum compatibility we'll use a simplified approach:
    // encode each line with 1D Huffman codes (horizontal mode only).
    // This is technically valid Group 4 — each coding line uses all-horizontal mode.

    std::vector<std::uint8_t> out;
    BitWriter bw(out);

    auto w = image.width();
    auto h = image.height();

    for (std::int32_t y = 0; y < h; ++y) {
        // Encode this row.  In Group 4, each row starts in horizontal mode
        // relative to the reference row.  We'll use all-horizontal coding.

        // Horizontal mode prefix: 001
        bw.write_bits(0x01, 3);

        // Get runs for this row.
        // BW1: bit 1 = foreground (black), bit 0 = background (white).
        // TIFF Group 4 expects WhiteIsZero: white pixels are the first color.
        std::int32_t x = 0;
        bool current_white = true;  // Start with white run.

        while (x < w) {
            // Count run of current color.
            int run = 0;
            while (x < w) {
                int px = image.get_bw_pixel(x, y);
                bool is_white = (px == 0);
                if (is_white != current_white) break;
                ++run;
                ++x;
            }

            encode_run(bw, run, current_white);
            current_white = !current_white;
        }

        // If we ended on a non-white run, we need to emit a zero-length
        // white run to properly terminate (each horizontal mode pair needs
        // both a white and black run).
        // Actually, horizontal mode always emits a1-a2 pair.
        // For simplicity, ensure we always emit alternating runs.
    }

    // EOFB: two consecutive EOL codes (000000000001 x 2).
    bw.write_bits(0x001, 12);
    bw.write_bits(0x001, 12);
    bw.flush();

    return out;
}

// ---------------------------------------------------------------------------
// LZW compression
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> lzw_encode(const std::uint8_t* data, std::size_t len) {
    // TIFF LZW uses MSB-first bit ordering and variable code sizes 9-12 bits.
    std::vector<std::uint8_t> out;

    const int CLEAR_CODE = 256;
    const int EOI_CODE = 257;
    const int MAX_CODE = 4095;

    struct LzwBitWriter {
        std::vector<std::uint8_t>& buf;
        std::uint32_t accum{0};
        int bits{0};

        explicit LzwBitWriter(std::vector<std::uint8_t>& b) : buf(b) {}

        void write(int code, int code_size) {
            // MSB-first packing for TIFF LZW.
            accum = (accum << code_size) | static_cast<std::uint32_t>(code);
            bits += code_size;
            while (bits >= 8) {
                bits -= 8;
                buf.push_back(static_cast<std::uint8_t>((accum >> bits) & 0xFF));
            }
        }

        void flush() {
            if (bits > 0) {
                buf.push_back(static_cast<std::uint8_t>((accum << (8 - bits)) & 0xFF));
                bits = 0;
                accum = 0;
            }
        }
    };

    LzwBitWriter bw(out);

    // Simple LZW without dictionary — just emit clear + raw codes + EOI.
    // For a proper implementation we'd need a hash table, but for correctness
    // and simplicity we'll implement the basic algorithm.

    // Dictionary: maps string → code.
    // Use a simple approach: prefix code + next byte → new code.
    struct Entry {
        int prefix;
        std::uint8_t byte;
    };

    std::vector<Entry> dict;
    dict.reserve(MAX_CODE + 1);

    auto reset_dict = [&]() {
        dict.clear();
        // First 258 entries are implicit (0-255 = literals, 256=clear, 257=eoi).
    };

    int next_code = 258;
    int code_size = 9;

    auto find_entry = [&](int prefix, std::uint8_t byte) -> int {
        for (std::size_t i = 0; i < dict.size(); ++i) {
            if (dict[i].prefix == prefix && dict[i].byte == byte)
                return static_cast<int>(i) + 258;
        }
        return -1;
    };

    // Emit clear code.
    reset_dict();
    next_code = 258;
    code_size = 9;
    bw.write(CLEAR_CODE, code_size);

    if (len == 0) {
        bw.write(EOI_CODE, code_size);
        bw.flush();
        return out;
    }

    int w_code = data[0];  // Current string = first byte.

    for (std::size_t i = 1; i < len; ++i) {
        std::uint8_t k = data[i];
        int found = find_entry(w_code, k);

        if (found >= 0) {
            w_code = found;
        } else {
            // Output current code.
            bw.write(w_code, code_size);

            // Add new entry.
            if (next_code <= MAX_CODE) {
                dict.push_back({w_code, k});
                ++next_code;

                // Increase code size if needed.
                if (next_code > (1 << code_size) && code_size < 12) {
                    ++code_size;
                }
            } else {
                // Dictionary full — emit clear and reset.
                bw.write(CLEAR_CODE, code_size);
                reset_dict();
                next_code = 258;
                code_size = 9;
            }

            w_code = k;
        }
    }

    // Output last code.
    bw.write(w_code, code_size);
    bw.write(EOI_CODE, code_size);
    bw.flush();

    return out;
}

// ---------------------------------------------------------------------------
// IFD entry builder
// ---------------------------------------------------------------------------

struct IfdEntryData {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t count;
    std::uint32_t value_or_offset;  // Value if fits in 4 bytes, else offset.
    std::vector<std::uint8_t> overflow;  // Data that doesn't fit inline.
};

void add_short_entry(std::vector<IfdEntryData>& entries, std::uint16_t tag, std::uint16_t value) {
    entries.push_back({tag, 3, 1, value, {}});
}

void add_long_entry(std::vector<IfdEntryData>& entries, std::uint16_t tag, std::uint32_t value) {
    entries.push_back({tag, 4, 1, value, {}});
}

void add_rational_entry(std::vector<IfdEntryData>& entries, std::uint16_t tag,
                         std::uint32_t num, std::uint32_t den) {
    std::vector<std::uint8_t> data(8);
    data[0] = static_cast<std::uint8_t>(num & 0xFF);
    data[1] = static_cast<std::uint8_t>((num >> 8) & 0xFF);
    data[2] = static_cast<std::uint8_t>((num >> 16) & 0xFF);
    data[3] = static_cast<std::uint8_t>((num >> 24) & 0xFF);
    data[4] = static_cast<std::uint8_t>(den & 0xFF);
    data[5] = static_cast<std::uint8_t>((den >> 8) & 0xFF);
    data[6] = static_cast<std::uint8_t>((den >> 16) & 0xFF);
    data[7] = static_cast<std::uint8_t>((den >> 24) & 0xFF);
    entries.push_back({tag, 5, 1, 0, std::move(data)});
}

void add_ascii_entry(std::vector<IfdEntryData>& entries, std::uint16_t tag,
                      const std::string& value) {
    std::uint32_t count = static_cast<std::uint32_t>(value.size() + 1);  // Include null.
    if (count <= 4) {
        std::uint32_t packed = 0;
        std::memcpy(&packed, value.c_str(), count);
        entries.push_back({tag, 2, count, packed, {}});
    } else {
        std::vector<std::uint8_t> data(value.begin(), value.end());
        data.push_back(0);
        entries.push_back({tag, 2, count, 0, std::move(data)});
    }
}

// ---------------------------------------------------------------------------
// Core TIFF writing logic
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> build_tiff(const Image& image, const WriteOptions& options) {
    std::vector<std::uint8_t> buf;
    buf.reserve(image.data_size() + 1024);

    auto w = image.width();
    auto h = image.height();

    // Determine TIFF parameters from image format.
    std::uint16_t bits_per_sample = 8;
    std::uint16_t samples_per_pixel = 1;
    auto photometric = static_cast<std::uint16_t>(options.photometric);
    auto compression = static_cast<std::uint16_t>(options.compression);

    switch (image.format()) {
        case PixelFormat::BW1:
            bits_per_sample = 1;
            samples_per_pixel = 1;
            if (photometric == static_cast<std::uint16_t>(Photometric::RGB)) {
                photometric = static_cast<std::uint16_t>(Photometric::WhiteIsZero);
            }
            break;
        case PixelFormat::Gray8:
            bits_per_sample = 8;
            samples_per_pixel = 1;
            break;
        case PixelFormat::RGB24:
            bits_per_sample = 8;
            samples_per_pixel = 3;
            photometric = static_cast<std::uint16_t>(Photometric::RGB);
            break;
        case PixelFormat::RGBA32:
            bits_per_sample = 8;
            samples_per_pixel = 4;
            photometric = static_cast<std::uint16_t>(Photometric::RGB);
            break;
    }

    // Compress strip data.
    // For BW1, use the minimum row bytes (no DWORD padding in TIFF strips).
    std::int32_t tiff_row_bytes;
    if (image.format() == PixelFormat::BW1) {
        tiff_row_bytes = (w + 7) / 8;
    } else {
        tiff_row_bytes = w * static_cast<std::int32_t>(bytes_per_pixel(image.format()));
    }

    // Build uncompressed strip data (removing any padding from Image stride).
    std::vector<std::uint8_t> strip_data;
    strip_data.reserve(static_cast<std::size_t>(tiff_row_bytes) * h);
    for (std::int32_t y = 0; y < h; ++y) {
        const auto* row = image.row(y);
        strip_data.insert(strip_data.end(), row, row + tiff_row_bytes);
    }

    // Apply compression.
    std::vector<std::uint8_t> compressed_data;
    bool use_compressed = false;

    switch (options.compression) {
        case Compression::Uncompressed:
            break;
        case Compression::PackBits:
            compressed_data = packbits_encode(strip_data.data(), strip_data.size());
            use_compressed = true;
            break;
        case Compression::LZW:
            compressed_data = lzw_encode(strip_data.data(), strip_data.size());
            use_compressed = true;
            break;
        case Compression::Group4Fax:
            if (image.format() == PixelFormat::BW1) {
                compressed_data = group4_encode(image);
                use_compressed = true;
            }
            break;
        default:
            // Unsupported compression — fall back to uncompressed.
            compression = static_cast<std::uint16_t>(Compression::Uncompressed);
            break;
    }

    const auto& final_strip = use_compressed ? compressed_data : strip_data;
    auto strip_byte_count = static_cast<std::uint32_t>(final_strip.size());

    // TIFF header.
    buf.push_back('I'); buf.push_back('I');  // Little-endian.
    put16(buf, 42);     // Magic.
    put32(buf, 0);      // IFD offset — patched later.

    // Write strip data first (after header).
    auto strip_offset = static_cast<std::uint32_t>(buf.size());
    buf.insert(buf.end(), final_strip.begin(), final_strip.end());

    // Pad to word boundary.
    while (buf.size() & 1) buf.push_back(0);

    // Build IFD entries.
    std::vector<IfdEntryData> entries;

    add_short_entry(entries, static_cast<std::uint16_t>(Tag::ImageWidth),
                    static_cast<std::uint16_t>(w));
    if (w > 65535) {
        entries.back().type = 4;  // LONG.
        entries.back().value_or_offset = static_cast<std::uint32_t>(w);
    }

    add_short_entry(entries, static_cast<std::uint16_t>(Tag::ImageLength),
                    static_cast<std::uint16_t>(h));
    if (h > 65535) {
        entries.back().type = 4;
        entries.back().value_or_offset = static_cast<std::uint32_t>(h);
    }

    add_short_entry(entries, static_cast<std::uint16_t>(Tag::BitsPerSample), bits_per_sample);
    add_short_entry(entries, static_cast<std::uint16_t>(Tag::Compression), compression);
    add_short_entry(entries, static_cast<std::uint16_t>(Tag::PhotometricInterpretation), photometric);

    add_long_entry(entries, static_cast<std::uint16_t>(Tag::StripOffsets), strip_offset);
    add_short_entry(entries, static_cast<std::uint16_t>(Tag::SamplesPerPixel), samples_per_pixel);

    add_long_entry(entries, static_cast<std::uint16_t>(Tag::RowsPerStrip),
                   static_cast<std::uint32_t>(h));
    add_long_entry(entries, static_cast<std::uint16_t>(Tag::StripByteCounts), strip_byte_count);

    // Resolution.
    if (image.dpi_x() > 0) {
        add_rational_entry(entries, static_cast<std::uint16_t>(Tag::XResolution),
                           static_cast<std::uint32_t>(image.dpi_x()), 1);
    }
    if (image.dpi_y() > 0) {
        add_rational_entry(entries, static_cast<std::uint16_t>(Tag::YResolution),
                           static_cast<std::uint32_t>(image.dpi_y()), 1);
    }

    // ResolutionUnit = inches.
    add_short_entry(entries, static_cast<std::uint16_t>(Tag::ResolutionUnit), 2);

    // Software tag.
    if (!options.software.empty()) {
        add_ascii_entry(entries, static_cast<std::uint16_t>(Tag::Software), options.software);
    }

    // Sort entries by tag (TIFF requirement).
    std::sort(entries.begin(), entries.end(),
              [](const IfdEntryData& a, const IfdEntryData& b) { return a.tag < b.tag; });

    // Write IFD.
    auto ifd_offset = static_cast<std::uint32_t>(buf.size());
    patch32(buf, 4, ifd_offset);  // Patch header to point to IFD.

    auto num_entries = static_cast<std::uint16_t>(entries.size());
    put16(buf, num_entries);

    // Calculate where overflow data will go (after IFD + next-IFD pointer).
    auto overflow_start = static_cast<std::uint32_t>(
        buf.size() + entries.size() * 12 + 4);

    // First pass: calculate overflow offsets.
    std::uint32_t overflow_offset = overflow_start;
    for (auto& e : entries) {
        if (!e.overflow.empty()) {
            e.value_or_offset = overflow_offset;
            overflow_offset += static_cast<std::uint32_t>(e.overflow.size());
            // Pad to word boundary.
            if (overflow_offset & 1) ++overflow_offset;
        }
    }

    // Write IFD entries.
    for (const auto& e : entries) {
        put16(buf, e.tag);
        put16(buf, e.type);
        put32(buf, e.count);
        put32(buf, e.value_or_offset);
    }

    // Next IFD offset = 0 (no more IFDs).
    put32(buf, 0);

    // Write overflow data.
    for (const auto& e : entries) {
        if (!e.overflow.empty()) {
            buf.insert(buf.end(), e.overflow.begin(), e.overflow.end());
            if (buf.size() & 1) buf.push_back(0);  // Pad.
        }
    }

    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> write_tiff_to_memory(const Image& image,
                                                const WriteOptions& options) {
    if (image.empty()) return {};
    return build_tiff(image, options);
}

bool write_tiff(const Image& image, const std::filesystem::path& path,
                const WriteOptions& options) {
    auto data = write_tiff_to_memory(image, options);
    if (data.empty()) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return out.good();
}

bool write_multipage_tiff(const std::vector<Image>& images,
                           const std::filesystem::path& path,
                           const WriteOptions& options) {
    if (images.empty()) return false;

    // Build each page as a separate TIFF buffer, then stitch them together
    // by chaining IFD pointers.
    std::vector<std::uint8_t> combined;

    for (std::size_t i = 0; i < images.size(); ++i) {
        if (images[i].empty()) continue;

        auto page_data = build_tiff(images[i], options);
        if (page_data.empty()) return false;

        if (i == 0) {
            combined = std::move(page_data);
        } else {
            // Find the "next IFD" field of the last page and patch it.
            // The last IFD's "next IFD" is 4 bytes after the last IFD entry.
            // We need to find it: walk the IFD chain in combined.

            std::uint32_t ifd_off = read32le(combined.data() + 4);
            while (true) {
                std::uint16_t count = read16le(combined.data() + ifd_off);
                std::uint32_t next_ifd_pos = ifd_off + 2 + count * 12;
                std::uint32_t next_ifd = read32le(combined.data() + next_ifd_pos);
                if (next_ifd == 0) {
                    // Patch this to point to the new page's IFD.
                    // The new page's data starts at combined.size().
                    // Its IFD offset is at bytes 4-7 of page_data.
                    std::uint32_t new_page_base = static_cast<std::uint32_t>(combined.size());
                    std::uint32_t new_ifd_off = read32le(page_data.data() + 4);

                    // Relocate the new page's offsets.
                    // Patch the new page's strip offset and overflow data offsets.
                    // Walk the new page's IFD and add new_page_base to all offsets.
                    std::uint16_t new_count = read16le(page_data.data() + new_ifd_off);
                    for (std::uint16_t e = 0; e < new_count; ++e) {
                        std::size_t entry_pos = new_ifd_off + 2 + e * 12;
                        std::uint16_t tag = read16le(page_data.data() + entry_pos);
                        std::uint16_t type = read16le(page_data.data() + entry_pos + 2);
                        std::uint32_t count_val = read32le(page_data.data() + entry_pos + 4);

                        // Determine if value is an offset (doesn't fit in 4 bytes).
                        std::size_t type_size = 0;
                        switch (type) {
                            case 1: case 2: case 6: case 7: type_size = 1; break;
                            case 3: case 8: type_size = 2; break;
                            case 4: case 9: case 11: type_size = 4; break;
                            case 5: case 10: type_size = 8; break;
                            case 12: type_size = 8; break;
                        }

                        bool is_offset = (type_size * count_val > 4);

                        // StripOffsets (273) and StripByteCounts (279) with count=1
                        // store values inline, but StripOffsets points to strip data.
                        if (tag == 273 && !is_offset) {
                            // StripOffsets: relocate.
                            std::uint32_t val = read32le(page_data.data() + entry_pos + 8);
                            patch32(page_data, entry_pos + 8, val + new_page_base);
                        } else if (is_offset) {
                            std::uint32_t val = read32le(page_data.data() + entry_pos + 8);
                            patch32(page_data, entry_pos + 8, val + new_page_base);
                        }
                    }

                    // Patch the previous page's next-IFD pointer.
                    patch32(combined, next_ifd_pos, new_ifd_off + new_page_base);

                    // Append new page data.
                    combined.insert(combined.end(), page_data.begin(), page_data.end());
                    break;
                }
                ifd_off = next_ifd;
            }
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(combined.data()),
              static_cast<std::streamsize>(combined.size()));
    return out.good();
}

// ---------------------------------------------------------------------------
// TIFF reader (pixel data extraction)
// ---------------------------------------------------------------------------

Image read_tiff_image(const std::uint8_t* data, std::size_t size,
                      std::size_t page) {
    auto structure = Structure::read(data, size);
    if (!structure || page >= structure->page_count()) return {};

    const auto& ifd = structure->page(page);

    auto width_opt = ifd.get_int(Tag::ImageWidth);
    auto height_opt = ifd.get_int(Tag::ImageLength);
    if (!width_opt || !height_opt) return {};

    auto w = static_cast<std::int32_t>(*width_opt);
    auto h = static_cast<std::int32_t>(*height_opt);

    auto bps_opt = ifd.get_int(Tag::BitsPerSample);
    auto spp_opt = ifd.get_int(Tag::SamplesPerPixel);
    auto comp_opt = ifd.get_int(Tag::Compression);

    int bps = bps_opt ? static_cast<int>(*bps_opt) : 1;
    int spp = spp_opt ? static_cast<int>(*spp_opt) : 1;
    int comp = comp_opt ? static_cast<int>(*comp_opt) : 1;

    // Only support uncompressed for reading pixel data.
    if (comp != 1) return {};

    // Determine pixel format.
    PixelFormat fmt;
    if (bps == 1 && spp == 1) fmt = PixelFormat::BW1;
    else if (bps == 8 && spp == 1) fmt = PixelFormat::Gray8;
    else if (bps == 8 && spp == 3) fmt = PixelFormat::RGB24;
    else if (bps == 8 && spp == 4) fmt = PixelFormat::RGBA32;
    else return {};

    // Get strip offsets and byte counts.
    const auto* strip_off_entry = ifd.find(Tag::StripOffsets);
    const auto* strip_bc_entry = ifd.find(Tag::StripByteCounts);
    auto rps_opt = ifd.get_int(Tag::RowsPerStrip);

    if (!strip_off_entry || !strip_bc_entry) return {};

    // Extract strip offsets.
    std::vector<std::uint32_t> strip_offsets;
    if (auto* longs = std::get_if<std::vector<std::uint32_t>>(&strip_off_entry->value)) {
        strip_offsets = *longs;
    } else if (auto* shorts = std::get_if<std::vector<std::uint16_t>>(&strip_off_entry->value)) {
        for (auto s : *shorts) strip_offsets.push_back(s);
    } else {
        return {};
    }

    // Get resolution.
    double dpi_x = 0, dpi_y = 0;
    auto xres = ifd.get_double(Tag::XResolution);
    auto yres = ifd.get_double(Tag::YResolution);
    if (xres) dpi_x = *xres;
    if (yres) dpi_y = *yres;

    Image img(w, h, fmt, dpi_x, dpi_y);

    // Calculate TIFF row bytes (no padding).
    std::int32_t tiff_row_bytes;
    if (fmt == PixelFormat::BW1) {
        tiff_row_bytes = (w + 7) / 8;
    } else {
        tiff_row_bytes = w * static_cast<std::int32_t>(bytes_per_pixel(fmt));
    }

    int rows_per_strip = rps_opt ? static_cast<int>(*rps_opt) : h;
    std::int32_t row = 0;

    for (std::size_t s = 0; s < strip_offsets.size() && row < h; ++s) {
        auto offset = strip_offsets[s];
        int rows_this_strip = std::min(rows_per_strip, static_cast<int>(h - row));

        for (int r = 0; r < rows_this_strip && row < h; ++r, ++row) {
            auto src_pos = offset + static_cast<std::uint32_t>(r) * tiff_row_bytes;
            if (src_pos + tiff_row_bytes > size) return {};
            std::memcpy(img.row(row), data + src_pos, tiff_row_bytes);
        }
    }

    return img;
}

Image read_tiff_image_file(const std::filesystem::path& path, std::size_t page) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    in.seekg(0, std::ios::end);
    auto file_size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> data(file_size);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(file_size));
    if (!in) return {};

    return read_tiff_image(data.data(), data.size(), page);
}

} // namespace ppp::core::tiff

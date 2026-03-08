#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ppp::core::tiff {

// ---------------------------------------------------------------------------
// TIFF field types (wire format)
// ---------------------------------------------------------------------------

enum class FieldType : std::uint16_t {
    Byte = 1,
    Ascii = 2,
    Short = 3,
    Long = 4,
    Rational = 5,
    SByte = 6,
    Undefined = 7,
    SShort = 8,
    SLong = 9,
    SRational = 10,
    Float = 11,
    Double = 12
};

[[nodiscard]] std::string_view to_string(FieldType type) noexcept;

// ---------------------------------------------------------------------------
// Standard TIFF tags
// ---------------------------------------------------------------------------

enum class Tag : std::uint16_t {
    NewSubfileType = 254,
    SubfileType = 255,
    ImageWidth = 256,
    ImageLength = 257,
    BitsPerSample = 258,
    Compression = 259,
    PhotometricInterpretation = 262,
    Threshholding = 263,
    CellWidth = 264,
    CellLength = 265,
    FillOrder = 266,
    DocumentName = 269,
    ImageDescription = 270,
    Make = 271,
    Model = 272,
    StripOffsets = 273,
    Orientation = 274,
    SamplesPerPixel = 277,
    RowsPerStrip = 278,
    StripByteCounts = 279,
    MinSampleValue = 280,
    MaxSampleValue = 281,
    XResolution = 282,
    YResolution = 283,
    PlanarConfiguration = 284,
    PageName = 285,
    XPosition = 286,
    YPosition = 287,
    FreeOffsets = 288,
    FreeByteCounts = 289,
    GrayResponseUnit = 290,
    GrayResponseCurve = 291,
    T4Options = 292,
    T6Options = 293,
    ResolutionUnit = 296,
    PageNumber = 297,
    Software = 305,
    DateTime = 306,
    Artist = 315,
    HostComputer = 316,
    Predictor = 317,
    WhitePoint = 318,
    PrimaryChromaticities = 319,
    ColorMap = 320,
    HalftoneHints = 321,
    TileWidth = 322,
    TileLength = 323,
    TileOffsets = 324,
    TileByteCounts = 325,
    InkSet = 332,
    InkNames = 333,
    NumberOfInks = 334,
    DotRange = 336,
    TargetPrinter = 337,
    ExtraSamples = 338,
    SampleFormat = 339,
    TransferRange = 342,
    JPEGProc = 512,
    JPEGInterchangeFormat = 513,
    JPEGInterchangeFormatLength = 514,
    YCbCrCoefficients = 529,
    YCbCrSubSampling = 530,
    YCbCrPositioning = 531,
    ReferenceBlackWhite = 532,
    Copyright = 33432
};

// ---------------------------------------------------------------------------
// Compression types
// ---------------------------------------------------------------------------

enum class Compression : std::uint16_t {
    Uncompressed = 1,
    CCITT_1D = 2,
    Group3Fax = 3,
    Group4Fax = 4,
    LZW = 5,
    JPEG_Old = 6,
    JPEG = 7,
    Deflate = 8,
    PackBits = 32773
};

// ---------------------------------------------------------------------------
// Photometric interpretation
// ---------------------------------------------------------------------------

enum class Photometric : std::uint16_t {
    WhiteIsZero = 0,
    BlackIsZero = 1,
    RGB = 2,
    Palette = 3,
    TransparencyMask = 4,
    CMYK = 5,
    YCbCr = 6,
    CIELab = 8
};

// ---------------------------------------------------------------------------
// Rational number (two 32-bit values)
// ---------------------------------------------------------------------------

struct Rational {
    std::uint32_t numerator{0};
    std::uint32_t denominator{1};

    [[nodiscard]] double to_double() const noexcept {
        return denominator != 0 ? static_cast<double>(numerator) / denominator : 0.0;
    }
};

struct SRational {
    std::int32_t numerator{0};
    std::int32_t denominator{1};

    [[nodiscard]] double to_double() const noexcept {
        return denominator != 0 ? static_cast<double>(numerator) / denominator : 0.0;
    }
};

// ---------------------------------------------------------------------------
// Field value — tagged union for any TIFF field
// ---------------------------------------------------------------------------

using FieldValue = std::variant<
    std::vector<std::uint8_t>,     // Byte / Undefined
    std::string,                    // Ascii
    std::vector<std::uint16_t>,    // Short
    std::vector<std::uint32_t>,    // Long
    std::vector<Rational>,         // Rational
    std::vector<std::int8_t>,      // SByte
    std::vector<std::int16_t>,     // SShort
    std::vector<std::int32_t>,     // SLong
    std::vector<SRational>,        // SRational
    std::vector<float>,            // Float
    std::vector<double>            // Double
>;

// ---------------------------------------------------------------------------
// IFD entry (tag + type + value)
// ---------------------------------------------------------------------------

struct IfdEntry {
    FieldType type{FieldType::Byte};
    FieldValue value;

    [[nodiscard]] std::optional<std::int64_t> to_int() const noexcept;
    [[nodiscard]] std::optional<double> to_double() const noexcept;
    [[nodiscard]] std::optional<std::string> to_string() const;
};

// ---------------------------------------------------------------------------
// IFD — one image directory (one per TIFF page)
// ---------------------------------------------------------------------------

class Ifd {
public:
    using Map = std::map<std::uint16_t, IfdEntry>;

    void set(Tag tag, IfdEntry entry);
    void set(std::uint16_t tag, IfdEntry entry);

    [[nodiscard]] const IfdEntry* find(Tag tag) const noexcept;
    [[nodiscard]] const IfdEntry* find(std::uint16_t tag) const noexcept;

    [[nodiscard]] std::optional<std::int64_t> get_int(Tag tag) const noexcept;
    [[nodiscard]] std::optional<double> get_double(Tag tag) const noexcept;
    [[nodiscard]] std::optional<std::string> get_string(Tag tag) const;

    [[nodiscard]] const Map& entries() const noexcept { return entries_; }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    Map entries_;
};

// ---------------------------------------------------------------------------
// TIFF structure — parsed TIFF file
// ---------------------------------------------------------------------------

enum class ByteOrder : std::uint8_t {
    LittleEndian = 0,
    BigEndian = 1
};

class Structure {
public:
    [[nodiscard]] static std::optional<Structure> read(const std::uint8_t* data, std::size_t size);
    [[nodiscard]] static std::optional<Structure> read(const std::vector<std::uint8_t>& data);

    [[nodiscard]] ByteOrder byte_order() const noexcept { return byte_order_; }
    [[nodiscard]] std::size_t page_count() const noexcept { return ifds_.size(); }
    [[nodiscard]] const Ifd& page(std::size_t index) const { return ifds_.at(index); }
    [[nodiscard]] const std::vector<Ifd>& pages() const noexcept { return ifds_; }

    // Convenience accessors for common tags (first page)
    [[nodiscard]] std::optional<std::int64_t> image_width() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> image_length() const noexcept;
    [[nodiscard]] std::optional<double> x_resolution() const noexcept;
    [[nodiscard]] std::optional<double> y_resolution() const noexcept;
    [[nodiscard]] std::optional<Compression> compression() const noexcept;
    [[nodiscard]] std::optional<Photometric> photometric() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> orientation() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> bits_per_sample() const noexcept;

private:
    ByteOrder byte_order_{ByteOrder::LittleEndian};
    std::vector<Ifd> ifds_;
};

// ---------------------------------------------------------------------------
// File I/O helpers
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<Structure> read_tiff_file(const std::string& path);

} // namespace ppp::core::tiff

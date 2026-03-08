#include "ppp/core/tiff.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace ppp::core::tiff {

// ---------------------------------------------------------------------------
// FieldType to_string
// ---------------------------------------------------------------------------

std::string_view to_string(FieldType type) noexcept {
    switch (type) {
    case FieldType::Byte: return "byte";
    case FieldType::Ascii: return "ascii";
    case FieldType::Short: return "short";
    case FieldType::Long: return "long";
    case FieldType::Rational: return "rational";
    case FieldType::SByte: return "sbyte";
    case FieldType::Undefined: return "undefined";
    case FieldType::SShort: return "sshort";
    case FieldType::SLong: return "slong";
    case FieldType::SRational: return "srational";
    case FieldType::Float: return "float";
    case FieldType::Double: return "double";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Byte-swap helpers
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* p, bool big_endian) noexcept {
    if (big_endian) {
        return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
    }
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* p, bool big_endian) noexcept {
    if (big_endian) {
        return (static_cast<std::uint32_t>(p[0]) << 24) |
               (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) |
               static_cast<std::uint32_t>(p[3]);
    }
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

[[nodiscard]] std::int16_t read_s16(const std::uint8_t* p, bool be) noexcept {
    return static_cast<std::int16_t>(read_u16(p, be));
}

[[nodiscard]] std::int32_t read_s32(const std::uint8_t* p, bool be) noexcept {
    return static_cast<std::int32_t>(read_u32(p, be));
}

[[nodiscard]] float read_f32(const std::uint8_t* p, bool be) noexcept {
    std::uint32_t bits = read_u32(p, be);
    float result;
    std::memcpy(&result, &bits, sizeof(float));
    return result;
}

[[nodiscard]] double read_f64(const std::uint8_t* p, bool be) noexcept {
    std::uint64_t bits;
    if (be) {
        bits = (static_cast<std::uint64_t>(read_u32(p, true)) << 32) |
               static_cast<std::uint64_t>(read_u32(p + 4, true));
    } else {
        bits = static_cast<std::uint64_t>(read_u32(p, false)) |
               (static_cast<std::uint64_t>(read_u32(p + 4, false)) << 32);
    }
    double result;
    std::memcpy(&result, &bits, sizeof(double));
    return result;
}

// Size of a single element for each TIFF field type
[[nodiscard]] std::size_t element_size(FieldType type) noexcept {
    switch (type) {
    case FieldType::Byte:
    case FieldType::SByte:
    case FieldType::Undefined:
    case FieldType::Ascii:
        return 1;
    case FieldType::Short:
    case FieldType::SShort:
        return 2;
    case FieldType::Long:
    case FieldType::SLong:
    case FieldType::Float:
        return 4;
    case FieldType::Rational:
    case FieldType::SRational:
    case FieldType::Double:
        return 8;
    }
    return 0;
}

// Read a field value from raw bytes
[[nodiscard]] std::optional<FieldValue> read_field_value(
    FieldType type, std::uint32_t count,
    const std::uint8_t* data, std::size_t data_size,
    bool be) {

    switch (type) {
    case FieldType::Byte:
    case FieldType::Undefined: {
        if (count > data_size) return std::nullopt;
        return FieldValue{std::vector<std::uint8_t>(data, data + count)};
    }
    case FieldType::Ascii: {
        if (count > data_size) return std::nullopt;
        std::size_t len = count;
        while (len > 0 && data[len - 1] == '\0') --len;
        return FieldValue{std::string(reinterpret_cast<const char*>(data), len)};
    }
    case FieldType::Short: {
        if (count * 2 > data_size) return std::nullopt;
        std::vector<std::uint16_t> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = read_u16(data + i * 2, be);
        return FieldValue{std::move(values)};
    }
    case FieldType::Long: {
        if (count * 4 > data_size) return std::nullopt;
        std::vector<std::uint32_t> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = read_u32(data + i * 4, be);
        return FieldValue{std::move(values)};
    }
    case FieldType::Rational: {
        if (count * 8 > data_size) return std::nullopt;
        std::vector<Rational> values(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            values[i].numerator = read_u32(data + i * 8, be);
            values[i].denominator = read_u32(data + i * 8 + 4, be);
        }
        return FieldValue{std::move(values)};
    }
    case FieldType::SByte: {
        if (count > data_size) return std::nullopt;
        std::vector<std::int8_t> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = static_cast<std::int8_t>(data[i]);
        return FieldValue{std::move(values)};
    }
    case FieldType::SShort: {
        if (count * 2 > data_size) return std::nullopt;
        std::vector<std::int16_t> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = read_s16(data + i * 2, be);
        return FieldValue{std::move(values)};
    }
    case FieldType::SLong: {
        if (count * 4 > data_size) return std::nullopt;
        std::vector<std::int32_t> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = read_s32(data + i * 4, be);
        return FieldValue{std::move(values)};
    }
    case FieldType::SRational: {
        if (count * 8 > data_size) return std::nullopt;
        std::vector<SRational> values(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            values[i].numerator = read_s32(data + i * 8, be);
            values[i].denominator = read_s32(data + i * 8 + 4, be);
        }
        return FieldValue{std::move(values)};
    }
    case FieldType::Float: {
        if (count * 4 > data_size) return std::nullopt;
        std::vector<float> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = read_f32(data + i * 4, be);
        return FieldValue{std::move(values)};
    }
    case FieldType::Double: {
        if (count * 8 > data_size) return std::nullopt;
        std::vector<double> values(count);
        for (std::uint32_t i = 0; i < count; ++i)
            values[i] = read_f64(data + i * 8, be);
        return FieldValue{std::move(values)};
    }
    }
    return std::nullopt;
}

} // namespace

// ---------------------------------------------------------------------------
// IfdEntry
// ---------------------------------------------------------------------------

std::optional<std::int64_t> IfdEntry::to_int() const noexcept {
    return std::visit([](const auto& v) -> std::optional<std::int64_t> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::vector<Rational>>) {
            if (v.empty()) return std::nullopt;
            return static_cast<std::int64_t>(v[0].to_double());
        } else if constexpr (std::is_same_v<T, std::vector<SRational>>) {
            if (v.empty()) return std::nullopt;
            return static_cast<std::int64_t>(v[0].to_double());
        } else if constexpr (std::is_same_v<T, std::vector<float>>) {
            if (v.empty()) return std::nullopt;
            return static_cast<std::int64_t>(v[0]);
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            if (v.empty()) return std::nullopt;
            return static_cast<std::int64_t>(v[0]);
        } else {
            if (v.empty()) return std::nullopt;
            return static_cast<std::int64_t>(v[0]);
        }
    }, value);
}

std::optional<double> IfdEntry::to_double() const noexcept {
    return std::visit([](const auto& v) -> std::optional<double> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return std::nullopt;
        } else if constexpr (std::is_same_v<T, std::vector<Rational>>) {
            if (v.empty()) return std::nullopt;
            return v[0].to_double();
        } else if constexpr (std::is_same_v<T, std::vector<SRational>>) {
            if (v.empty()) return std::nullopt;
            return v[0].to_double();
        } else {
            if (v.empty()) return std::nullopt;
            return static_cast<double>(v[0]);
        }
    }, value);
}

std::optional<std::string> IfdEntry::to_string() const {
    return std::visit([](const auto& v) -> std::optional<std::string> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
            return v;
        } else if constexpr (std::is_same_v<T, std::vector<Rational>>) {
            if (v.empty()) return std::nullopt;
            return std::to_string(v[0].numerator) + "/" + std::to_string(v[0].denominator);
        } else if constexpr (std::is_same_v<T, std::vector<SRational>>) {
            if (v.empty()) return std::nullopt;
            return std::to_string(v[0].numerator) + "/" + std::to_string(v[0].denominator);
        } else if constexpr (std::is_same_v<T, std::vector<float>> ||
                             std::is_same_v<T, std::vector<double>>) {
            if (v.empty()) return std::nullopt;
            return std::to_string(v[0]);
        } else {
            if (v.empty()) return std::nullopt;
            return std::to_string(static_cast<std::int64_t>(v[0]));
        }
    }, value);
}

// ---------------------------------------------------------------------------
// Ifd
// ---------------------------------------------------------------------------

void Ifd::set(Tag tag, IfdEntry entry) {
    set(static_cast<std::uint16_t>(tag), std::move(entry));
}

void Ifd::set(std::uint16_t tag, IfdEntry entry) {
    entries_[tag] = std::move(entry);
}

const IfdEntry* Ifd::find(Tag tag) const noexcept {
    return find(static_cast<std::uint16_t>(tag));
}

const IfdEntry* Ifd::find(std::uint16_t tag) const noexcept {
    auto it = entries_.find(tag);
    return it != entries_.end() ? &it->second : nullptr;
}

std::optional<std::int64_t> Ifd::get_int(Tag tag) const noexcept {
    const auto* entry = find(tag);
    return entry ? entry->to_int() : std::nullopt;
}

std::optional<double> Ifd::get_double(Tag tag) const noexcept {
    const auto* entry = find(tag);
    return entry ? entry->to_double() : std::nullopt;
}

std::optional<std::string> Ifd::get_string(Tag tag) const {
    const auto* entry = find(tag);
    return entry ? entry->to_string() : std::nullopt;
}

// ---------------------------------------------------------------------------
// Structure — TIFF parser
// ---------------------------------------------------------------------------

std::optional<Structure> Structure::read(const std::vector<std::uint8_t>& data) {
    return read(data.data(), data.size());
}

std::optional<Structure> Structure::read(const std::uint8_t* data, std::size_t size) {
    if (size < 8) return std::nullopt;

    // Parse header
    bool be;
    if (data[0] == 'I' && data[1] == 'I') {
        be = false;
    } else if (data[0] == 'M' && data[1] == 'M') {
        be = true;
    } else {
        return std::nullopt;
    }

    std::uint16_t magic = read_u16(data + 2, be);
    if (magic != 42) return std::nullopt;

    Structure structure;
    structure.byte_order_ = be ? ByteOrder::BigEndian : ByteOrder::LittleEndian;

    std::uint32_t ifd_offset = read_u32(data + 4, be);

    // Guard against infinite loops
    constexpr std::size_t max_ifds = 10000;

    while (ifd_offset != 0 && structure.ifds_.size() < max_ifds) {
        if (ifd_offset + 2 > size) return std::nullopt;

        std::uint16_t entry_count = read_u16(data + ifd_offset, be);
        std::size_t entries_size = static_cast<std::size_t>(entry_count) * 12;
        if (ifd_offset + 2 + entries_size + 4 > size) return std::nullopt;

        Ifd ifd;
        const std::uint8_t* entries_data = data + ifd_offset + 2;

        for (std::uint16_t i = 0; i < entry_count; ++i) {
            const std::uint8_t* ep = entries_data + i * 12;
            std::uint16_t tag = read_u16(ep, be);
            std::uint16_t type_raw = read_u16(ep + 2, be);
            std::uint32_t count = read_u32(ep + 4, be);

            if (type_raw < 1 || type_raw > 12) continue;
            auto type = static_cast<FieldType>(type_raw);

            std::size_t total_bytes = static_cast<std::size_t>(count) * element_size(type);

            const std::uint8_t* value_data;
            if (total_bytes <= 4) {
                value_data = ep + 8;
            } else {
                std::uint32_t offset = read_u32(ep + 8, be);
                if (offset + total_bytes > size) continue;
                value_data = data + offset;
            }

            auto field_value = read_field_value(type, count, value_data, total_bytes, be);
            if (field_value) {
                IfdEntry entry;
                entry.type = type;
                entry.value = std::move(*field_value);
                ifd.set(tag, std::move(entry));
            }
        }

        structure.ifds_.push_back(std::move(ifd));
        ifd_offset = read_u32(data + ifd_offset + 2 + entries_size, be);
    }

    if (structure.ifds_.empty()) return std::nullopt;
    return structure;
}

// Convenience accessors

std::optional<std::int64_t> Structure::image_width() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    return ifds_[0].get_int(Tag::ImageWidth);
}

std::optional<std::int64_t> Structure::image_length() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    return ifds_[0].get_int(Tag::ImageLength);
}

std::optional<double> Structure::x_resolution() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    return ifds_[0].get_double(Tag::XResolution);
}

std::optional<double> Structure::y_resolution() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    return ifds_[0].get_double(Tag::YResolution);
}

std::optional<Compression> Structure::compression() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    auto v = ifds_[0].get_int(Tag::Compression);
    if (!v) return std::nullopt;
    return static_cast<Compression>(*v);
}

std::optional<Photometric> Structure::photometric() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    auto v = ifds_[0].get_int(Tag::PhotometricInterpretation);
    if (!v) return std::nullopt;
    return static_cast<Photometric>(*v);
}

std::optional<std::uint16_t> Structure::orientation() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    auto v = ifds_[0].get_int(Tag::Orientation);
    if (!v) return std::nullopt;
    return static_cast<std::uint16_t>(*v);
}

std::optional<std::uint16_t> Structure::bits_per_sample() const noexcept {
    if (ifds_.empty()) return std::nullopt;
    auto v = ifds_[0].get_int(Tag::BitsPerSample);
    if (!v) return std::nullopt;
    return static_cast<std::uint16_t>(*v);
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

std::optional<Structure> read_tiff_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;

    auto size = file.tellg();
    if (size <= 0) return std::nullopt;
    file.seekg(0);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    if (!file) return std::nullopt;

    return Structure::read(data);
}

} // namespace ppp::core::tiff

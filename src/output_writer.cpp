#include "ppp/core/output_writer.h"

#include "ppp/core/bmp.h"
#include "ppp/core/pdf_writer.h"
#include "ppp/core/tiff_writer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace ppp::core::output {

namespace fs = std::filesystem;

namespace {

std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

/// Select TIFF compression based on pixel format and raster_format config.
tiff::Compression select_tiff_compression(PixelFormat fmt, RasterFormat rf) {
    switch (fmt) {
        case PixelFormat::BW1:
            if (rf == RasterFormat::Raw) return tiff::Compression::Uncompressed;
            if (rf == RasterFormat::Group4) return tiff::Compression::Group4Fax;
            if (rf == RasterFormat::LZW) return tiff::Compression::LZW;
            return tiff::Compression::Group4Fax;  // Default for BW1.

        case PixelFormat::Gray8:
        case PixelFormat::RGB24:
        case PixelFormat::RGBA32:
            if (rf == RasterFormat::LZW) return tiff::Compression::LZW;
            if (rf == RasterFormat::Raw) return tiff::Compression::Uncompressed;
            return tiff::Compression::LZW;  // Default for grayscale/color.
    }
    return tiff::Compression::Uncompressed;
}

/// Select TIFF photometric based on pixel format.
tiff::Photometric select_tiff_photometric(PixelFormat fmt) {
    switch (fmt) {
        case PixelFormat::BW1:
            return tiff::Photometric::WhiteIsZero;
        case PixelFormat::Gray8:
            return tiff::Photometric::BlackIsZero;
        case PixelFormat::RGB24:
        case PixelFormat::RGBA32:
            return tiff::Photometric::RGB;
    }
    return tiff::Photometric::BlackIsZero;
}

bool write_as_tiff(const Image& image, const fs::path& path,
                   const OutputConfig& config) {
    tiff::WriteOptions opts;
    opts.compression = select_tiff_compression(image.format(), config.raster_format);
    opts.photometric = select_tiff_photometric(image.format());
    return tiff::write_tiff(image, path, opts);
}

bool write_as_bmp(const Image& image, const fs::path& path) {
    return bmp::write_bmp(image, path);
}

bool write_as_pdf(const Image& image, const fs::path& path) {
    return pdf::write_pdf(image, path);
}

/// Determine output format from file extension.
std::string format_from_extension(const fs::path& path) {
    auto ext = to_lower(path.extension().string());
    if (ext == ".bmp") return "bmp";
    if (ext == ".pdf") return "pdf";
    if (ext == ".tif" || ext == ".tiff") return "tiff";
    return "tiff";  // Default.
}

}  // namespace

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

fs::path resolve_output_path(const fs::path& source_path,
                              const OutputConfig& config,
                              const std::string& suffix) {
    fs::path dir;
    if (config.save_to_different_dir && !config.output_directory.empty()) {
        dir = fs::path(config.output_directory);
    } else {
        dir = source_path.parent_path();
    }

    // Build filename.
    auto stem = source_path.stem().string();
    if (!suffix.empty()) {
        stem += suffix;
    }

    // Determine extension.
    std::string ext;
    if (!config.new_extension.empty()) {
        ext = config.new_extension;
        if (ext.front() != '.') ext = "." + ext;
    } else if (config.tiff_output) {
        ext = ".tif";
    } else {
        ext = source_path.extension().string();
        if (ext.empty()) ext = ".tif";
    }

    return dir / (stem + ext);
}

// ---------------------------------------------------------------------------
// Write output
// ---------------------------------------------------------------------------

OutputResult write_output(const Image& image,
                           const fs::path& source_path,
                           const OutputConfig& config,
                           const std::string& suffix) {
    OutputResult result;

    if (image.empty()) {
        result.error = "empty image";
        return result;
    }

    auto out_path = resolve_output_path(source_path, config, suffix);

    // Ensure output directory exists.
    std::error_code ec;
    if (out_path.has_parent_path()) {
        fs::create_directories(out_path.parent_path(), ec);
        if (ec) {
            result.error = "cannot create output directory: " + ec.message();
            return result;
        }
    }

    // Check conflict policy.
    if (config.conflict_policy == ConflictPolicy::Report && fs::exists(out_path, ec)) {
        result.error = "output file already exists: " + out_path.string();
        return result;
    }

    // Determine format and write.
    auto fmt = format_from_extension(out_path);

    bool ok = false;
    if (fmt == "bmp") {
        ok = write_as_bmp(image, out_path);
    } else if (fmt == "pdf") {
        ok = write_as_pdf(image, out_path);
    } else {
        ok = write_as_tiff(image, out_path, config);
    }

    if (!ok) {
        result.error = "failed to write " + fmt + " file: " + out_path.string();
        return result;
    }

    result.success = true;
    result.output_path = out_path;
    result.format = fmt;
    return result;
}

OutputResult write_output_to(const Image& image,
                              const fs::path& path,
                              const OutputConfig& config) {
    OutputResult result;

    if (image.empty()) {
        result.error = "empty image";
        return result;
    }

    // Ensure directory exists.
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

    auto fmt = format_from_extension(path);
    bool ok = false;

    if (fmt == "bmp") {
        ok = write_as_bmp(image, path);
    } else if (fmt == "pdf") {
        ok = write_as_pdf(image, path);
    } else {
        ok = write_as_tiff(image, path, config);
    }

    if (!ok) {
        result.error = "failed to write " + fmt + " file: " + path.string();
        return result;
    }

    result.success = true;
    result.output_path = path;
    result.format = fmt;
    return result;
}

OutputResult write_multipage_output(const std::vector<Image>& images,
                                     const fs::path& path,
                                     const OutputConfig& config) {
    OutputResult result;

    if (images.empty()) {
        result.error = "no images to write";
        return result;
    }

    // Ensure directory exists.
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }

    tiff::WriteOptions opts;
    opts.compression = select_tiff_compression(images[0].format(), config.raster_format);
    opts.photometric = select_tiff_photometric(images[0].format());

    bool ok = tiff::write_multipage_tiff(images, path, opts);

    if (!ok) {
        result.error = "failed to write multi-page TIFF: " + path.string();
        return result;
    }

    result.success = true;
    result.output_path = path;
    result.format = "tiff";
    return result;
}

} // namespace ppp::core::output

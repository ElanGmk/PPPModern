#include "ppp/core/pdf_writer.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace ppp::core::pdf {

namespace {

// ---------------------------------------------------------------------------
// PDF object builder
// ---------------------------------------------------------------------------

/// Tracks object byte offsets for the xref table.
struct PdfBuilder {
    std::ostringstream out;
    std::vector<std::size_t> offsets;  // Byte offset of each object (1-based index).
    int next_obj{1};

    int alloc_obj() { return next_obj++; }

    void begin_obj(int id) {
        while (static_cast<int>(offsets.size()) < id) offsets.push_back(0);
        offsets[id - 1] = static_cast<std::size_t>(out.tellp());
        out << id << " 0 obj\n";
    }

    void end_obj() {
        out << "endobj\n";
    }

    /// Write the cross-reference table and trailer, return the final PDF bytes.
    std::string finish(int catalog_id) {
        auto xref_offset = static_cast<std::size_t>(out.tellp());
        int n = static_cast<int>(offsets.size()) + 1;  // +1 for object 0.

        out << "xref\n0 " << n << "\n";
        // Object 0 (free).
        out << "0000000000 65535 f \n";
        for (auto off : offsets) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", off);
            out << buf;
        }

        out << "trailer\n<< /Size " << n
            << " /Root " << catalog_id << " 0 R >>\n";
        out << "startxref\n" << xref_offset << "\n%%EOF\n";

        return out.str();
    }
};

// ---------------------------------------------------------------------------
// Image data helpers
// ---------------------------------------------------------------------------

/// Get raw image data suitable for PDF embedding.
/// For BW1: returns packed bytes (MSB-first, 1 = black, matching PDF convention
/// with /BlackIs1 true).
/// For Gray8: returns row data without stride padding.
/// For RGB24: returns row data without stride padding.
/// For RGBA32: converts to RGB24.
std::vector<std::uint8_t> get_pdf_image_data(const Image& image) {
    const auto w = image.width();
    const auto h = image.height();
    std::vector<std::uint8_t> data;

    switch (image.format()) {
    case PixelFormat::BW1: {
        // BW1 is already packed MSB-first.  We need to copy row data
        // without stride padding.
        const auto row_bytes = (w + 7) / 8;
        data.resize(static_cast<std::size_t>(row_bytes) * h);
        for (std::int32_t y = 0; y < h; ++y) {
            std::memcpy(data.data() + y * row_bytes, image.row(y), row_bytes);
        }
        break;
    }
    case PixelFormat::Gray8: {
        data.resize(static_cast<std::size_t>(w) * h);
        for (std::int32_t y = 0; y < h; ++y) {
            std::memcpy(data.data() + y * w, image.row(y), w);
        }
        break;
    }
    case PixelFormat::RGB24: {
        const auto row_size = static_cast<std::size_t>(w) * 3;
        data.resize(row_size * h);
        for (std::int32_t y = 0; y < h; ++y) {
            std::memcpy(data.data() + y * row_size, image.row(y), row_size);
        }
        break;
    }
    case PixelFormat::RGBA32: {
        // Drop alpha channel.
        const auto row_size = static_cast<std::size_t>(w) * 3;
        data.resize(row_size * h);
        for (std::int32_t y = 0; y < h; ++y) {
            const auto* src = image.row(y);
            auto* dst = data.data() + y * row_size;
            for (std::int32_t x = 0; x < w; ++x) {
                dst[x * 3 + 0] = src[x * 4 + 0];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 2];
            }
        }
        break;
    }
    }

    return data;
}

/// Get PDF color space string for a pixel format.
const char* pdf_color_space(PixelFormat fmt) {
    switch (fmt) {
    case PixelFormat::BW1:    return "/DeviceGray";
    case PixelFormat::Gray8:  return "/DeviceGray";
    case PixelFormat::RGB24:  return "/DeviceRGB";
    case PixelFormat::RGBA32: return "/DeviceRGB";  // Alpha stripped.
    }
    return "/DeviceGray";
}

int pdf_bits_per_component(PixelFormat fmt) {
    return (fmt == PixelFormat::BW1) ? 1 : 8;
}

/// Resolve page dimensions in points.
void resolve_page_size(const Image& image, const PdfWriteOptions& options,
                        double& width_pt, double& height_pt) {
    if (options.page_width_pt > 0 && options.page_height_pt > 0) {
        width_pt = options.page_width_pt;
        height_pt = options.page_height_pt;
        return;
    }

    double dpi_x = image.dpi_x();
    double dpi_y = image.dpi_y();
    if (dpi_x <= 0) dpi_x = 72.0;
    if (dpi_y <= 0) dpi_y = 72.0;

    width_pt = static_cast<double>(image.width()) * 72.0 / dpi_x;
    height_pt = static_cast<double>(image.height()) * 72.0 / dpi_y;
}

// ---------------------------------------------------------------------------
// Core PDF generation
// ---------------------------------------------------------------------------

std::string build_pdf(const std::vector<const Image*>& images,
                       const PdfWriteOptions& options) {
    PdfBuilder pdf;
    pdf.out << "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";  // Header + binary comment.

    const int num_pages = static_cast<int>(images.size());

    // Pre-allocate object IDs.
    int catalog_id = pdf.alloc_obj();   // 1
    int pages_id = pdf.alloc_obj();     // 2
    int info_id = pdf.alloc_obj();      // 3

    // Per-page objects: page, image XObject, content stream = 3 per page.
    struct PageObjs {
        int page_id;
        int image_id;
        int content_id;
    };
    std::vector<PageObjs> page_objs;
    page_objs.reserve(num_pages);
    for (int i = 0; i < num_pages; ++i) {
        PageObjs po;
        po.page_id = pdf.alloc_obj();
        po.image_id = pdf.alloc_obj();
        po.content_id = pdf.alloc_obj();
        page_objs.push_back(po);
    }

    // --- Catalog ---
    pdf.begin_obj(catalog_id);
    pdf.out << "<< /Type /Catalog /Pages " << pages_id << " 0 R >>\n";
    pdf.end_obj();

    // --- Pages ---
    pdf.begin_obj(pages_id);
    pdf.out << "<< /Type /Pages /Kids [";
    for (int i = 0; i < num_pages; ++i) {
        if (i > 0) pdf.out << ' ';
        pdf.out << page_objs[i].page_id << " 0 R";
    }
    pdf.out << "] /Count " << num_pages << " >>\n";
    pdf.end_obj();

    // --- Info ---
    pdf.begin_obj(info_id);
    pdf.out << "<< /Producer (" << options.producer << ") >>\n";
    pdf.end_obj();

    // --- Per-page objects ---
    for (int i = 0; i < num_pages; ++i) {
        const auto& img = *images[i];
        const auto& po = page_objs[i];

        double page_w, page_h;
        resolve_page_size(img, options, page_w, page_h);

        // Content stream: draw the image scaled to fill the page.
        std::ostringstream cs;
        cs << "q " << page_w << " 0 0 " << page_h << " 0 0 cm /Im" << i << " Do Q";
        auto content_str = cs.str();

        // Image data.
        auto img_data = get_pdf_image_data(img);

        // BW1 images: in our BW1 format, bit=1 means foreground (black).
        // PDF with /DeviceGray and /BitsPerComponent 1 treats 0=black, 1=white
        // by default.  We set /BlackIs1 true so that 1=black matches our format,
        // but /BlackIs1 is only for CCITTFaxDecode.  For raw data, we need to
        // use /Decode [1 0] to invert the mapping (1->black, 0->white).
        bool is_bw1 = (img.format() == PixelFormat::BW1);

        // --- Image XObject ---
        pdf.begin_obj(po.image_id);
        pdf.out << "<< /Type /XObject /Subtype /Image"
                << " /Width " << img.width()
                << " /Height " << img.height()
                << " /ColorSpace " << pdf_color_space(img.format())
                << " /BitsPerComponent " << pdf_bits_per_component(img.format())
                << " /Length " << img_data.size();
        if (is_bw1) {
            pdf.out << " /Decode [1 0]";  // Invert: 1=black, 0=white.
        }
        pdf.out << " >>\nstream\n";
        pdf.out.write(reinterpret_cast<const char*>(img_data.data()),
                      static_cast<std::streamsize>(img_data.size()));
        pdf.out << "\nendstream\n";
        pdf.end_obj();

        // --- Content stream ---
        pdf.begin_obj(po.content_id);
        pdf.out << "<< /Length " << content_str.size() << " >>\nstream\n"
                << content_str << "\nendstream\n";
        pdf.end_obj();

        // --- Page ---
        pdf.begin_obj(po.page_id);
        pdf.out << "<< /Type /Page /Parent " << pages_id << " 0 R"
                << " /MediaBox [0 0 " << page_w << " " << page_h << "]"
                << " /Contents " << po.content_id << " 0 R"
                << " /Resources << /XObject << /Im" << i << " "
                << po.image_id << " 0 R >> >> >>\n";
        pdf.end_obj();
    }

    return pdf.finish(catalog_id);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool write_pdf(const Image& image,
                const std::filesystem::path& path,
                const PdfWriteOptions& options) {
    if (image.empty()) return false;

    const Image* img_ptr = &image;
    auto data = build_pdf({img_ptr}, options);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.flush();
    return file.good();
}

std::vector<std::uint8_t> write_pdf_to_memory(
    const Image& image,
    const PdfWriteOptions& options) {
    if (image.empty()) return {};

    const Image* img_ptr = &image;
    auto data = build_pdf({img_ptr}, options);

    return {data.begin(), data.end()};
}

bool write_multipage_pdf(const std::vector<Image>& images,
                          const std::filesystem::path& path,
                          const PdfWriteOptions& options) {
    if (images.empty()) return false;

    std::vector<const Image*> ptrs;
    ptrs.reserve(images.size());
    for (const auto& img : images) {
        if (img.empty()) return false;
        ptrs.push_back(&img);
    }

    auto data = build_pdf(ptrs, options);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.flush();
    return file.good();
}

} // namespace ppp::core::pdf

// ppp_batch — Command-line batch image processing tool for PPP Modern.
//
// Usage:
//   ppp_batch --input <dir> --output <dir> [--profile <json>]
//             [--extensions tif,tiff,bmp] [--overwrite] [--verbose]
//             [--good-list <path>] [--exception-list <path>]
//
// Processes all image files in the input directory through the PPP
// processing pipeline and writes results to the output directory.
// Produces a Good Files List and Exception List as JSON.

#include "ppp/core/bmp.h"
#include "ppp/core/image.h"
#include "ppp/core/output_writer.h"
#include "ppp/core/processing_config.h"
#include "ppp/core/processing_config_io.h"
#include "ppp/core/processing_pipeline.h"
#include "ppp/core/tiff_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace ppp::core;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string escape_json(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

std::string timestamp_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S");
    return oss.str();
}

// ---------------------------------------------------------------------------
// Per-file result
// ---------------------------------------------------------------------------

struct FileResult {
    fs::path source_path;
    fs::path output_path;
    bool success{false};
    bool is_blank{false};
    std::string error;
    std::vector<ProcessingStep> steps;
    double elapsed_ms{0.0};
};

// ---------------------------------------------------------------------------
// Collect input files
// ---------------------------------------------------------------------------

std::vector<fs::path> collect_input_files(const fs::path& input_dir,
                                           const std::vector<std::string>& extensions) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(input_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        auto ext = to_lower(entry.path().extension().string());
        for (auto& allowed : extensions) {
            if (ext == allowed) {
                files.push_back(entry.path());
                break;
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// ---------------------------------------------------------------------------
// Load image (detect format from extension)
// ---------------------------------------------------------------------------

Image load_image(const fs::path& path) {
    auto ext = to_lower(path.extension().string());
    if (ext == ".bmp") {
        return bmp::read_bmp_file(path);
    } else if (ext == ".tif" || ext == ".tiff") {
        return tiff::read_tiff_image_file(path);
    } else {
        auto img = tiff::read_tiff_image_file(path);
        if (img.empty()) img = bmp::read_bmp_file(path);
        return img;
    }
}

// ---------------------------------------------------------------------------
// Process a single file
// ---------------------------------------------------------------------------

FileResult process_file(const fs::path& source,
                        const fs::path& output_dir,
                        const ProcessingProfile& profile,
                        bool overwrite) {
    FileResult fr;
    fr.source_path = source;

    auto t0 = std::chrono::steady_clock::now();

    // Load image.
    auto img = load_image(source);
    if (img.empty()) {
        fr.error = "cannot read image";
        auto t1 = std::chrono::steady_clock::now();
        fr.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return fr;
    }

    // Run pipeline.
    auto result = run_pipeline(img, profile);
    fr.steps = result.steps;
    fr.is_blank = result.is_blank;

    if (!result.success) {
        fr.error = result.error;
        auto t1 = std::chrono::steady_clock::now();
        fr.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return fr;
    }

    // Check if any step actually modified the image.
    bool any_applied = false;
    for (auto& s : result.steps)
        if (s.applied) { any_applied = true; break; }

    // Determine output path.
    auto out_path = output_dir / source.filename();
    fr.output_path = out_path;

    // Check conflict.
    if (fs::exists(out_path) && !overwrite) {
        fr.error = "output file exists (use --overwrite)";
        auto t1 = std::chrono::steady_clock::now();
        fr.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return fr;
    }

    // Write output.
    bool write_ok = false;
    if (!any_applied) {
        // No processing changes — copy original file as-is.
        std::error_code ec;
        fs::copy_file(source, out_path, fs::copy_options::overwrite_existing, ec);
        write_ok = !ec;
        if (ec) fr.error = "copy failed: " + ec.message();
    } else {
        // Write processed image using output writer.
        auto wr = output::write_output_to(result.image, out_path, profile.output);
        write_ok = wr.success;
        if (!wr.success) fr.error = wr.error;
        fr.output_path = wr.output_path;
    }

    fr.success = write_ok;
    auto t1 = std::chrono::steady_clock::now();
    fr.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return fr;
}

// ---------------------------------------------------------------------------
// Write JSON lists
// ---------------------------------------------------------------------------

bool write_good_list(const std::vector<FileResult>& results,
                     const fs::path& path,
                     int total) {
    std::ofstream f(path);
    if (!f) return false;

    int count = 0;
    for (auto& r : results)
        if (r.success) ++count;

    f << "{\n";
    f << "  \"timestamp\": \"" << timestamp_iso8601() << "\",\n";
    f << "  \"total_processed\": " << total << ",\n";
    f << "  \"succeeded\": " << count << ",\n";
    f << "  \"files\": [\n";

    bool first = true;
    for (auto& r : results) {
        if (!r.success) continue;
        if (!first) f << ",\n";
        first = false;

        f << "    {\n";
        f << "      \"source\": \"" << escape_json(r.source_path.string()) << "\",\n";
        f << "      \"output\": \"" << escape_json(r.output_path.string()) << "\",\n";
        f << "      \"elapsed_ms\": " << std::fixed << std::setprecision(1) << r.elapsed_ms << ",\n";
        f << "      \"is_blank\": " << (r.is_blank ? "true" : "false") << ",\n";

        // Steps applied.
        f << "      \"steps_applied\": [";
        bool first_step = true;
        for (auto& s : r.steps) {
            if (!s.applied) continue;
            if (!first_step) f << ", ";
            first_step = false;
            f << "\"" << escape_json(s.name) << "\"";
        }
        f << "],\n";

        // Step details.
        f << "      \"step_details\": [";
        first_step = true;
        for (auto& s : r.steps) {
            if (!first_step) f << ", ";
            first_step = false;
            f << "{\"name\": \"" << escape_json(s.name)
              << "\", \"applied\": " << (s.applied ? "true" : "false")
              << ", \"detail\": \"" << escape_json(s.detail) << "\"}";
        }
        f << "]\n";

        f << "    }";
    }

    f << "\n  ]\n";
    f << "}\n";
    return f.good();
}

bool write_exception_list(const std::vector<FileResult>& results,
                          const fs::path& path,
                          int total) {
    std::ofstream f(path);
    if (!f) return false;

    int count = 0;
    for (auto& r : results)
        if (!r.success) ++count;

    f << "{\n";
    f << "  \"timestamp\": \"" << timestamp_iso8601() << "\",\n";
    f << "  \"total_processed\": " << total << ",\n";
    f << "  \"total_exceptions\": " << count << ",\n";
    f << "  \"files\": [\n";

    bool first = true;
    for (auto& r : results) {
        if (r.success) continue;
        if (!first) f << ",\n";
        first = false;

        f << "    {\n";
        f << "      \"source\": \"" << escape_json(r.source_path.string()) << "\",\n";
        f << "      \"error\": \"" << escape_json(r.error) << "\",\n";
        f << "      \"is_blank\": " << (r.is_blank ? "true" : "false") << ",\n";
        f << "      \"elapsed_ms\": " << std::fixed << std::setprecision(1) << r.elapsed_ms << "\n";
        f << "    }";
    }

    f << "\n  ]\n";
    f << "}\n";
    return f.good();
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

void print_usage() {
    std::cerr
        << "ppp_batch — PPP Modern batch image processing tool\n"
        << "\n"
        << "Usage:\n"
        << "  ppp_batch --input <dir> --output <dir> [options]\n"
        << "\n"
        << "Required:\n"
        << "  --input  <dir>     Input directory containing image files\n"
        << "  --output <dir>     Output directory for processed files\n"
        << "\n"
        << "Options:\n"
        << "  --profile <json>   Processing profile JSON file (default: built-in defaults)\n"
        << "  --extensions <ext> Comma-separated extensions (default: .tif,.tiff,.bmp)\n"
        << "  --overwrite        Overwrite existing output files\n"
        << "  --verbose          Print per-step details for each file\n"
        << "  --good-list <path> Path for good files JSON (default: <output>/good_files.json)\n"
        << "  --exception-list <path>  Path for exception JSON (default: <output>/exceptions.json)\n"
        << "  --save-profile <path>    Save default profile to JSON and exit\n"
        << "\n"
        << "Exit codes:\n"
        << "  0  All files processed successfully\n"
        << "  1  Some files had exceptions (partial success)\n"
        << "  2  Usage error or setup failure\n";
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    fs::path input_dir, output_dir, profile_path;
    fs::path good_list_path, exception_list_path;
    fs::path save_profile_path;
    std::vector<std::string> extensions = {".tif", ".tiff", ".bmp"};
    bool overwrite = false;
    bool verbose = false;

    // Parse arguments.
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        } else if (arg == "--input" && i + 1 < argc) {
            input_dir = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_dir = argv[++i];
        } else if (arg == "--profile" && i + 1 < argc) {
            profile_path = argv[++i];
        } else if (arg == "--extensions" && i + 1 < argc) {
            extensions.clear();
            std::string exts{argv[++i]};
            std::istringstream ss(exts);
            std::string ext;
            while (std::getline(ss, ext, ',')) {
                ext = to_lower(ext);
                if (!ext.empty() && ext[0] != '.') ext = "." + ext;
                if (!ext.empty()) extensions.push_back(ext);
            }
        } else if (arg == "--overwrite") {
            overwrite = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--good-list" && i + 1 < argc) {
            good_list_path = argv[++i];
        } else if (arg == "--exception-list" && i + 1 < argc) {
            exception_list_path = argv[++i];
        } else if (arg == "--save-profile" && i + 1 < argc) {
            save_profile_path = argv[++i];
        } else {
            std::cerr << "error: unknown argument: " << arg << "\n\n";
            print_usage();
            return 2;
        }
    }

    // Handle --save-profile: write default profile and exit.
    if (!save_profile_path.empty()) {
        ProcessingProfile default_profile;
        default_profile.name = "Default";
        if (write_processing_profile(default_profile, save_profile_path)) {
            std::cout << "Saved default profile to: " << save_profile_path.string() << std::endl;
            return 0;
        } else {
            std::cerr << "error: cannot write profile to: " << save_profile_path.string() << std::endl;
            return 2;
        }
    }

    // Validate required arguments.
    if (input_dir.empty() || output_dir.empty()) {
        std::cerr << "error: --input and --output are required\n\n";
        print_usage();
        return 2;
    }

    if (!fs::is_directory(input_dir)) {
        std::cerr << "error: input directory does not exist: " << input_dir.string() << std::endl;
        return 2;
    }

    // Create output directory.
    {
        std::error_code ec;
        fs::create_directories(output_dir, ec);
        if (!fs::is_directory(output_dir)) {
            std::cerr << "error: cannot create output directory: " << output_dir.string() << std::endl;
            return 2;
        }
    }

    // Set default list paths.
    if (good_list_path.empty()) good_list_path = output_dir / "good_files.json";
    if (exception_list_path.empty()) exception_list_path = output_dir / "exceptions.json";

    // Load profile.
    ProcessingProfile profile;
    if (!profile_path.empty()) {
        auto loaded = read_processing_profile(profile_path);
        if (!loaded) {
            std::cerr << "error: cannot load profile: " << profile_path.string() << std::endl;
            return 2;
        }
        profile = std::move(*loaded);
        std::cerr << "Profile: " << profile.name << std::endl;
    } else {
        profile.name = "Default";
        std::cerr << "Profile: Default (built-in)" << std::endl;
    }

    // Configure output settings for the output directory.
    profile.output.save_to_different_dir = true;
    profile.output.output_directory = output_dir.string();
    if (overwrite)
        profile.output.conflict_policy = ConflictPolicy::Overwrite;
    else
        profile.output.conflict_policy = ConflictPolicy::Report;

    // Collect input files.
    auto files = collect_input_files(input_dir, extensions);
    if (files.empty()) {
        std::cerr << "error: no matching files found in: " << input_dir.string() << std::endl;
        std::cerr << "  extensions: ";
        for (auto& e : extensions) std::cerr << e << " ";
        std::cerr << std::endl;
        return 2;
    }

    std::cerr << "Input:  " << input_dir.string() << std::endl;
    std::cerr << "Output: " << output_dir.string() << std::endl;
    std::cerr << "Files:  " << files.size() << std::endl;
    std::cerr << std::endl;

    // Process files.
    std::vector<FileResult> results;
    results.reserve(files.size());

    int succeeded = 0, failed = 0;
    auto batch_t0 = std::chrono::steady_clock::now();

    for (std::size_t i = 0; i < files.size(); ++i) {
        auto& file = files[i];
        std::cerr << "[" << (i + 1) << "/" << files.size() << "] "
                  << file.filename().string() << " ... " << std::flush;

        auto fr = process_file(file, output_dir, profile, overwrite);
        results.push_back(fr);

        if (fr.success) {
            ++succeeded;
            std::cerr << "OK";
        } else {
            ++failed;
            std::cerr << "FAIL: " << fr.error;
        }
        std::cerr << " (" << std::fixed << std::setprecision(0)
                  << fr.elapsed_ms << "ms)" << std::endl;

        if (verbose && fr.success) {
            for (auto& s : fr.steps) {
                std::cerr << "    [" << (s.applied ? "+" : "-") << "] "
                          << s.name << ": " << s.detail << std::endl;
            }
        }
    }

    auto batch_t1 = std::chrono::steady_clock::now();
    double batch_ms = std::chrono::duration<double, std::milli>(batch_t1 - batch_t0).count();

    // Write output lists.
    std::cerr << std::endl;

    int total = static_cast<int>(files.size());
    if (write_good_list(results, good_list_path, total)) {
        std::cerr << "Good list:      " << good_list_path.string()
                  << " (" << succeeded << " files)" << std::endl;
    } else {
        std::cerr << "warning: failed to write good list: " << good_list_path.string() << std::endl;
    }

    if (write_exception_list(results, exception_list_path, total)) {
        std::cerr << "Exception list: " << exception_list_path.string()
                  << " (" << failed << " files)" << std::endl;
    } else {
        std::cerr << "warning: failed to write exception list: " << exception_list_path.string() << std::endl;
    }

    // Summary.
    std::cerr << std::endl;
    std::cerr << "=== Summary ===" << std::endl;
    std::cerr << "  Total:      " << total << " files" << std::endl;
    std::cerr << "  Succeeded:  " << succeeded << std::endl;
    std::cerr << "  Exceptions: " << failed << std::endl;
    std::cerr << "  Time:       " << std::fixed << std::setprecision(0) << batch_ms << "ms"
              << " (" << std::setprecision(0) << batch_ms / total << "ms/file)" << std::endl;

    // Also output a machine-readable summary to stdout.
    std::cout << "{\"total\": " << total
              << ", \"succeeded\": " << succeeded
              << ", \"failed\": " << failed
              << ", \"elapsed_ms\": " << std::fixed << std::setprecision(1) << batch_ms
              << ", \"good_list\": \"" << escape_json(good_list_path.string()) << "\""
              << ", \"exception_list\": \"" << escape_json(exception_list_path.string()) << "\""
              << "}" << std::endl;

    return (failed > 0) ? 1 : 0;
}

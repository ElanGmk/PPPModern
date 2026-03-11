// PPP Modern — Regression test suite.
//
// Runs ppp_batch with 20 different processing profiles against the 10 sample
// TIFF files and verifies success/failure counts and applied processing steps.
//
// Build requirements:
//   PPP_REGRESSION_INPUT_DIR  — path to test input directory (test2/in)
//   PPP_BATCH_EXE             — path to ppp_batch executable
//   PPP_BASELINE_DIR          — path to baselines directory

#include "ppp/core/processing_config.h"
#include "ppp/core/processing_config_io.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ppp::core;

// ---------------------------------------------------------------------------
// Macros from compile definitions
// ---------------------------------------------------------------------------

#ifndef PPP_REGRESSION_INPUT_DIR
#define PPP_REGRESSION_INPUT_DIR ""
#endif
#ifndef PPP_BATCH_EXE
#define PPP_BATCH_EXE ""
#endif
#ifndef PPP_BASELINE_DIR
#define PPP_BASELINE_DIR ""
#endif

// ---------------------------------------------------------------------------
// RAII temp directory
// ---------------------------------------------------------------------------

namespace {

struct TempDir {
    fs::path path;

    TempDir() {
        auto base = fs::temp_directory_path() / "ppp_regression";
        std::error_code ec;
        fs::create_directories(base, ec);
        auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
        auto rng = static_cast<unsigned long long>(std::random_device{}());
        path = base / ("run-" + std::to_string(ts) + "-" + std::to_string(rng));
        fs::create_directories(path, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Minimal JSON value extraction from the flat stdout summary line.
int json_int(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return -1;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return -1;
    ++pos;
    while (pos < json.size() && json[pos] == ' ') ++pos;
    return std::atoi(json.c_str() + pos);
}

// Check if a step name appears in good_files.json steps_applied arrays.
bool has_step(const std::string& good_json, const std::string& step) {
    return good_json.find("\"" + step + "\"") != std::string::npos;
}

// Count output TIFF files in a directory.
int count_tifs(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".tif") ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Test result
// ---------------------------------------------------------------------------

struct BatchResult {
    int total{0};
    int succeeded{0};
    int failed{0};
    int exit_code{-1};
    std::string stdout_json;
    std::string good_list;
    std::string exception_list;
    fs::path output_dir;
};

// Run ppp_batch with a profile and return results.
BatchResult run_batch(const ProcessingProfile& profile, const std::string& case_name) {
    TempDir tmp;
    BatchResult br;
    br.output_dir = tmp.path / "out";
    fs::create_directories(br.output_dir);

    auto profile_path = tmp.path / (case_name + ".json");
    (void)write_processing_profile(profile, profile_path);

    // Build command.  On Windows, cmd.exe requires the entire command to be
    // wrapped in an outer pair of quotes when the executable path is quoted.
    std::string cmd;
#ifdef _WIN32
    cmd += "\"";  // Outer quote for cmd.exe
#endif
    cmd += "\"" + std::string(PPP_BATCH_EXE) + "\"";
    cmd += " --input \"" + std::string(PPP_REGRESSION_INPUT_DIR) + "\"";
    cmd += " --output \"" + br.output_dir.string() + "\"";
    cmd += " --profile \"" + profile_path.string() + "\"";
    cmd += " --overwrite";
#ifdef _WIN32
    cmd += " 2>nul\"";  // Close outer quote, redirect stderr
#else
    cmd += " 2>/dev/null";
#endif

    // Execute and capture stdout.
#ifdef _WIN32
    auto* pipe = _popen(cmd.c_str(), "r");
#else
    auto* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        br.exit_code = -1;
        return br;
    }

    char buf[4096];
    std::string output;
    while (std::fgets(buf, sizeof(buf), pipe))
        output += buf;

#ifdef _WIN32
    br.exit_code = _pclose(pipe);
#else
    br.exit_code = pclose(pipe);
#endif

    br.stdout_json = output;
    br.total = json_int(output, "total");
    br.succeeded = json_int(output, "succeeded");
    br.failed = json_int(output, "failed");

    // Read JSON lists.
    br.good_list = read_file(br.output_dir / "good_files.json");
    br.exception_list = read_file(br.output_dir / "exceptions.json");

    return br;
}

// Create a default profile suitable for regression tests.
ProcessingProfile default_profile() {
    ProcessingProfile p;
    p.name = "regression_default";
    return p;
}

// ---------------------------------------------------------------------------
// Assertion helpers
// ---------------------------------------------------------------------------

int g_pass = 0, g_fail = 0;

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::cerr << "  FAIL: " << #a << " == " << (a) << ", expected " << (b) << \
            " at line " << __LINE__ << std::endl; \
        return false; \
    } \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << " at line " << __LINE__ << std::endl; \
        return false; \
    } \
} while (0)

// ===========================================================================
// Group A: Page Setup
// ===========================================================================

bool test_A1_page_detect_disabled() {
    auto p = default_profile();
    p.position_image = false;
    auto r = run_batch(p, "A1_page_detect_disabled");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    // With positioning disabled, margins step should not be applied.
    ASSERT_TRUE(!has_step(r.good_list, "margins") ||
                r.good_list.find("\"positioning disabled\"") != std::string::npos);
    return true;
}

bool test_A2_rotate_cw90() {
    auto p = default_profile();
    p.rotation = Rotation::CW90;
    auto r = run_batch(p, "A2_rotate_cw90");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "rotate"));
    ASSERT_TRUE(r.good_list.find("CW 90") != std::string::npos);
    return true;
}

bool test_A3_rotate_ccw90() {
    auto p = default_profile();
    p.rotation = Rotation::CCW90;
    auto r = run_batch(p, "A3_rotate_ccw90");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "rotate"));
    ASSERT_TRUE(r.good_list.find("CCW 90") != std::string::npos);
    return true;
}

bool test_A4_rotate_180() {
    auto p = default_profile();
    p.rotation = Rotation::R180;
    auto r = run_batch(p, "A4_rotate_180");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "rotate"));
    return true;
}

bool test_A5_canvas_a4_landscape() {
    auto p = default_profile();
    p.canvas.preset = CanvasPreset::A4;
    p.canvas.width = {210.0, MeasurementUnit::Millimeters};
    p.canvas.height = {297.0, MeasurementUnit::Millimeters};
    p.canvas.orientation = Orientation::Landscape;
    p.keep_original_size = false;
    auto r = run_batch(p, "A5_canvas_a4_landscape");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    return true;
}

bool test_A6_keep_outside_subimage() {
    auto p = default_profile();
    p.position_image = true;
    p.keep_outside_subimage = true;
    auto r = run_batch(p, "A6_keep_outside_subimage");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "detect_subimage"));
    return true;
}

// ===========================================================================
// Group B: Margins
// ===========================================================================

bool test_B1_margins_set_all() {
    auto p = default_profile();
    Measurement half_inch{0.5, MeasurementUnit::Inches};
    for (auto& m : p.margins) {
        m.top = {half_inch, MarginMode::Set};
        m.left = {half_inch, MarginMode::Set};
        m.right = {half_inch, MarginMode::Set};
        m.bottom = {half_inch, MarginMode::Set};
    }
    auto r = run_batch(p, "B1_margins_set_all");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_B2_margins_check_mode() {
    auto p = default_profile();
    Measurement quarter{0.25, MeasurementUnit::Inches};
    p.margins[0].top = {quarter, MarginMode::Set};
    p.margins[0].left = {quarter, MarginMode::Set};
    p.margins[0].right = {quarter, MarginMode::Check};
    p.margins[0].bottom = {quarter, MarginMode::Check};
    auto r = run_batch(p, "B2_margins_check_mode");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    return true;
}

bool test_B3_margins_center() {
    auto p = default_profile();
    p.margins[0].center_horizontal = true;
    p.margins[0].center_vertical = true;
    auto r = run_batch(p, "B3_margins_center");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_B4_margins_odd_even() {
    auto p = default_profile();
    p.odd_even_mode = true;
    p.margins[0].top = {{0.5, MeasurementUnit::Inches}, MarginMode::Set};
    p.margins[1].top = {{1.0, MeasurementUnit::Inches}, MarginMode::Set};
    auto r = run_batch(p, "B4_margins_odd_even");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    return true;
}

bool test_B5_offset_position() {
    auto p = default_profile();
    p.offsets[0].x = {0.25, MeasurementUnit::Inches};
    p.offsets[0].y = {0.5, MeasurementUnit::Inches};
    auto r = run_batch(p, "B5_offset_position");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    return true;
}

// ===========================================================================
// Group C: Cleanup
// ===========================================================================

bool test_C1_despeckle_single() {
    auto p = default_profile();
    p.despeckle.mode = DespeckleMode::SinglePixel;
    auto r = run_batch(p, "C1_despeckle_single");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "despeckle"));
    return true;
}

bool test_C2_despeckle_object() {
    auto p = default_profile();
    p.despeckle.mode = DespeckleMode::Object;
    p.despeckle.object_min = 1;
    p.despeckle.object_max = 5;
    auto r = run_batch(p, "C2_despeckle_object");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "despeckle"));
    return true;
}

bool test_C3_edge_cleanup_before() {
    auto p = default_profile();
    p.edge_cleanup.enabled = true;
    p.edge_cleanup.order = EdgeCleanupOrder::BeforeDeskew;
    Measurement tenth{0.1, MeasurementUnit::Inches};
    p.edge_cleanup.set1 = {tenth, tenth, tenth, tenth};
    auto r = run_batch(p, "C3_edge_cleanup_before");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "edge_cleanup"));
    return true;
}

bool test_C4_hole_cleanup() {
    auto p = default_profile();
    p.hole_cleanup.enabled = true;
    Measurement half{0.5, MeasurementUnit::Inches};
    p.hole_cleanup.set1.left = half;
    auto r = run_batch(p, "C4_hole_cleanup");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "hole_cleanup"));
    return true;
}

bool test_C5_subimage_report_small() {
    auto p = default_profile();
    p.subimage.report_small = true;
    p.subimage.min_width_px = 50000;   // Impossibly large — forces "too small".
    p.subimage.min_height_px = 50000;
    auto r = run_batch(p, "C5_subimage_report_small");
    // All should succeed (report_small is informational, not an error).
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "detect_subimage"));
    // With impossibly large min sizes, all components are filtered out.
    ASSERT_TRUE(r.good_list.find("no content detected") != std::string::npos);
    return true;
}

// ===========================================================================
// Group D: Deskew
// ===========================================================================

bool test_D1_deskew_basic() {
    auto p = default_profile();
    p.deskew.enabled = true;
    p.deskew.min_angle = 0.05;
    p.deskew.max_angle = 3.0;
    auto r = run_batch(p, "D1_deskew_basic");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "deskew"));
    return true;
}

bool test_D2_deskew_interpolated() {
    auto p = default_profile();
    p.deskew.enabled = true;
    p.deskew.interpolate = true;
    p.deskew.min_angle = 0.05;
    p.deskew.max_angle = 5.0;
    auto r = run_batch(p, "D2_deskew_interpolated");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "deskew"));
    return true;
}

// ===========================================================================
// Group E: Combined
// ===========================================================================

bool test_E1_full_pipeline() {
    auto p = default_profile();
    p.position_image = true;
    p.deskew.enabled = true;
    p.deskew.min_angle = 0.05;
    p.deskew.max_angle = 3.0;
    p.despeckle.mode = DespeckleMode::SinglePixel;
    p.edge_cleanup.enabled = true;
    p.edge_cleanup.order = EdgeCleanupOrder::AfterDeskew;
    Measurement tenth{0.1, MeasurementUnit::Inches};
    p.edge_cleanup.set1 = {tenth, tenth, tenth, tenth};
    Measurement quarter{0.25, MeasurementUnit::Inches};
    for (auto& m : p.margins) {
        m.top = {quarter, MarginMode::Set};
        m.left = {quarter, MarginMode::Set};
        m.right = {quarter, MarginMode::Set};
        m.bottom = {quarter, MarginMode::Set};
    }
    p.canvas.preset = CanvasPreset::Letter;
    p.canvas.orientation = Orientation::Portrait;
    auto r = run_batch(p, "E1_full_pipeline");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "deskew"));
    ASSERT_TRUE(has_step(r.good_list, "despeckle"));
    ASSERT_TRUE(has_step(r.good_list, "edge_cleanup"));
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_E2_movement_limit() {
    auto p = default_profile();
    p.position_image = true;
    p.movement_limit.enabled = true;
    p.movement_limit.max_vertical = {2.0, MeasurementUnit::Inches};
    p.movement_limit.max_horizontal = {1.5, MeasurementUnit::Inches};
    auto r = run_batch(p, "E2_movement_limit");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    return true;
}

// ===========================================================================
// Group F: Alignment
// ===========================================================================

bool test_F1_margin_center_h() {
    auto p = default_profile();
    p.position_image = true;
    p.canvas.preset = CanvasPreset::Letter;
    p.canvas.orientation = Orientation::Portrait;
    p.keep_original_size = false;
    p.margins[0].center_horizontal = true;
    p.margins[0].center_vertical = false;
    Measurement half{0.5, MeasurementUnit::Inches};
    p.margins[0].top = {half, MarginMode::Set};
    p.margins[0].bottom = {half, MarginMode::Set};
    auto r = run_batch(p, "F1_margin_center_h");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_F2_margin_center_v() {
    auto p = default_profile();
    p.position_image = true;
    p.canvas.preset = CanvasPreset::Letter;
    p.canvas.orientation = Orientation::Portrait;
    p.keep_original_size = false;
    p.margins[0].center_horizontal = false;
    p.margins[0].center_vertical = true;
    Measurement half{0.5, MeasurementUnit::Inches};
    p.margins[0].left = {half, MarginMode::Set};
    p.margins[0].right = {half, MarginMode::Set};
    auto r = run_batch(p, "F2_margin_center_v");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_F3_margin_center_both() {
    auto p = default_profile();
    p.position_image = true;
    p.canvas.preset = CanvasPreset::Letter;
    p.canvas.orientation = Orientation::Portrait;
    p.keep_original_size = false;
    p.margins[0].center_horizontal = true;
    p.margins[0].center_vertical = true;
    auto r = run_batch(p, "F3_margin_center_both");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_F4_margin_keep_position() {
    auto p = default_profile();
    p.position_image = true;
    p.canvas.preset = CanvasPreset::Letter;
    p.canvas.orientation = Orientation::Portrait;
    p.keep_original_size = false;
    p.margins[0].keep_horizontal = true;
    p.margins[0].keep_vertical = true;
    auto r = run_batch(p, "F4_margin_keep_position");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_F5_resize_valign_top() {
    auto p = default_profile();
    p.resize.enabled = true;
    p.resize.canvas.preset = CanvasPreset::Letter;
    p.resize.canvas.orientation = Orientation::Portrait;
    p.resize.v_alignment = VAlignment::Top;
    p.resize.h_alignment = HAlignment::Center;
    p.resize.allow_shrink = true;
    p.resize.allow_enlarge = true;
    auto r = run_batch(p, "F5_resize_valign_top");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "resize"));
    return true;
}

bool test_F6_resize_valign_bottom() {
    auto p = default_profile();
    p.resize.enabled = true;
    p.resize.canvas.preset = CanvasPreset::Letter;
    p.resize.canvas.orientation = Orientation::Portrait;
    p.resize.v_alignment = VAlignment::Bottom;
    p.resize.h_alignment = HAlignment::Center;
    p.resize.allow_shrink = true;
    p.resize.allow_enlarge = true;
    auto r = run_batch(p, "F6_resize_valign_bottom");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "resize"));
    return true;
}

bool test_F7_resize_valign_proportional() {
    auto p = default_profile();
    p.resize.enabled = true;
    p.resize.canvas.preset = CanvasPreset::Letter;
    p.resize.canvas.orientation = Orientation::Portrait;
    p.resize.v_alignment = VAlignment::Proportional;
    p.resize.h_alignment = HAlignment::Proportional;
    p.resize.allow_shrink = true;
    p.resize.allow_enlarge = true;
    auto r = run_batch(p, "F7_resize_valign_proportional");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "resize"));
    return true;
}

bool test_F8_resize_no_shrink() {
    auto p = default_profile();
    p.resize.enabled = true;
    p.resize.canvas.preset = CanvasPreset::Letter;
    p.resize.canvas.orientation = Orientation::Portrait;
    p.resize.v_alignment = VAlignment::Center;
    p.resize.h_alignment = HAlignment::Center;
    p.resize.allow_shrink = false;
    p.resize.allow_enlarge = true;
    auto r = run_batch(p, "F8_resize_no_shrink");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    return true;
}

bool test_F9_margin_to_margin_offset() {
    // Set margins + offset: explicit left/right margins with horizontal offset.
    auto p = default_profile();
    p.position_image = true;
    p.canvas.preset = CanvasPreset::Letter;
    p.canvas.orientation = Orientation::Portrait;
    p.keep_original_size = false;
    Measurement quarter{0.25, MeasurementUnit::Inches};
    Measurement half{0.5, MeasurementUnit::Inches};
    p.margins[0].top = {quarter, MarginMode::Set};
    p.margins[0].left = {half, MarginMode::Set};
    p.margins[0].right = {quarter, MarginMode::Set};
    p.margins[0].bottom = {quarter, MarginMode::Set};
    p.offsets[0].x = {0.125, MeasurementUnit::Inches};
    p.offsets[0].y = {0.125, MeasurementUnit::Inches};
    auto r = run_batch(p, "F9_margin_to_margin_offset");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "margins"));
    return true;
}

bool test_F10_resize_center_with_margins() {
    // Resize with centered alignment + explicit resize margins.
    auto p = default_profile();
    p.resize.enabled = true;
    p.resize.canvas.preset = CanvasPreset::Letter;
    p.resize.canvas.orientation = Orientation::Portrait;
    p.resize.v_alignment = VAlignment::Center;
    p.resize.h_alignment = HAlignment::Center;
    p.resize.allow_shrink = true;
    p.resize.allow_enlarge = true;
    Measurement quarter{0.25, MeasurementUnit::Inches};
    p.resize.margins_set1 = {quarter, quarter, quarter, quarter};
    auto r = run_batch(p, "F10_resize_center_with_margins");
    ASSERT_EQ(r.succeeded, 10);
    ASSERT_EQ(r.failed, 0);
    ASSERT_TRUE(has_step(r.good_list, "resize"));
    return true;
}

// ===========================================================================
// Baseline capture and verification
// ===========================================================================

struct BaselineEntry {
    std::string case_name;
    int expected_succeeded;
    int expected_failed;
};

// Write baseline manifest from a set of batch results.
void capture_baselines(const std::vector<std::pair<std::string, BatchResult>>& results,
                       const fs::path& baseline_dir) {
    fs::create_directories(baseline_dir);
    auto manifest_path = baseline_dir / "baseline_manifest.json";
    std::ofstream f(manifest_path);
    f << "{\n  \"version\": 1,\n  \"cases\": {\n";

    bool first = true;
    for (auto& [name, br] : results) {
        if (!first) f << ",\n";
        first = false;
        f << "    \"" << name << "\": {\n";
        f << "      \"succeeded\": " << br.succeeded << ",\n";
        f << "      \"failed\": " << br.failed << ",\n";
        f << "      \"output_files\": " << count_tifs(br.output_dir) << "\n";
        f << "    }";
    }

    f << "\n  }\n}\n";
    std::cout << "Baseline manifest written to: " << manifest_path.string() << std::endl;
}

} // anonymous namespace

// ===========================================================================
// Main
// ===========================================================================

int main(int argc, char* argv[]) {
    // Check prerequisites.
    fs::path input_dir{PPP_REGRESSION_INPUT_DIR};
    fs::path batch_exe{PPP_BATCH_EXE};

    if (!fs::is_directory(input_dir)) {
        std::cerr << "error: input directory not found: " << input_dir.string() << std::endl;
        std::cerr << "  set PPP_REGRESSION_INPUT_DIR or ensure test2/in exists" << std::endl;
        return 2;
    }

    if (!fs::exists(batch_exe)) {
        std::cerr << "error: ppp_batch not found: " << batch_exe.string() << std::endl;
        std::cerr << "  build ppp_batch first" << std::endl;
        return 2;
    }

    // Parse --filter argument.
    std::string filter;
    bool capture_mode = false;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg == "--capture-baseline") {
            capture_mode = true;
        }
    }

    // Register test cases.
    std::vector<std::pair<std::string, bool (*)()>> tests = {
        // Group A: Page Setup
        {"A1_page_detect_disabled",   test_A1_page_detect_disabled},
        {"A2_rotate_cw90",            test_A2_rotate_cw90},
        {"A3_rotate_ccw90",           test_A3_rotate_ccw90},
        {"A4_rotate_180",             test_A4_rotate_180},
        {"A5_canvas_a4_landscape",    test_A5_canvas_a4_landscape},
        {"A6_keep_outside_subimage",  test_A6_keep_outside_subimage},
        // Group B: Margins
        {"B1_margins_set_all",        test_B1_margins_set_all},
        {"B2_margins_check_mode",     test_B2_margins_check_mode},
        {"B3_margins_center",         test_B3_margins_center},
        {"B4_margins_odd_even",       test_B4_margins_odd_even},
        {"B5_offset_position",        test_B5_offset_position},
        // Group C: Cleanup
        {"C1_despeckle_single",       test_C1_despeckle_single},
        {"C2_despeckle_object",       test_C2_despeckle_object},
        {"C3_edge_cleanup_before",    test_C3_edge_cleanup_before},
        {"C4_hole_cleanup",           test_C4_hole_cleanup},
        {"C5_subimage_report_small",  test_C5_subimage_report_small},
        // Group D: Deskew
        {"D1_deskew_basic",           test_D1_deskew_basic},
        {"D2_deskew_interpolated",    test_D2_deskew_interpolated},
        // Group E: Combined
        {"E1_full_pipeline",          test_E1_full_pipeline},
        {"E2_movement_limit",         test_E2_movement_limit},
        // Group F: Alignment
        {"F1_margin_center_h",           test_F1_margin_center_h},
        {"F2_margin_center_v",           test_F2_margin_center_v},
        {"F3_margin_center_both",        test_F3_margin_center_both},
        {"F4_margin_keep_position",      test_F4_margin_keep_position},
        {"F5_resize_valign_top",         test_F5_resize_valign_top},
        {"F6_resize_valign_bottom",      test_F6_resize_valign_bottom},
        {"F7_resize_valign_proportional",test_F7_resize_valign_proportional},
        {"F8_resize_no_shrink",          test_F8_resize_no_shrink},
        {"F9_margin_to_margin_offset",   test_F9_margin_to_margin_offset},
        {"F10_resize_center_with_margins", test_F10_resize_center_with_margins},
    };

    // Run tests.
    int passed = 0, failed = 0, skipped = 0;
    auto total_t0 = std::chrono::steady_clock::now();

    for (auto& [name, fn] : tests) {
        if (!filter.empty() && name.find(filter) == std::string::npos) {
            ++skipped;
            continue;
        }

        std::cout << "[RUN ] " << name << std::flush;
        auto t0 = std::chrono::steady_clock::now();

        bool ok = false;
        try {
            ok = fn();
        } catch (const std::exception& e) {
            std::cerr << "  EXCEPTION: " << e.what() << std::endl;
        }

        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        if (ok) {
            std::cout << " ... PASS (" << ms << "ms)" << std::endl;
            ++passed;
        } else {
            std::cout << " ... FAIL (" << ms << "ms)" << std::endl;
            ++failed;
        }
    }

    auto total_t1 = std::chrono::steady_clock::now();
    auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(total_t1 - total_t0).count();

    std::cout << "\n=== Regression Results ===" << std::endl;
    std::cout << "  Passed:  " << passed << std::endl;
    std::cout << "  Failed:  " << failed << std::endl;
    if (skipped > 0) std::cout << "  Skipped: " << skipped << std::endl;
    std::cout << "  Time:    " << total_ms << "ms" << std::endl;

    return (failed > 0) ? 1 : 0;
}

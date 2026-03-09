// PPP Job Viewer — Win32 GUI for document image processing.
//
// Tabbed UI matching the legacy ELAN PPP application:
//   Tab 1: Job Setup    — file browser, selected files, batch parameters
//   Tab 2: Page Setup   — canvas size, orientation, rotation, subimage
//   Tab 3: Margin Setup — per-edge margins, alignment, verso/recto
//   Tab 4: Cleanup      — despeckle, deskew, edge/hole cleanup
//   Tab 5: Resize       — resize workflow with separate canvas/margins
//
// QC workflow: Select → Configure → Process → Good/Exception → Investigate → loop

#include "ppp/core/bmp.h"
#include "ppp/core/image.h"
#include "ppp/core/image_ops.h"
#include "ppp/core/output_writer.h"
#include "ppp/core/pdf_writer.h"
#include "ppp/core/processing_config.h"
#include "ppp/core/processing_config_io.h"
#include "ppp/core/processing_pipeline.h"
#include "ppp/core/tiff.h"
#include "ppp/core/tiff_writer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "comctl32.lib")

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace ppp::core;
namespace ops = ppp::core::ops;

// ---------------------------------------------------------------------------
// Menu / control identifiers
// ---------------------------------------------------------------------------

enum ControlId : UINT {
    // File menu
    IDM_FILE_OPEN = 1001,
    IDM_FILE_SAVE,
    IDM_FILE_EXIT,
    // Profile menu
    IDM_PROFILE_LOAD,
    IDM_PROFILE_SAVE,
    IDM_PROFILE_EDIT,
    IDM_PROFILE_RESET,
    // Process menu
    IDM_PROCESS_CURRENT,
    IDM_PROCESS_ALL,
    IDM_PROCESS_STEP_ROTATE,
    IDM_PROCESS_STEP_COLOR_DROPOUT,
    IDM_PROCESS_STEP_EDGE_CLEANUP,
    IDM_PROCESS_STEP_DESKEW,
    IDM_PROCESS_STEP_HOLE_CLEANUP,
    IDM_PROCESS_STEP_DESPECKLE,
    IDM_PROCESS_STEP_SUBIMAGE,
    IDM_PROCESS_STEP_BLANK_PAGE,
    IDM_PROCESS_STEP_MARGINS,
    IDM_PROCESS_STEP_RESIZE,
    // View menu
    IDM_VIEW_ORIGINAL,
    IDM_VIEW_PROCESSED,
    IDM_VIEW_TOGGLE,
    IDM_VIEW_ZOOM_IN,
    IDM_VIEW_ZOOM_OUT,
    IDM_VIEW_FIT,
    IDM_VIEW_ACTUAL,
    IDM_VIEW_NEXT,
    IDM_VIEW_PREV,
    IDM_VIEW_FIRST,
    IDM_VIEW_LAST,
    // Image operations
    IDM_IMAGE_ROTATE_CW,
    IDM_IMAGE_ROTATE_CCW,
    IDM_IMAGE_ROTATE_180,
    IDM_IMAGE_DELETE_PAGE,
    // Help
    IDM_HELP_ABOUT,

    // --- Layout controls ---
    IDC_TOOLBAR = 2001,
    IDC_STATUSBAR,
    IDC_IMAGE_PANEL,
    IDC_MAIN_TAB,           // Main 5-tab control (left panel)

    // --- Job Setup tab (Tab 1) ---
    IDC_JS_LOOK_IN_COMBO = 2100,
    IDC_JS_LOOK_IN_BROWSE,
    IDC_JS_UP_DIR_BTN,
    IDC_JS_LIST_VIEW_BTN,
    IDC_JS_DETAIL_VIEW_BTN,
    IDC_JS_FILE_LIST,       // ListView: files in current directory
    IDC_JS_FILE_TYPE_COMBO,
    IDC_JS_SUB_TAB,         // Sub-tab strip: Selected Files / Batch Params
    IDC_JS_SELECTED_LIST,   // ListView: selected files
    IDC_JS_ADD_BTN,
    IDC_JS_REMOVE_BTN,
    IDC_JS_CLEAR_BTN,
    IDC_JS_SAVE_LIST_BTN,
    IDC_JS_LOAD_LIST_BTN,
    IDC_JS_PROCESS_BTN,
    // Batch params sub-tab
    IDC_JS_OUTDIR_CHECK,
    IDC_JS_OUTDIR_COMBO,
    IDC_JS_OUTDIR_BROWSE,
    IDC_JS_NEWEXT_CHECK,
    IDC_JS_NEWEXT_COMBO,
    IDC_JS_CONFLICT_REPORT,
    IDC_JS_CONFLICT_OVERWRITE,
    IDC_JS_OUTPUT_TYPE_COMBO,
    IDC_JS_PDF_CHECK,

    // --- Page Setup tab (Tab 2) ---
    IDC_PS_DETECT_CHECK = 2200,
    IDC_PS_UNIT_PIXELS,
    IDC_PS_UNIT_INCHES,
    IDC_PS_UNIT_MM,
    IDC_PS_DETECTED_W,
    IDC_PS_DETECTED_H,
    IDC_PS_DPI,
    IDC_PS_SUBIMAGE_W,
    IDC_PS_SUBIMAGE_H,
    IDC_PS_KEEP_ORIGINAL,
    IDC_PS_CANVAS_W,
    IDC_PS_CANVAS_H,
    IDC_PS_CANVAS_AUTODETECT,
    IDC_PS_CANVAS_LETTER,
    IDC_PS_CANVAS_LEGAL,
    IDC_PS_CANVAS_TABLOID,
    IDC_PS_CANVAS_A4,
    IDC_PS_CANVAS_A3,
    IDC_PS_CANVAS_CUSTOM,
    IDC_PS_ORIENT_PORTRAIT,
    IDC_PS_ORIENT_LANDSCAPE,
    IDC_PS_TURN_CHECK,
    IDC_PS_TURN_CW,
    IDC_PS_TURN_CCW,
    IDC_PS_TURN_180,
    IDC_PS_KEEP_OUTSIDE,
    IDC_PS_MAX_HMOV,
    IDC_PS_MAX_VMOV,
    IDC_PS_REPORT_MOV,

    // --- Margin Setup tab (Tab 3) ---
    IDC_MS_SIMPLE_RADIO = 2300,
    IDC_MS_ODDEVEN_RADIO,
    IDC_MS_TOP_EDIT,
    IDC_MS_LEFT_EDIT,
    IDC_MS_RIGHT_EDIT,
    IDC_MS_BOTTOM_EDIT,
    IDC_MS_TOP_SET,
    IDC_MS_TOP_CHECK,
    IDC_MS_LEFT_SET,
    IDC_MS_LEFT_CHECK,
    IDC_MS_RIGHT_SET,
    IDC_MS_RIGHT_CHECK,
    IDC_MS_BOTTOM_SET,
    IDC_MS_BOTTOM_CHECK,
    IDC_MS_CENTER_H,
    IDC_MS_CENTER_V,
    IDC_MS_KEEP_H,
    IDC_MS_KEEP_V,
    IDC_MS_TO_MARGIN_H,
    IDC_MS_TO_MARGIN_V,
    IDC_MS_MIRROR_BTN,
    IDC_MS_VERSO_RECTO_TAB,

    // --- Cleanup tab (Tab 4) ---
    IDC_CL_DESPECKLE_NONE = 2400,
    IDC_CL_DESPECKLE_SINGLE,
    IDC_CL_DESPECKLE_OBJECTS,
    IDC_CL_DESPECKLE_MIN,
    IDC_CL_DESPECKLE_MAX,
    IDC_CL_DESKEW_CHECK,
    IDC_CL_DESKEW_INTERP,
    IDC_CL_DESKEW_CHARPROTECT,
    IDC_CL_DESKEW_ALG_COMBO,
    IDC_CL_DESKEW_MIN,
    IDC_CL_DESKEW_MAX,
    IDC_CL_EDGE_CHECK,
    IDC_CL_EDGE_BEFORE_DESKEW,
    IDC_CL_EDGE_AFTER_DESKEW,
    IDC_CL_EDGE_TOP,
    IDC_CL_EDGE_LEFT,
    IDC_CL_EDGE_RIGHT,
    IDC_CL_EDGE_BOTTOM,
    IDC_CL_HOLE_CHECK,
    IDC_CL_HOLE_TOP,
    IDC_CL_HOLE_LEFT,
    IDC_CL_HOLE_RIGHT,
    IDC_CL_HOLE_BOTTOM,
    IDC_CL_REPORT_SUBIMAGE,
    IDC_CL_REPORT_W,
    IDC_CL_REPORT_H,
    IDC_CL_REPORT_NOSKEW,

    // --- Resize tab (Tab 5) ---
    IDC_RS_ENABLE_CHECK = 2500,
    IDC_RS_FROM_SUBIMAGE,
    IDC_RS_FROM_FULLPAGE,
    IDC_RS_FROM_CUSTOM,
    IDC_RS_FROM_SMART,
    IDC_RS_CANVAS_W,
    IDC_RS_CANVAS_H,
    IDC_RS_TOP_EDIT,
    IDC_RS_LEFT_EDIT,
    IDC_RS_RIGHT_EDIT,
    IDC_RS_BOTTOM_EDIT,
    IDC_RS_VALIGN_TOP,
    IDC_RS_VALIGN_BOTTOM,
    IDC_RS_VALIGN_CENTER,
    IDC_RS_VALIGN_PROP,
    IDC_RS_HALIGN_CENTER,
    IDC_RS_HALIGN_PROP,
    IDC_RS_ALLOW_SHRINK,
    IDC_RS_ALLOW_INCREASE,
    IDC_RS_ANTIALIAS,

    // --- Exception List ---
    IDC_EX_LIST = 2600,
    IDC_EX_INVESTIGATE_BTN,
    IDC_EX_REPROCESS_BTN,
    IDC_EX_DELETE_BTN,
};

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring w(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()), w.data(), len);
    return w;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                   static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                        static_cast<int>(text.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// ---------------------------------------------------------------------------
// Image entry — one page of a loaded document
// ---------------------------------------------------------------------------

struct ImageEntry {
    fs::path source_path;
    std::size_t page_index{0};
    Image original;
    std::optional<Image> processed;
    std::optional<ProcessingResult> result;
    bool is_exception{false};
    std::string exception_reason;
};

// Tab indices for the main tab control.
enum TabIndex { kTabJobSetup = 0, kTabPageSetup, kTabMarginSetup, kTabCleanup, kTabResize };

// Sub-tab indices for Job Setup bottom panel.
enum JobSubTab { kJobSubSelected = 0, kJobSubBatchParams };

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

struct AppState {
    HINSTANCE hinstance{};
    HFONT hfont_ui{};       // Default UI font (Segoe UI 9pt)
    HFONT hfont_mono{};     // Monospace font for log

    // Window handles — main layout
    HWND hwnd_main{};
    HWND hwnd_toolbar{};
    HWND hwnd_statusbar{};
    HWND hwnd_image_panel{};
    HWND hwnd_main_tab{};   // Left-side 5-tab control

    // Tab page containers (one per tab, shown/hidden on tab change)
    HWND hwnd_tab_pages[5]{};

    // --- Job Setup (Tab 1) controls ---
    HWND hwnd_js_look_in{};
    HWND hwnd_js_browse{};
    HWND hwnd_js_up_dir{};
    HWND hwnd_js_list_view{};
    HWND hwnd_js_detail_view{};
    HWND hwnd_js_file_list{};
    HWND hwnd_js_file_type{};
    HWND hwnd_js_sub_tab{};
    HWND hwnd_js_selected_list{};
    HWND hwnd_js_btn_add{};
    HWND hwnd_js_btn_remove{};
    HWND hwnd_js_btn_clear{};
    HWND hwnd_js_btn_save{};
    HWND hwnd_js_btn_load{};
    HWND hwnd_js_btn_process{};
    // Batch params sub-panel
    HWND hwnd_js_batch_panel{};
    HWND hwnd_js_selected_panel{};
    bool js_detail_view{true};  // File list view mode

    // --- Page Setup (Tab 2) controls ---
    // (will be populated when Tab 2 is built)

    // --- Exception list (bottom of image panel) ---
    HWND hwnd_exception_list{};

    // Image state
    std::vector<ImageEntry> entries;
    int current_index{-1};
    bool showing_processed{false};

    // Display
    HBITMAP hbmp_display{};
    int bmp_width{0};
    int bmp_height{0};
    double zoom{1.0};
    int scroll_x{0};
    int scroll_y{0};
    bool fit_mode{true};

    // Processing
    ProcessingProfile profile;

    // File state — Job Setup
    fs::path browse_dir;     // Current "Look in" directory
    fs::path current_file;
    fs::path profile_path;
    std::vector<fs::path> selected_files;  // Files selected for batch

    // Active tab
    int active_tab{kTabJobSetup};
    int active_js_sub_tab{kJobSubSelected};
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK ImagePanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

void create_menus(HWND hwnd);
void create_toolbar(AppState& state);
void create_statusbar(AppState& state);
void create_image_panel(AppState& state);
void create_main_tabs(AppState& state);
void create_tab_job_setup(AppState& state);
void create_tab_page_setup(AppState& state);
void create_tab_margin_setup(AppState& state);
void create_tab_cleanup(AppState& state);
void create_tab_resize(AppState& state);
void switch_tab(AppState& state, int tab_index);
void switch_js_sub_tab(AppState& state, int sub_index);
void layout_children(AppState& state);
void layout_tab_job_setup(AppState& state, RECT rc);
void update_title(AppState& state);
void update_statusbar(AppState& state);
void update_display(AppState& state);
void rebuild_display_bitmap(AppState& state);
void free_display_bitmap(AppState& state);
void populate_file_list(AppState& state);
void populate_selected_list(AppState& state);
void update_exception_list(AppState& state);
void handle_file_list_dblclick(AppState& state);

void do_file_open(AppState& state);
void do_file_save(AppState& state);
void do_add_selected(AppState& state);
void do_remove_selected(AppState& state);
void do_clear_selected(AppState& state);
void do_process_current(AppState& state);
void do_process_all(AppState& state);
void do_process_step(AppState& state, const std::string& step_name);
void do_profile_load(AppState& state);
void do_profile_save(AppState& state);
void do_profile_edit(AppState& state);
void do_profile_reset(AppState& state);

Image load_image_file(const fs::path& path, std::size_t page = 0);

// Helper: set font on a control.
void set_ui_font(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

// Helper: create a static label.
HWND create_label(HWND parent, HINSTANCE inst, HFONT font,
                   const wchar_t* text, int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        x, y, w, h, parent, nullptr, inst, nullptr);
    set_ui_font(hw, font);
    return hw;
}

// Helper: create a push button.
HWND create_button(HWND parent, HINSTANCE inst, HFONT font,
                    const wchar_t* text, int x, int y, int w, int h, UINT id) {
    HWND hw = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
    set_ui_font(hw, font);
    return hw;
}

// Helper: create a checkbox.
HWND create_checkbox(HWND parent, HINSTANCE inst, HFONT font,
                      const wchar_t* text, int x, int y, int w, int h, UINT id,
                      bool checked = false) {
    HWND hw = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
    set_ui_font(hw, font);
    if (checked) SendMessageW(hw, BM_SETCHECK, BST_CHECKED, 0);
    return hw;
}

// Helper: create a radio button.
HWND create_radio(HWND parent, HINSTANCE inst, HFONT font,
                   const wchar_t* text, int x, int y, int w, int h, UINT id,
                   bool group = false, bool checked = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_TABSTOP;
    if (group) style |= WS_GROUP;
    HWND hw = CreateWindowExW(0, L"BUTTON", text, style,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
    set_ui_font(hw, font);
    if (checked) SendMessageW(hw, BM_SETCHECK, BST_CHECKED, 0);
    return hw;
}

// Helper: create a combo box.
HWND create_combo(HWND parent, HINSTANCE inst, HFONT font,
                   int x, int y, int w, int h, UINT id,
                   std::initializer_list<const wchar_t*> items, int sel = 0) {
    HWND hw = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
    set_ui_font(hw, font);
    for (auto* s : items)
        SendMessageW(hw, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(hw, CB_SETCURSEL, sel, 0);
    return hw;
}

// Helper: create a text edit.
HWND create_edit(HWND parent, HINSTANCE inst, HFONT font,
                  int x, int y, int w, int h, UINT id,
                  const wchar_t* text = L"", bool readonly = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP;
    if (readonly) style |= ES_READONLY;
    HWND hw = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, style,
        x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(id)), inst, nullptr);
    set_ui_font(hw, font);
    return hw;
}

// Helper: create a group box.
HWND create_groupbox(HWND parent, HINSTANCE inst, HFONT font,
                      const wchar_t* text, int x, int y, int w, int h) {
    HWND hw = CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        x, y, w, h, parent, nullptr, inst, nullptr);
    set_ui_font(hw, font);
    return hw;
}

// ---------------------------------------------------------------------------
// Image → HBITMAP conversion (RGB→BGR swap for GDI)
// ---------------------------------------------------------------------------

HBITMAP create_display_bitmap(HDC hdc, const Image& image) {
    if (image.empty()) return nullptr;

    const int w = image.width();
    const int h = image.height();

    switch (image.format()) {
    case PixelFormat::BW1: {
        // 1-bpp DIB with palette: index 0 = white, index 1 = black
        // (matches our convention: bit 1 = foreground/black).
        struct {
            BITMAPINFOHEADER bih;
            RGBQUAD palette[2];
        } bmi{};
        bmi.bih.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bih.biWidth = w;
        bmi.bih.biHeight = -h;  // Top-down.
        bmi.bih.biPlanes = 1;
        bmi.bih.biBitCount = 1;
        bmi.bih.biCompression = BI_RGB;
        bmi.palette[0] = {255, 255, 255, 0};  // Index 0 = white.
        bmi.palette[1] = {0, 0, 0, 0};        // Index 1 = black.

        void* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bmi),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbmp) return nullptr;

        // Copy row data.  Our stride matches Windows DWORD alignment.
        auto dib_stride = ((w + 31) / 32) * 4;
        for (int y = 0; y < h; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(bits) + y * dib_stride,
                        image.row(y), (w + 7) / 8);
        }
        return hbmp;
    }

    case PixelFormat::Gray8: {
        // 8-bpp DIB with 256-entry grayscale palette.
        struct {
            BITMAPINFOHEADER bih;
            RGBQUAD palette[256];
        } bmi{};
        bmi.bih.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bih.biWidth = w;
        bmi.bih.biHeight = -h;
        bmi.bih.biPlanes = 1;
        bmi.bih.biBitCount = 8;
        bmi.bih.biCompression = BI_RGB;
        for (int i = 0; i < 256; ++i) {
            bmi.palette[i] = {static_cast<BYTE>(i), static_cast<BYTE>(i),
                              static_cast<BYTE>(i), 0};
        }

        void* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bmi),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbmp) return nullptr;

        auto dib_stride = ((w + 3) & ~3);
        for (int y = 0; y < h; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(bits) + y * dib_stride,
                        image.row(y), w);
        }
        return hbmp;
    }

    case PixelFormat::RGB24: {
        // 24-bpp DIB — swap R and B channels.
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = w;
        bih.biHeight = -h;
        bih.biPlanes = 1;
        bih.biBitCount = 24;
        bih.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bih),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbmp) return nullptr;

        auto dib_stride = ((w * 3 + 3) & ~3);
        for (int y = 0; y < h; ++y) {
            const auto* src = image.row(y);
            auto* dst = static_cast<std::uint8_t*>(bits) + y * dib_stride;
            for (int x = 0; x < w; ++x) {
                dst[x * 3 + 0] = src[x * 3 + 2];  // B
                dst[x * 3 + 1] = src[x * 3 + 1];  // G
                dst[x * 3 + 2] = src[x * 3 + 0];  // R
            }
        }
        return hbmp;
    }

    case PixelFormat::RGBA32: {
        // 32-bpp DIB — swap R and B, keep A.
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = w;
        bih.biHeight = -h;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bih),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!hbmp) return nullptr;

        for (int y = 0; y < h; ++y) {
            const auto* src = image.row(y);
            auto* dst = static_cast<std::uint8_t*>(bits) + y * w * 4;
            for (int x = 0; x < w; ++x) {
                dst[x * 4 + 0] = src[x * 4 + 2];  // B
                dst[x * 4 + 1] = src[x * 4 + 1];  // G
                dst[x * 4 + 2] = src[x * 4 + 0];  // R
                dst[x * 4 + 3] = src[x * 4 + 3];  // A
            }
        }
        return hbmp;
    }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Image file loading
// ---------------------------------------------------------------------------

Image load_image_file(const fs::path& path, std::size_t page) {
    auto ext = to_lower(path.extension().string());
    if (ext == ".bmp") return bmp::read_bmp_file(path);
    if (ext == ".tif" || ext == ".tiff") return tiff::read_tiff_image_file(path, page);
    // Fallback: try TIFF then BMP.
    auto img = tiff::read_tiff_image_file(path);
    if (img.empty()) img = bmp::read_bmp_file(path);
    return img;
}

/// Count pages in a TIFF file.
std::size_t count_tiff_pages(const fs::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return 0;
    auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    std::vector<std::uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) return 0;

    auto structure = tiff::Structure::read(data.data(), size);
    if (!structure) return 0;
    return structure->page_count();
}

// ---------------------------------------------------------------------------
// Menu creation
// ---------------------------------------------------------------------------

void create_menus(HWND hwnd) {
    HMENU menubar = CreateMenu();

    // File menu
    HMENU file_menu = CreatePopupMenu();
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_OPEN, L"&Open Image...\tCtrl+O");
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_SAVE, L"&Save Processed...\tCtrl+S");
    AppendMenuW(file_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file_menu, MF_STRING, IDM_FILE_EXIT, L"E&xit");

    // Profile menu
    HMENU profile_menu = CreatePopupMenu();
    AppendMenuW(profile_menu, MF_STRING, IDM_PROFILE_LOAD, L"&Load Profile...");
    AppendMenuW(profile_menu, MF_STRING, IDM_PROFILE_SAVE, L"&Save Profile...");
    AppendMenuW(profile_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(profile_menu, MF_STRING, IDM_PROFILE_EDIT, L"&Edit Profile...");
    AppendMenuW(profile_menu, MF_STRING, IDM_PROFILE_RESET, L"&Reset to Defaults");

    // Process > Step submenu
    HMENU step_menu = CreatePopupMenu();
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_ROTATE, L"Rotate");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_COLOR_DROPOUT, L"Color Dropout");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_EDGE_CLEANUP, L"Edge Cleanup");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_DESKEW, L"Deskew");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_HOLE_CLEANUP, L"Hole Cleanup");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_DESPECKLE, L"Despeckle");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_SUBIMAGE, L"Detect Subimage");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_BLANK_PAGE, L"Blank Page");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_MARGINS, L"Margins");
    AppendMenuW(step_menu, MF_STRING, IDM_PROCESS_STEP_RESIZE, L"Resize");

    // Process menu
    HMENU process_menu = CreatePopupMenu();
    AppendMenuW(process_menu, MF_STRING, IDM_PROCESS_CURRENT, L"&Process Current\tF5");
    AppendMenuW(process_menu, MF_STRING, IDM_PROCESS_ALL, L"Process &All\tShift+F5");
    AppendMenuW(process_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(process_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(step_menu), L"Process &Step");

    // View menu
    HMENU view_menu = CreatePopupMenu();
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_TOGGLE, L"&Toggle Original/Processed\tSpace");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_ORIGINAL, L"Show &Original\tF6");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_PROCESSED, L"Show P&rocessed\tF7");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_ZOOM_IN, L"Zoom &In\tCtrl++");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_ZOOM_OUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_FIT, L"&Fit to Window\tCtrl+0");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_ACTUAL, L"&Actual Size\tCtrl+1");
    AppendMenuW(view_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_FIRST, L"&First Page\tHome");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_PREV, L"&Previous Page\tPage Up");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_NEXT, L"&Next Page\tPage Down");
    AppendMenuW(view_menu, MF_STRING, IDM_VIEW_LAST, L"&Last Page\tEnd");

    // Image menu
    HMENU image_menu = CreatePopupMenu();
    AppendMenuW(image_menu, MF_STRING, IDM_IMAGE_ROTATE_CW, L"Rotate &Clockwise 90\xB0\tCtrl+R");
    AppendMenuW(image_menu, MF_STRING, IDM_IMAGE_ROTATE_CCW, L"Rotate Counter-C&lockwise 90\xB0\tCtrl+L");
    AppendMenuW(image_menu, MF_STRING, IDM_IMAGE_ROTATE_180, L"Rotate &180\xB0");
    AppendMenuW(image_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(image_menu, MF_STRING, IDM_IMAGE_DELETE_PAGE, L"&Delete Page\tDel");

    // Help menu
    HMENU help_menu = CreatePopupMenu();
    AppendMenuW(help_menu, MF_STRING, IDM_HELP_ABOUT, L"&About...");

    AppendMenuW(menubar, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"&File");
    AppendMenuW(menubar, MF_POPUP, reinterpret_cast<UINT_PTR>(profile_menu), L"&Profile");
    AppendMenuW(menubar, MF_POPUP, reinterpret_cast<UINT_PTR>(process_menu), L"P&rocess");
    AppendMenuW(menubar, MF_POPUP, reinterpret_cast<UINT_PTR>(image_menu), L"&Image");
    AppendMenuW(menubar, MF_POPUP, reinterpret_cast<UINT_PTR>(view_menu), L"&View");
    AppendMenuW(menubar, MF_POPUP, reinterpret_cast<UINT_PTR>(help_menu), L"&Help");

    SetMenu(hwnd, menubar);
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void create_toolbar(AppState& state) {
    state.hwnd_toolbar = CreateWindowExW(
        0, TOOLBARCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_TOP,
        0, 0, 0, 0, state.hwnd_main,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_TOOLBAR)),
        state.hinstance, nullptr);

    SendMessageW(state.hwnd_toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);

    // Add standard bitmaps.
    TBADDBITMAP tbab{HINST_COMMCTRL, IDB_STD_SMALL_COLOR};
    SendMessageW(state.hwnd_toolbar, TB_ADDBITMAP, 0, reinterpret_cast<LPARAM>(&tbab));

    TBADDBITMAP tbab2{HINST_COMMCTRL, IDB_VIEW_SMALL_COLOR};
    auto view_offset = SendMessageW(state.hwnd_toolbar, TB_ADDBITMAP, 0,
                                     reinterpret_cast<LPARAM>(&tbab2));

    TBBUTTON buttons[] = {
        {STD_FILEOPEN,  IDM_FILE_OPEN,       TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {STD_FILESAVE,  IDM_FILE_SAVE,       TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {STD_FIND,      IDM_PROCESS_CURRENT, TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {static_cast<int>(view_offset) + VIEW_LARGEICONS, IDM_VIEW_ZOOM_IN,
            TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {static_cast<int>(view_offset) + VIEW_SMALLICONS, IDM_VIEW_ZOOM_OUT,
            TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {STD_UNDO,      IDM_VIEW_TOGGLE,     TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {0, 0, 0, BTNS_SEP, {}, 0, 0},
        {static_cast<int>(view_offset) + VIEW_SORTNAME, IDM_VIEW_PREV,
            TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
        {static_cast<int>(view_offset) + VIEW_SORTDATE, IDM_VIEW_NEXT,
            TBSTATE_ENABLED, BTNS_BUTTON, {}, 0, 0},
    };

    SendMessageW(state.hwnd_toolbar, TB_ADDBUTTONSW,
                 static_cast<WPARAM>(std::size(buttons)),
                 reinterpret_cast<LPARAM>(buttons));
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------

void create_statusbar(AppState& state) {
    state.hwnd_statusbar = CreateWindowExW(
        0, STATUSCLASSNAMEW, nullptr,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, state.hwnd_main,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STATUSBAR)),
        state.hinstance, nullptr);

    int parts[] = {200, 350, 450, 530, 600, -1};
    SendMessageW(state.hwnd_statusbar, SB_SETPARTS, 6,
                 reinterpret_cast<LPARAM>(parts));
}

void update_statusbar(AppState& state) {
    if (state.current_index < 0 || state.entries.empty()) {
        SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 0,
                     reinterpret_cast<LPARAM>(L"No image loaded"));
        for (int i = 1; i < 6; ++i)
            SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, i,
                         reinterpret_cast<LPARAM>(L""));
        return;
    }

    const auto& entry = state.entries[state.current_index];
    const auto& img = state.showing_processed && entry.processed
                          ? *entry.processed : entry.original;

    // Part 0: filename
    auto fname = entry.source_path.filename().wstring();
    SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 0,
                 reinterpret_cast<LPARAM>(fname.c_str()));

    // Part 1: dimensions
    std::wostringstream dim;
    dim << img.width() << L" x " << img.height() << L" px";
    auto dim_str = dim.str();
    SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 1,
                 reinterpret_cast<LPARAM>(dim_str.c_str()));

    // Part 2: DPI
    std::wostringstream dpi;
    dpi << static_cast<int>(img.dpi_x()) << L" x "
        << static_cast<int>(img.dpi_y()) << L" DPI";
    auto dpi_str = dpi.str();
    SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 2,
                 reinterpret_cast<LPARAM>(dpi_str.c_str()));

    // Part 3: format
    const wchar_t* fmt_str = L"?";
    switch (img.format()) {
    case PixelFormat::BW1:    fmt_str = L"BW1"; break;
    case PixelFormat::Gray8:  fmt_str = L"Gray8"; break;
    case PixelFormat::RGB24:  fmt_str = L"RGB24"; break;
    case PixelFormat::RGBA32: fmt_str = L"RGBA32"; break;
    }
    SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 3,
                 reinterpret_cast<LPARAM>(fmt_str));

    // Part 4: zoom
    std::wostringstream zoom;
    zoom << static_cast<int>(state.zoom * 100) << L"%";
    auto zoom_str = zoom.str();
    SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 4,
                 reinterpret_cast<LPARAM>(zoom_str.c_str()));

    // Part 5: page
    std::wostringstream page;
    page << L"Page " << (state.current_index + 1) << L"/" << state.entries.size();
    if (state.showing_processed && entry.processed)
        page << L" [Processed]";
    else
        page << L" [Original]";
    auto page_str = page.str();
    SendMessageW(state.hwnd_statusbar, SB_SETTEXTW, 5,
                 reinterpret_cast<LPARAM>(page_str.c_str()));
}

// ---------------------------------------------------------------------------
// Image display panel (custom child window)
// ---------------------------------------------------------------------------

constexpr wchar_t kTabPageClass[] = L"PPPTabPage";
constexpr wchar_t kImagePanelClass[] = L"PPPImagePanel";

// Tab page container: forwards WM_COMMAND and WM_NOTIFY to the top-level main window.
LRESULT CALLBACK TabPageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY: {
        // Walk up to the top-level window and forward the message.
        HWND top = hwnd;
        while (HWND parent = GetParent(top))
            top = parent;
        return SendMessageW(top, msg, wp, lp);
    }
    case WM_ERASEBKGND: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void register_tab_page_class(HINSTANCE hinstance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = TabPageProc;
    wc.hInstance = hinstance;
    wc.lpszClassName = kTabPageClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
}

void register_image_panel_class(HINSTANCE hinstance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ImagePanelProc;
    wc.hInstance = hinstance;
    wc.lpszClassName = kImagePanelClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);
}

void create_image_panel(AppState& state) {
    state.hwnd_image_panel = CreateWindowExW(
        WS_EX_CLIENTEDGE, kImagePanelClass, nullptr,
        WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL,
        0, 0, 100, 100, state.hwnd_main,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_IMAGE_PANEL)),
        state.hinstance, nullptr);
}

// ---------------------------------------------------------------------------
// Main tab control (left panel with 5 tabs)
// ---------------------------------------------------------------------------

void create_main_tabs(AppState& state) {
    // Create the tab control.
    state.hwnd_main_tab = CreateWindowExW(
        0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS,
        0, 0, 400, 600, state.hwnd_main,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_MAIN_TAB)),
        state.hinstance, nullptr);
    set_ui_font(state.hwnd_main_tab, state.hfont_ui);

    // Insert tabs.
    const wchar_t* tab_names[] = {
        L"Job Setup", L"Page Setup", L"Margin Setup", L"Cleanup", L"Resize"
    };
    for (int i = 0; i < 5; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(tab_names[i]);
        SendMessageW(state.hwnd_main_tab, TCM_INSERTITEMW, i,
                     reinterpret_cast<LPARAM>(&item));
    }

    // Create container panels for each tab (custom class that forwards WM_COMMAND).
    for (int i = 0; i < 5; ++i) {
        state.hwnd_tab_pages[i] = CreateWindowExW(
            0, kTabPageClass, nullptr,
            WS_CHILD | WS_CLIPCHILDREN,
            0, 0, 100, 100, state.hwnd_main_tab,
            nullptr, state.hinstance, nullptr);
    }

    // Build each tab's controls.
    create_tab_job_setup(state);
    create_tab_page_setup(state);
    create_tab_margin_setup(state);
    create_tab_cleanup(state);
    create_tab_resize(state);

    // Show first tab.
    switch_tab(state, kTabJobSetup);
}

void switch_tab(AppState& state, int tab_index) {
    state.active_tab = tab_index;
    for (int i = 0; i < 5; ++i) {
        ShowWindow(state.hwnd_tab_pages[i], (i == tab_index) ? SW_SHOW : SW_HIDE);
    }
    // Sync the tab control selection.
    SendMessageW(state.hwnd_main_tab, TCM_SETCURSEL, tab_index, 0);
}

// ---------------------------------------------------------------------------
// Tab 1: Job Setup
// ---------------------------------------------------------------------------

void create_tab_job_setup(AppState& state) {
    HWND parent = state.hwnd_tab_pages[kTabJobSetup];
    HINSTANCE inst = state.hinstance;
    HFONT font = state.hfont_ui;

    // All positions are relative — layout_tab_job_setup() will resize dynamically.
    // Initial creation uses placeholder positions.

    // --- Row 1: "Look in:" label + path edit + browse + up-dir + list/detail buttons ---
    create_label(parent, inst, font, L"Look in:", 4, 6, 50, 18);

    state.hwnd_js_look_in = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        56, 4, 300, 23, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_LOOK_IN_COMBO)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_look_in, font);

    state.hwnd_js_browse = create_button(parent, inst, font,
        L"\x2026", 360, 4, 24, 23, IDC_JS_LOOK_IN_BROWSE);  // Ellipsis char

    state.hwnd_js_up_dir = create_button(parent, inst, font,
        L"\x2191", 388, 4, 24, 23, IDC_JS_UP_DIR_BTN);  // Up arrow

    state.hwnd_js_list_view = CreateWindowExW(0, L"BUTTON", L"\x2630",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        416, 4, 24, 23, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_LIST_VIEW_BTN)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_list_view, font);

    state.hwnd_js_detail_view = CreateWindowExW(0, L"BUTTON", L"\x2261",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_FLAT,
        444, 4, 24, 23, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_DETAIL_VIEW_BTN)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_detail_view, font);

    // --- Row 2: File browser ListView (detail mode with Name, Size, Date columns) ---
    state.hwnd_js_file_list = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | WS_VSCROLL,
        4, 30, 460, 160, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_FILE_LIST)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_file_list, font);
    ListView_SetExtendedListViewStyle(state.hwnd_js_file_list,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    // Columns: Name, Size, Last Changed.
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 250;
    col.pszText = const_cast<wchar_t*>(L"Name");
    SendMessageW(state.hwnd_js_file_list, LVM_INSERTCOLUMNW, 0,
                 reinterpret_cast<LPARAM>(&col));
    col.cx = 80;
    col.pszText = const_cast<wchar_t*>(L"Size");
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_RIGHT;
    SendMessageW(state.hwnd_js_file_list, LVM_INSERTCOLUMNW, 1,
                 reinterpret_cast<LPARAM>(&col));
    col.cx = 120;
    col.pszText = const_cast<wchar_t*>(L"Last Changed");
    col.fmt = LVCFMT_LEFT;
    SendMessageW(state.hwnd_js_file_list, LVM_INSERTCOLUMNW, 2,
                 reinterpret_cast<LPARAM>(&col));

    // --- Row 3: File type filter ---
    create_label(parent, inst, font, L"Files of type:", 4, 194, 80, 18);
    state.hwnd_js_file_type = CreateWindowExW(
        0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        88, 192, 380, 200, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_FILE_TYPE_COMBO)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_file_type, font);
    const wchar_t* filters[] = {
        L"Image Files (*.tif;*.tiff;*.bmp)",
        L"TIFF Files (*.tif;*.tiff)",
        L"BMP Files (*.bmp)",
        L"All Files (*.*)"
    };
    for (auto* f : filters)
        SendMessageW(state.hwnd_js_file_type, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(f));
    SendMessageW(state.hwnd_js_file_type, CB_SETCURSEL, 0, 0);

    // --- Row 4: Sub-tab strip: Selected Files / Batch Parameters ---
    state.hwnd_js_sub_tab = CreateWindowExW(
        0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_BOTTOM,
        4, 220, 460, 280, parent,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_SUB_TAB)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_sub_tab, font);

    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<wchar_t*>(L"Selected Files");
    SendMessageW(state.hwnd_js_sub_tab, TCM_INSERTITEMW, 0,
                 reinterpret_cast<LPARAM>(&ti));
    ti.pszText = const_cast<wchar_t*>(L"Batch Parameters");
    SendMessageW(state.hwnd_js_sub_tab, TCM_INSERTITEMW, 1,
                 reinterpret_cast<LPARAM>(&ti));

    // --- Selected files panel: buttons LEFT, list RIGHT (legacy layout) ---
    state.hwnd_js_selected_panel = CreateWindowExW(
        0, kTabPageClass, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        2, 4, 450, 250, state.hwnd_js_sub_tab,
        nullptr, inst, nullptr);

    // Buttons stacked vertically on the left (matching legacy: 79x24/25 buttons).
    int bx = 4, btn_w = 79, btn_h = 25, btn_gap = 4;
    state.hwnd_js_btn_add = create_button(state.hwnd_js_selected_panel, inst, font,
        L"&Add", bx, 4, btn_w, btn_h, IDC_JS_ADD_BTN);
    state.hwnd_js_btn_save = create_button(state.hwnd_js_selected_panel, inst, font,
        L"&Save", bx, 4 + (btn_h + btn_gap), btn_w, btn_h, IDC_JS_SAVE_LIST_BTN);
    state.hwnd_js_btn_load = create_button(state.hwnd_js_selected_panel, inst, font,
        L"&Load", bx, 4 + 2 * (btn_h + btn_gap), btn_w, btn_h, IDC_JS_LOAD_LIST_BTN);
    state.hwnd_js_btn_clear = create_button(state.hwnd_js_selected_panel, inst, font,
        L"&Clear", bx, 4 + 3 * (btn_h + btn_gap), btn_w, btn_h, IDC_JS_CLEAR_BTN);
    // Process button at bottom of button column.
    state.hwnd_js_btn_process = create_button(state.hwnd_js_selected_panel, inst, font,
        L"Process", bx, 4 + 5 * (btn_h + btn_gap), btn_w, btn_h, IDC_JS_PROCESS_BTN);
    state.hwnd_js_btn_remove = create_button(state.hwnd_js_selected_panel, inst, font,
        L"Re&move", bx, 4 + 4 * (btn_h + btn_gap), btn_w, btn_h, IDC_JS_REMOVE_BTN);

    // Selected files ListView — to the right of the buttons.
    int list_x = bx + btn_w + 8;
    state.hwnd_js_selected_list = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | WS_VSCROLL,
        list_x, 4, 350, 220, state.hwnd_js_selected_panel,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_SELECTED_LIST)),
        inst, nullptr);
    set_ui_font(state.hwnd_js_selected_list, font);
    ListView_SetExtendedListViewStyle(state.hwnd_js_selected_list,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW scol{};
    scol.mask = LVCF_TEXT | LVCF_WIDTH;
    scol.cx = 340;
    scol.pszText = const_cast<wchar_t*>(L"File Name");
    SendMessageW(state.hwnd_js_selected_list, LVM_INSERTCOLUMNW, 0,
                 reinterpret_cast<LPARAM>(&scol));

    // --- Batch parameters panel (child of sub-tab, initially hidden) ---
    state.hwnd_js_batch_panel = CreateWindowExW(
        0, kTabPageClass, nullptr,
        WS_CHILD | WS_CLIPCHILDREN,
        2, 4, 450, 250, state.hwnd_js_sub_tab,
        nullptr, inst, nullptr);

    auto* bp = state.hwnd_js_batch_panel;
    int by = 8;

    CreateWindowExW(0, L"BUTTON", L"Save to different directory",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        8, by, 200, 18, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_OUTDIR_CHECK)),
        inst, nullptr);
    by += 24;

    create_label(bp, inst, font, L"Output dir:", 24, by + 2, 65, 16);
    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        100, by, 290, 23, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_OUTDIR_COMBO)),
        inst, nullptr);
    create_button(bp, inst, font, L"\x2026", 396, by, 24, 23, IDC_JS_OUTDIR_BROWSE);
    by += 32;

    CreateWindowExW(0, L"BUTTON", L"New raster file extension:",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        8, by, 180, 18, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_NEWEXT_CHECK)),
        inst, nullptr);
    auto hwnd_ext = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        200, by - 2, 80, 150, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_NEWEXT_COMBO)),
        inst, nullptr);
    set_ui_font(hwnd_ext, font);
    for (auto* ext : {L".tif", L".bmp", L".png"})
        SendMessageW(hwnd_ext, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ext));
    SendMessageW(hwnd_ext, CB_SETCURSEL, 0, 0);
    by += 28;

    create_label(bp, inst, font, L"If same filename already exists:", 8, by + 2, 190, 16);
    by += 20;
    CreateWindowExW(0, L"BUTTON", L"Report",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP,
        200, by, 66, 18, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_CONFLICT_REPORT)),
        inst, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Overwrite",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        280, by, 81, 18, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_CONFLICT_OVERWRITE)),
        inst, nullptr);
    CheckDlgButton(bp, IDC_JS_CONFLICT_REPORT, BST_CHECKED);
    by += 28;

    create_label(bp, inst, font, L"Raster output (grayscale, color):", 8, by + 2, 190, 16);
    auto hwnd_out = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
        200, by, 220, 150, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_OUTPUT_TYPE_COMBO)),
        inst, nullptr);
    set_ui_font(hwnd_out, font);
    const wchar_t* outputs[] = {
        L"TIFF Uncompressed (lossless)",
        L"TIFF Packed Bits (lossless)",
        L"PNG (lossless)",
        L"JPEG (lossy)"
    };
    for (auto* o : outputs)
        SendMessageW(hwnd_out, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(o));
    SendMessageW(hwnd_out, CB_SETCURSEL, 0, 0);
    by += 32;

    CreateWindowExW(0, L"BUTTON", L"Generate PDF output",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
        8, by, 180, 18, bp,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_JS_PDF_CHECK)),
        inst, nullptr);

    // Set fonts for all batch panel children.
    EnumChildWindows(bp, [](HWND hw, LPARAM lp) -> BOOL {
        set_ui_font(hw, reinterpret_cast<HFONT>(lp));
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));

    // Default sub-tab: Selected Files.
    switch_js_sub_tab(state, kJobSubSelected);

    // Initialize browse dir.
    if (state.browse_dir.empty()) {
        wchar_t buf[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, buf);
        state.browse_dir = buf;
    }
    SetWindowTextW(state.hwnd_js_look_in, state.browse_dir.wstring().c_str());
}

void switch_js_sub_tab(AppState& state, int sub_index) {
    state.active_js_sub_tab = sub_index;
    ShowWindow(state.hwnd_js_selected_panel,
               sub_index == kJobSubSelected ? SW_SHOW : SW_HIDE);
    ShowWindow(state.hwnd_js_batch_panel,
               sub_index == kJobSubBatchParams ? SW_SHOW : SW_HIDE);
    SendMessageW(state.hwnd_js_sub_tab, TCM_SETCURSEL, sub_index, 0);
}

// ---------------------------------------------------------------------------
// Tab 2: Page Setup (placeholder — will be populated)
// ---------------------------------------------------------------------------

void create_tab_page_setup(AppState& state) {
    HWND p = state.hwnd_tab_pages[kTabPageSetup];
    auto i = state.hinstance;
    auto f = state.hfont_ui;
    int y = 4;

    // --- Page content detection ---
    create_checkbox(p, i, f, L"Page content detection", 8, y, 180, 18,
                    IDC_PS_DETECT_CHECK, true);
    y += 22;

    // General unit radio buttons.
    create_label(p, i, f, L"General unit:", 8, y + 2, 75, 16);
    create_radio(p, i, f, L"Pixels", 88, y, 60, 18, IDC_PS_UNIT_PIXELS, true);
    create_radio(p, i, f, L"Inches", 152, y, 60, 18, IDC_PS_UNIT_INCHES, false, true);
    create_radio(p, i, f, L"mm", 216, y, 40, 18, IDC_PS_UNIT_MM);
    y += 24;

    // Detected image info (read-only).
    create_label(p, i, f, L"Detected image size:", 8, y + 2, 120, 16);
    create_edit(p, i, f, 132, y, 60, 22, IDC_PS_DETECTED_W, L"", true);
    create_label(p, i, f, L"\x00D7", 196, y + 2, 12, 16);  // ×
    create_edit(p, i, f, 210, y, 60, 22, IDC_PS_DETECTED_H, L"", true);
    y += 26;

    create_label(p, i, f, L"Image resolution:", 8, y + 2, 100, 16);
    create_edit(p, i, f, 132, y, 60, 22, IDC_PS_DPI, L"", true);
    create_label(p, i, f, L"DPI", 196, y + 2, 30, 16);
    y += 30;

    // --- Maximum subimage size ---
    create_groupbox(p, i, f, L"Maximum subimage size", 8, y, 160, 56);
    create_label(p, i, f, L"W:", 18, y + 18, 18, 16);
    create_edit(p, i, f, 36, y + 16, 50, 22, IDC_PS_SUBIMAGE_W);
    create_label(p, i, f, L"H:", 94, y + 18, 18, 16);
    create_edit(p, i, f, 112, y + 16, 50, 22, IDC_PS_SUBIMAGE_H);

    // --- Output canvas (right column) ---
    int cx = 178;
    create_groupbox(p, i, f, L"Output canvas", cx, y, 168, 200);
    int cy = y + 16;
    create_checkbox(p, i, f, L"Keep original image size", cx + 8, cy, 155, 18,
                    IDC_PS_KEEP_ORIGINAL);
    cy += 20;

    create_label(p, i, f, L"Set canvas size:", cx + 8, cy + 2, 90, 16);
    cy += 18;
    create_edit(p, i, f, cx + 8, cy, 50, 22, IDC_PS_CANVAS_W, L"8.5");
    create_label(p, i, f, L"\x00D7", cx + 62, cy + 2, 12, 16);
    create_edit(p, i, f, cx + 76, cy, 50, 22, IDC_PS_CANVAS_H, L"11.0");
    cy += 26;

    // Canvas presets.
    create_radio(p, i, f, L"Autodetect", cx + 8, cy, 80, 16, IDC_PS_CANVAS_AUTODETECT, true);
    cy += 16;
    create_radio(p, i, f, L"Letter 8.5\"x11\"", cx + 8, cy, 120, 16, IDC_PS_CANVAS_LETTER, false, true);
    cy += 16;
    create_radio(p, i, f, L"Legal 8.5\"x14\"", cx + 8, cy, 120, 16, IDC_PS_CANVAS_LEGAL);
    cy += 16;
    create_radio(p, i, f, L"Tabloid 11\"x17\"", cx + 8, cy, 120, 16, IDC_PS_CANVAS_TABLOID);
    cy += 16;
    create_radio(p, i, f, L"A4 210x297mm", cx + 8, cy, 120, 16, IDC_PS_CANVAS_A4);
    cy += 16;
    create_radio(p, i, f, L"A3 297x420mm", cx + 8, cy, 120, 16, IDC_PS_CANVAS_A3);
    cy += 16;
    create_radio(p, i, f, L"Custom", cx + 8, cy, 80, 16, IDC_PS_CANVAS_CUSTOM);
    cy += 20;

    // Orientation.
    create_label(p, i, f, L"Orientation:", cx + 8, cy + 2, 65, 16);
    create_radio(p, i, f, L"Portrait", cx + 76, cy, 65, 16, IDC_PS_ORIENT_PORTRAIT, true, true);
    create_radio(p, i, f, L"Landscape", cx + 142, cy - 1, 80, 18, IDC_PS_ORIENT_LANDSCAPE);

    // --- Page rotation (below subimage group) ---
    y += 62;
    create_groupbox(p, i, f, L"Page rotation", 8, y, 160, 80);
    create_checkbox(p, i, f, L"Turn image first", 18, y + 18, 130, 18, IDC_PS_TURN_CHECK);
    create_radio(p, i, f, L"+90\x00B0", 18, y + 38, 50, 16, IDC_PS_TURN_CW, true, true);
    create_radio(p, i, f, L"-90\x00B0", 72, y + 38, 50, 16, IDC_PS_TURN_CCW);
    create_radio(p, i, f, L"180\x00B0", 126, y + 38, 50, 16, IDC_PS_TURN_180);
    y += 86;

    // Keep contents outside subimage.
    create_checkbox(p, i, f, L"Keep contents outside detected subimage", 8, y, 250, 18,
                    IDC_PS_KEEP_OUTSIDE);
    y += 24;

    // --- Max subimage movement ---
    create_groupbox(p, i, f, L"Max subimage movement", 8, y, 160, 64);
    create_checkbox(p, i, f, L"Report max movement", 18, y + 16, 140, 18, IDC_PS_REPORT_MOV);
    create_label(p, i, f, L"H:", 18, y + 38, 16, 16);
    create_edit(p, i, f, 34, y + 36, 44, 22, IDC_PS_MAX_HMOV, L"4.0");
    create_label(p, i, f, L"V:", 86, y + 38, 16, 16);
    create_edit(p, i, f, 102, y + 36, 44, 22, IDC_PS_MAX_VMOV, L"6.0");
}

// ---------------------------------------------------------------------------
// Tab 3: Margin Setup
// ---------------------------------------------------------------------------

void create_tab_margin_setup(AppState& state) {
    HWND p = state.hwnd_tab_pages[kTabMarginSetup];
    auto i = state.hinstance;
    auto f = state.hfont_ui;
    int y = 4;

    // Margin type: Simple / Odd-Even.
    create_groupbox(p, i, f, L"Margins", 8, y, 330, 340);
    y += 18;

    create_radio(p, i, f, L"Simple", 18, y, 60, 18, IDC_MS_SIMPLE_RADIO, true, true);
    create_radio(p, i, f, L"Odd-Even", 84, y, 70, 18, IDC_MS_ODDEVEN_RADIO);

    // Verso/Recto sub-tab.
    auto hw_vr = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_BOTTOM,
        18, y + 22, 310, 24, p,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_MS_VERSO_RECTO_TAB)),
        i, nullptr);
    set_ui_font(hw_vr, f);
    TCITEMW vr{};
    vr.mask = TCIF_TEXT;
    vr.pszText = const_cast<wchar_t*>(L"Verso (odd)");
    SendMessageW(hw_vr, TCM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&vr));
    vr.pszText = const_cast<wchar_t*>(L"Recto (even)");
    SendMessageW(hw_vr, TCM_INSERTITEMW, 1, reinterpret_cast<LPARAM>(&vr));
    y += 50;

    // Margin edge fields with Set/Check radio.
    auto add_margin_row = [&](const wchar_t* label, int ey, UINT edit_id,
                               UINT set_id, UINT check_id) {
        create_label(p, i, f, label, 18, ey + 2, 50, 16);
        create_edit(p, i, f, 70, ey, 60, 22, edit_id, L"0.000");
        create_radio(p, i, f, L"Set", 138, ey + 2, 40, 16, set_id, true, true);
        create_radio(p, i, f, L"Check", 182, ey + 2, 55, 16, check_id);
    };

    add_margin_row(L"Top:", y, IDC_MS_TOP_EDIT, IDC_MS_TOP_SET, IDC_MS_TOP_CHECK);
    y += 26;
    add_margin_row(L"Left:", y, IDC_MS_LEFT_EDIT, IDC_MS_LEFT_SET, IDC_MS_LEFT_CHECK);
    y += 26;
    add_margin_row(L"Right:", y, IDC_MS_RIGHT_EDIT, IDC_MS_RIGHT_SET, IDC_MS_RIGHT_CHECK);
    y += 26;
    add_margin_row(L"Bottom:", y, IDC_MS_BOTTOM_EDIT, IDC_MS_BOTTOM_SET, IDC_MS_BOTTOM_CHECK);
    y += 32;

    // Mirror margins button.
    create_button(p, i, f, L"Mirror Margins", 18, y, 100, 24, IDC_MS_MIRROR_BTN);
    y += 32;

    // Alignment group.
    create_groupbox(p, i, f, L"Alignment", 18, y, 310, 70);
    y += 16;
    create_label(p, i, f, L"Horizontal:", 28, y + 2, 65, 16);
    create_radio(p, i, f, L"Keep", 98, y, 48, 16, IDC_MS_KEEP_H, true, true);
    create_radio(p, i, f, L"Center", 150, y, 55, 16, IDC_MS_CENTER_H);
    create_radio(p, i, f, L"To margin", 210, y, 75, 16, IDC_MS_TO_MARGIN_H);
    y += 18;
    create_label(p, i, f, L"Vertical:", 28, y + 2, 55, 16);
    create_radio(p, i, f, L"Keep", 98, y, 48, 16, IDC_MS_KEEP_V, true, true);
    create_radio(p, i, f, L"Center", 150, y, 55, 16, IDC_MS_CENTER_V);
    create_radio(p, i, f, L"To margin", 210, y, 75, 16, IDC_MS_TO_MARGIN_V);
}

// ---------------------------------------------------------------------------
// Tab 4: Cleanup
// ---------------------------------------------------------------------------

void create_tab_cleanup(AppState& state) {
    HWND p = state.hwnd_tab_pages[kTabCleanup];
    auto i = state.hinstance;
    auto f = state.hfont_ui;
    int y = 4;

    // --- Despeckle ---
    create_groupbox(p, i, f, L"Despeckle", 8, y, 330, 74);
    create_radio(p, i, f, L"No speckle removal", 18, y + 16, 140, 16,
                 IDC_CL_DESPECKLE_NONE, true, true);
    create_radio(p, i, f, L"Single speckle removal", 18, y + 34, 150, 16,
                 IDC_CL_DESPECKLE_SINGLE);
    create_radio(p, i, f, L"Remove objects between", 18, y + 52, 155, 16,
                 IDC_CL_DESPECKLE_OBJECTS);
    create_edit(p, i, f, 178, y + 50, 35, 22, IDC_CL_DESPECKLE_MIN, L"1");
    create_label(p, i, f, L"and", 217, y + 52, 22, 16);
    create_edit(p, i, f, 242, y + 50, 35, 22, IDC_CL_DESPECKLE_MAX, L"10");
    create_label(p, i, f, L"pixels", 281, y + 52, 40, 16);
    y += 80;

    // --- Deskew ---
    create_groupbox(p, i, f, L"Deskew", 8, y, 330, 120);
    create_checkbox(p, i, f, L"Deskew", 18, y + 16, 80, 18, IDC_CL_DESKEW_CHECK);
    create_checkbox(p, i, f, L"Interpolated rotation", 18, y + 36, 145, 18, IDC_CL_DESKEW_INTERP);
    create_checkbox(p, i, f, L"Character protection", 18, y + 56, 140, 18, IDC_CL_DESKEW_CHARPROTECT);

    create_label(p, i, f, L"Algorithm:", 18, y + 78, 60, 16);
    create_combo(p, i, f, 82, y + 76, 130, 120, IDC_CL_DESKEW_ALG_COMBO,
                 {L"Primary", L"Text then Line", L"Line then Text", L"Alternative"});

    create_label(p, i, f, L"Min:", 220, y + 78, 28, 16);
    create_edit(p, i, f, 248, y + 76, 35, 22, IDC_CL_DESKEW_MIN, L"0.05");
    create_label(p, i, f, L"Max:", 220, y + 98, 28, 16);
    create_edit(p, i, f, 248, y + 96, 35, 22, IDC_CL_DESKEW_MAX, L"3.0");
    create_label(p, i, f, L"\x00B0", 286, y + 78, 12, 16);
    create_label(p, i, f, L"\x00B0", 286, y + 98, 12, 16);
    y += 126;

    // --- Edge cleanup ---
    create_groupbox(p, i, f, L"Edge cleanup", 8, y, 160, 110);
    create_checkbox(p, i, f, L"Remove everything within", 18, y + 16, 145, 18,
                    IDC_CL_EDGE_CHECK);
    create_radio(p, i, f, L"Before deskew", 18, y + 36, 100, 16,
                 IDC_CL_EDGE_BEFORE_DESKEW, true, true);
    create_radio(p, i, f, L"After deskew", 18, y + 52, 100, 16,
                 IDC_CL_EDGE_AFTER_DESKEW);

    create_label(p, i, f, L"Top:", 18, y + 72, 28, 16);
    create_combo(p, i, f, 50, y + 70, 46, 120, IDC_CL_EDGE_TOP, {L"0.0", L"0.25", L"0.5", L"1.0"});
    create_label(p, i, f, L"Left:", 100, y + 72, 28, 16);
    create_combo(p, i, f, 128, y + 70, 28, 120, IDC_CL_EDGE_LEFT, {L"0.0", L"0.25", L"0.5"});
    create_label(p, i, f, L"Right:", 18, y + 90, 32, 16);
    create_combo(p, i, f, 50, y + 88, 46, 120, IDC_CL_EDGE_RIGHT, {L"0.0", L"0.25", L"0.5", L"1.0"});
    create_label(p, i, f, L"Bot:", 100, y + 90, 28, 16);
    create_combo(p, i, f, 128, y + 88, 28, 120, IDC_CL_EDGE_BOTTOM, {L"0.0", L"0.25", L"0.5"});

    // --- Punch hole removal (right of edge cleanup) ---
    create_groupbox(p, i, f, L"Punch hole removal", 178, y, 160, 110);
    create_checkbox(p, i, f, L"Remove punch holes", 188, y + 16, 140, 18,
                    IDC_CL_HOLE_CHECK);

    create_label(p, i, f, L"Top:", 188, y + 40, 28, 16);
    create_combo(p, i, f, 218, y + 38, 46, 120, IDC_CL_HOLE_TOP, {L"0.0", L"0.5", L"1.0", L"1.5"});
    create_label(p, i, f, L"Left:", 270, y + 40, 28, 16);
    create_combo(p, i, f, 298, y + 38, 28, 120, IDC_CL_HOLE_LEFT, {L"0.0", L"0.5", L"1.0"});
    create_label(p, i, f, L"Right:", 188, y + 58, 32, 16);
    create_combo(p, i, f, 218, y + 56, 46, 120, IDC_CL_HOLE_RIGHT, {L"0.0", L"0.5", L"1.0", L"1.5"});
    create_label(p, i, f, L"Bot:", 270, y + 58, 28, 16);
    create_combo(p, i, f, 298, y + 56, 28, 120, IDC_CL_HOLE_BOTTOM, {L"0.0", L"0.5", L"1.0"});
    y += 116;

    // --- Report in Exception List if... ---
    create_groupbox(p, i, f, L"Report in Exception List if...", 8, y, 330, 66);
    create_checkbox(p, i, f, L"Subimage is less than", 18, y + 16, 145, 18,
                    IDC_CL_REPORT_SUBIMAGE);
    create_label(p, i, f, L"W:", 168, y + 18, 18, 16);
    create_edit(p, i, f, 186, y + 16, 40, 22, IDC_CL_REPORT_W, L"20");
    create_label(p, i, f, L"H:", 232, y + 18, 18, 16);
    create_edit(p, i, f, 250, y + 16, 40, 22, IDC_CL_REPORT_H, L"20");
    create_label(p, i, f, L"pixels", 294, y + 18, 35, 16);
    create_checkbox(p, i, f, L"Unable to detect skew", 18, y + 40, 160, 18,
                    IDC_CL_REPORT_NOSKEW);
}

// ---------------------------------------------------------------------------
// Tab 5: Resize
// ---------------------------------------------------------------------------

void create_tab_resize(AppState& state) {
    HWND p = state.hwnd_tab_pages[kTabResize];
    auto i = state.hinstance;
    auto f = state.hfont_ui;
    int y = 4;

    create_checkbox(p, i, f, L"Resize subimage", 8, y, 140, 18, IDC_RS_ENABLE_CHECK);
    y += 24;

    // --- Resize from ---
    create_groupbox(p, i, f, L"Resize from", 8, y, 160, 90);
    create_radio(p, i, f, L"Subimage", 18, y + 16, 80, 16, IDC_RS_FROM_SUBIMAGE, true, true);
    create_radio(p, i, f, L"Full page", 18, y + 34, 80, 16, IDC_RS_FROM_FULLPAGE);
    create_radio(p, i, f, L"Custom area", 18, y + 52, 90, 16, IDC_RS_FROM_CUSTOM);
    create_radio(p, i, f, L"Smart", 18, y + 70, 80, 16, IDC_RS_FROM_SMART);

    // --- Resize to margins ---
    int rx = 178;
    create_groupbox(p, i, f, L"Resize to margins", rx, y, 160, 90);
    create_label(p, i, f, L"Top:", rx + 10, y + 18, 28, 16);
    create_edit(p, i, f, rx + 40, y + 16, 46, 22, IDC_RS_TOP_EDIT, L"0.000");
    create_label(p, i, f, L"Bot:", rx + 92, y + 18, 28, 16);
    create_edit(p, i, f, rx + 120, y + 16, 30, 22, IDC_RS_BOTTOM_EDIT, L"0.000");
    create_label(p, i, f, L"Left:", rx + 10, y + 42, 28, 16);
    create_edit(p, i, f, rx + 40, y + 40, 46, 22, IDC_RS_LEFT_EDIT, L"0.000");
    create_label(p, i, f, L"Right:", rx + 92, y + 42, 32, 16);
    create_edit(p, i, f, rx + 124, y + 40, 26, 22, IDC_RS_RIGHT_EDIT, L"0.000");
    y += 96;

    // --- Canvas ---
    create_groupbox(p, i, f, L"Canvas", 8, y, 160, 50);
    create_label(p, i, f, L"W:", 18, y + 18, 18, 16);
    create_edit(p, i, f, 36, y + 16, 50, 22, IDC_RS_CANVAS_W, L"8.5");
    create_label(p, i, f, L"H:", 94, y + 18, 18, 16);
    create_edit(p, i, f, 112, y + 16, 50, 22, IDC_RS_CANVAS_H, L"11.0");

    // --- Alignment ---
    create_groupbox(p, i, f, L"Vertical alignment", rx, y, 160, 50);
    create_radio(p, i, f, L"Top", rx + 10, y + 16, 36, 16, IDC_RS_VALIGN_TOP, true);
    create_radio(p, i, f, L"Ctr", rx + 48, y + 16, 36, 16, IDC_RS_VALIGN_CENTER, false, true);
    create_radio(p, i, f, L"Bot", rx + 86, y + 16, 36, 16, IDC_RS_VALIGN_BOTTOM);
    create_radio(p, i, f, L"Prop", rx + 120, y + 16, 42, 16, IDC_RS_VALIGN_PROP);

    create_groupbox(p, i, f, L"Horiz. alignment", rx, y + 50, 160, 36);
    create_radio(p, i, f, L"Center", rx + 10, y + 66, 60, 16, IDC_RS_HALIGN_CENTER, true, true);
    create_radio(p, i, f, L"Proportional", rx + 76, y + 66, 84, 16, IDC_RS_HALIGN_PROP);
    y += 92;

    // --- Settings ---
    create_checkbox(p, i, f, L"Allow image shrinking", 8, y, 160, 18, IDC_RS_ALLOW_SHRINK, true);
    y += 20;
    create_checkbox(p, i, f, L"Allow image increase", 8, y, 160, 18, IDC_RS_ALLOW_INCREASE, true);
    y += 20;
    create_checkbox(p, i, f, L"Anti-aliased shrinking", 8, y, 160, 18, IDC_RS_ANTIALIAS);
}

// ---------------------------------------------------------------------------
// File list population (Job Setup Tab 1)
// ---------------------------------------------------------------------------

void populate_file_list(AppState& state) {
    SendMessageW(state.hwnd_js_file_list, LVM_DELETEALLITEMS, 0, 0);

    if (state.browse_dir.empty() || !fs::exists(state.browse_dir)) return;

    // Determine filter from file type combo.
    int filter = static_cast<int>(
        SendMessageW(state.hwnd_js_file_type, CB_GETCURSEL, 0, 0));

    auto matches = [&](const fs::path& p) -> bool {
        auto ext = to_lower(p.extension().string());
        switch (filter) {
        case 0:  return ext == ".tif" || ext == ".tiff" || ext == ".bmp";
        case 1:  return ext == ".tif" || ext == ".tiff";
        case 2:  return ext == ".bmp";
        default: return true;
        }
    };

    // First add subdirectories (for navigation).
    std::error_code ec;
    int idx = 0;
    for (auto& entry : fs::directory_iterator(state.browse_dir, ec)) {
        if (!entry.is_directory()) continue;
        auto name = L"[" + entry.path().filename().wstring() + L"]";
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        SendMessageW(state.hwnd_js_file_list, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&item));

        // Type column.
        LVITEMW sub{};
        sub.iItem = idx;
        sub.iSubItem = 1;
        sub.mask = LVIF_TEXT;
        sub.pszText = const_cast<wchar_t*>(L"<DIR>");
        SendMessageW(state.hwnd_js_file_list, LVM_SETITEMTEXTW, idx,
                     reinterpret_cast<LPARAM>(&sub));
        ++idx;
    }

    // Then add matching files.
    for (auto& entry : fs::directory_iterator(state.browse_dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (!matches(entry.path())) continue;

        auto name = entry.path().filename().wstring();
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = idx;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        SendMessageW(state.hwnd_js_file_list, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&item));

        // Size column.
        auto size = entry.file_size(ec);
        std::wostringstream ss;
        if (size >= 1024 * 1024)
            ss << (size / (1024 * 1024)) << L" MB";
        else if (size >= 1024)
            ss << (size / 1024) << L" KB";
        else
            ss << size << L" B";
        auto size_str = ss.str();
        LVITEMW sub{};
        sub.iItem = idx;
        sub.iSubItem = 1;
        sub.mask = LVIF_TEXT;
        sub.pszText = const_cast<wchar_t*>(size_str.c_str());
        SendMessageW(state.hwnd_js_file_list, LVM_SETITEMTEXTW, idx,
                     reinterpret_cast<LPARAM>(&sub));

        // Last changed column.
        auto lwt = entry.last_write_time(ec);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            lwt - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        auto time_t = std::chrono::system_clock::to_time_t(sctp);
        struct tm tm_buf {};
        localtime_s(&tm_buf, &time_t);
        wchar_t date_buf[64];
        wcsftime(date_buf, 64, L"%Y-%m-%d %H:%M", &tm_buf);
        sub.iSubItem = 2;
        sub.pszText = date_buf;
        SendMessageW(state.hwnd_js_file_list, LVM_SETITEMTEXTW, idx,
                     reinterpret_cast<LPARAM>(&sub));
        ++idx;
    }

    // Update Look-in path display.
    SetWindowTextW(state.hwnd_js_look_in, state.browse_dir.wstring().c_str());
}

// Navigate into a directory or open a file on double-click in the file browser.
void handle_file_list_dblclick(AppState& state) {
    int sel = ListView_GetNextItem(state.hwnd_js_file_list, -1, LVNI_SELECTED);
    if (sel < 0) return;

    wchar_t buf[MAX_PATH];
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = sel;
    item.iSubItem = 0;
    item.pszText = buf;
    item.cchTextMax = MAX_PATH;
    SendMessageW(state.hwnd_js_file_list, LVM_GETITEMTEXTW, sel,
                 reinterpret_cast<LPARAM>(&item));

    std::wstring name(buf);

    // Check if it's a directory entry (wrapped in []).
    if (name.size() > 2 && name.front() == L'[' && name.back() == L']') {
        auto dir_name = name.substr(1, name.size() - 2);
        state.browse_dir = state.browse_dir / dir_name;
        populate_file_list(state);
        return;
    }

    // It's a file — load into the image viewer for preview.
    auto path = state.browse_dir / name;
    auto ext = to_lower(path.extension().string());

    state.entries.clear();
    state.current_index = -1;
    state.showing_processed = false;
    state.current_file = path;

    std::size_t pages = 1;
    if (ext == ".tif" || ext == ".tiff") {
        pages = count_tiff_pages(path);
        if (pages == 0) pages = 1;
    }

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (std::size_t p = 0; p < pages; ++p) {
        auto img = load_image_file(path, p);
        if (img.empty()) break;
        ImageEntry entry;
        entry.source_path = path;
        entry.page_index = p;
        entry.original = std::move(img);
        state.entries.push_back(std::move(entry));
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!state.entries.empty()) {
        state.current_index = 0;
        state.fit_mode = true;
        update_display(state);
    }
}

void populate_selected_list(AppState& state) {
    SendMessageW(state.hwnd_js_selected_list, LVM_DELETEALLITEMS, 0, 0);
    for (int i = 0; i < static_cast<int>(state.selected_files.size()); ++i) {
        auto name = state.selected_files[i].filename().wstring();
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        SendMessageW(state.hwnd_js_selected_list, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&item));
    }
}

void do_add_selected(AppState& state) {
    int count = ListView_GetItemCount(state.hwnd_js_file_list);
    for (int i = 0; i < count; ++i) {
        if (ListView_GetItemState(state.hwnd_js_file_list, i, LVIS_SELECTED) & LVIS_SELECTED) {
            wchar_t buf[MAX_PATH];
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.iSubItem = 0;
            item.pszText = buf;
            item.cchTextMax = MAX_PATH;
            SendMessageW(state.hwnd_js_file_list, LVM_GETITEMTEXTW, i,
                         reinterpret_cast<LPARAM>(&item));
            auto path = state.browse_dir / buf;
            // Avoid duplicates.
            bool found = false;
            for (auto& f : state.selected_files)
                if (f == path) { found = true; break; }
            if (!found) state.selected_files.push_back(path);
        }
    }
    populate_selected_list(state);
}

void do_remove_selected(AppState& state) {
    // Remove selected items from selected_files (in reverse order).
    std::vector<int> to_remove;
    int count = ListView_GetItemCount(state.hwnd_js_selected_list);
    for (int i = 0; i < count; ++i) {
        if (ListView_GetItemState(state.hwnd_js_selected_list, i, LVIS_SELECTED) & LVIS_SELECTED)
            to_remove.push_back(i);
    }
    for (auto it = to_remove.rbegin(); it != to_remove.rend(); ++it) {
        if (*it < static_cast<int>(state.selected_files.size()))
            state.selected_files.erase(state.selected_files.begin() + *it);
    }
    populate_selected_list(state);
}

void do_clear_selected(AppState& state) {
    state.selected_files.clear();
    populate_selected_list(state);
}

// ---------------------------------------------------------------------------
// Exception list
// ---------------------------------------------------------------------------

void update_exception_list(AppState& state) {
    if (!state.hwnd_exception_list) return;
    SendMessageW(state.hwnd_exception_list, LVM_DELETEALLITEMS, 0, 0);
    int idx = 0;
    for (int i = 0; i < static_cast<int>(state.entries.size()); ++i) {
        auto& e = state.entries[i];
        if (!e.is_exception) continue;
        auto name = e.source_path.filename().wstring();
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = idx;
        item.iSubItem = 0;
        item.pszText = const_cast<wchar_t*>(name.c_str());
        item.lParam = i;  // Index into entries.
        SendMessageW(state.hwnd_exception_list, LVM_INSERTITEMW, 0,
                     reinterpret_cast<LPARAM>(&item));

        auto reason = widen(e.exception_reason);
        item.iSubItem = 1;
        item.mask = LVIF_TEXT;
        item.pszText = const_cast<wchar_t*>(reason.c_str());
        SendMessageW(state.hwnd_exception_list, LVM_SETITEMTEXTW, idx,
                     reinterpret_cast<LPARAM>(&item));
        ++idx;
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void layout_children(AppState& state) {
    if (!state.hwnd_main) return;

    RECT rc;
    GetClientRect(state.hwnd_main, &rc);
    int client_w = rc.right - rc.left;
    int client_h = rc.bottom - rc.top;

    // Toolbar auto-sizes.
    SendMessageW(state.hwnd_toolbar, TB_AUTOSIZE, 0, 0);
    RECT tb_rect;
    GetWindowRect(state.hwnd_toolbar, &tb_rect);
    int toolbar_h = tb_rect.bottom - tb_rect.top;

    // Statusbar auto-sizes.
    SendMessageW(state.hwnd_statusbar, WM_SIZE, 0, 0);
    RECT sb_rect;
    GetWindowRect(state.hwnd_statusbar, &sb_rect);
    int statusbar_h = sb_rect.bottom - sb_rect.top;

    int avail_h = client_h - toolbar_h - statusbar_h;

    // Layout: left panel (tabs) | right panel (image viewer)
    int tab_width = 350;
    if (client_w < 700) tab_width = client_w / 2;
    int image_width = client_w - tab_width;

    // Tab control fills the left side.
    MoveWindow(state.hwnd_main_tab, 0, toolbar_h, tab_width, avail_h, TRUE);

    // Get the tab display area (inside the tab control, below the tab strip).
    RECT tab_rc;
    GetClientRect(state.hwnd_main_tab, &tab_rc);
    SendMessageW(state.hwnd_main_tab, TCM_ADJUSTRECT, FALSE,
                 reinterpret_cast<LPARAM>(&tab_rc));

    int tab_content_x = tab_rc.left;
    int tab_content_y = tab_rc.top;
    int tab_content_w = tab_rc.right - tab_rc.left;
    int tab_content_h = tab_rc.bottom - tab_rc.top;

    // Position all tab pages in the content area.
    for (int i = 0; i < 5; ++i) {
        MoveWindow(state.hwnd_tab_pages[i],
                   tab_content_x, tab_content_y,
                   tab_content_w, tab_content_h, TRUE);
    }

    // Layout Job Setup tab contents dynamically.
    RECT js_rc = {0, 0, tab_content_w, tab_content_h};
    layout_tab_job_setup(state, js_rc);

    // Image panel fills the right side.
    // If exception list exists, split vertically: image on top, exceptions on bottom.
    int exception_h = 0;
    if (state.hwnd_exception_list && IsWindowVisible(state.hwnd_exception_list))
        exception_h = 140;

    MoveWindow(state.hwnd_image_panel, tab_width, toolbar_h,
               image_width, avail_h - exception_h, TRUE);

    if (state.hwnd_exception_list) {
        MoveWindow(state.hwnd_exception_list, tab_width,
                   toolbar_h + avail_h - exception_h,
                   image_width, exception_h, TRUE);
    }
}

void layout_tab_job_setup(AppState& state, RECT rc) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 50 || h < 50) return;

    int margin = 4;
    int row_h = 23;
    int y = margin;

    // --- Row 1: Look-in bar ---
    // "Look in:" label is at fixed position (not moved).
    int lbl_w = 52;
    int btn_size = 24;
    int n_btns = 4;  // browse, up, list, detail
    int btns_total = n_btns * (btn_size + 2);
    int edit_w = w - lbl_w - btns_total - margin * 2 - 4;
    if (edit_w < 60) edit_w = 60;

    MoveWindow(state.hwnd_js_look_in, lbl_w + margin, y, edit_w, row_h, TRUE);
    int bx = lbl_w + margin + edit_w + 4;
    MoveWindow(state.hwnd_js_browse, bx, y, btn_size, row_h, TRUE);     bx += btn_size + 2;
    MoveWindow(state.hwnd_js_up_dir, bx, y, btn_size, row_h, TRUE);     bx += btn_size + 2;
    MoveWindow(state.hwnd_js_list_view, bx, y, btn_size, row_h, TRUE);  bx += btn_size + 2;
    MoveWindow(state.hwnd_js_detail_view, bx, y, btn_size, row_h, TRUE);
    y += row_h + 4;

    // --- Row 2: File list (upper ~40% of remaining space) ---
    int remaining = h - y - margin;
    int file_list_h = remaining * 35 / 100;
    if (file_list_h < 80) file_list_h = 80;
    MoveWindow(state.hwnd_js_file_list, margin, y, w - margin * 2, file_list_h, TRUE);

    // Resize columns proportionally.
    int lw = w - margin * 2 - 24;  // Account for scrollbar.
    ListView_SetColumnWidth(state.hwnd_js_file_list, 0, lw * 55 / 100);
    ListView_SetColumnWidth(state.hwnd_js_file_list, 1, lw * 18 / 100);
    ListView_SetColumnWidth(state.hwnd_js_file_list, 2, lw * 27 / 100);
    y += file_list_h + 4;

    // --- Row 3: File type filter ---
    // "Files of type:" label at (4, y+2) — fixed position.
    int ft_lbl_w = 84;
    MoveWindow(state.hwnd_js_file_type, margin + ft_lbl_w, y,
               w - margin * 2 - ft_lbl_w, row_h + 150, TRUE);  // +150 for dropdown
    y += row_h + 4;

    // --- Row 4: Sub-tab (selected files / batch params) takes the rest ---
    int sub_h = h - y - margin;
    if (sub_h < 60) sub_h = 60;
    MoveWindow(state.hwnd_js_sub_tab, margin, y, w - margin * 2, sub_h, TRUE);

    // Get the sub-tab content area.
    RECT sub_rc;
    GetClientRect(state.hwnd_js_sub_tab, &sub_rc);
    SendMessageW(state.hwnd_js_sub_tab, TCM_ADJUSTRECT, FALSE,
                 reinterpret_cast<LPARAM>(&sub_rc));
    int sp_x = sub_rc.left;
    int sp_y = sub_rc.top;
    int sp_w = sub_rc.right - sub_rc.left;
    int sp_h = sub_rc.bottom - sub_rc.top;

    MoveWindow(state.hwnd_js_selected_panel, sp_x, sp_y, sp_w, sp_h, TRUE);
    MoveWindow(state.hwnd_js_batch_panel, sp_x, sp_y, sp_w, sp_h, TRUE);

    // --- Selected files panel layout: buttons LEFT, list RIGHT ---
    int btn_w = 79;
    int btn_h2 = 25;
    int btn_gap = 4;
    int btn_col_w = btn_w + 8;
    int list_x = btn_col_w;
    int list_w = sp_w - list_x - 4;
    if (list_w < 50) list_w = 50;

    MoveWindow(state.hwnd_js_selected_list, list_x, 4, list_w, sp_h - 8, TRUE);
    ListView_SetColumnWidth(state.hwnd_js_selected_list, 0, list_w - 8);

    // Stack buttons vertically.
    int by2 = 4;
    MoveWindow(state.hwnd_js_btn_add, 4, by2, btn_w, btn_h2, TRUE);       by2 += btn_h2 + btn_gap;
    MoveWindow(state.hwnd_js_btn_save, 4, by2, btn_w, btn_h2, TRUE);      by2 += btn_h2 + btn_gap;
    MoveWindow(state.hwnd_js_btn_load, 4, by2, btn_w, btn_h2, TRUE);      by2 += btn_h2 + btn_gap;
    MoveWindow(state.hwnd_js_btn_clear, 4, by2, btn_w, btn_h2, TRUE);     by2 += btn_h2 + btn_gap;
    MoveWindow(state.hwnd_js_btn_remove, 4, by2, btn_w, btn_h2, TRUE);    by2 += btn_h2 + btn_gap * 3;
    MoveWindow(state.hwnd_js_btn_process, 4, by2, btn_w, btn_h2, TRUE);
}

// ---------------------------------------------------------------------------
// Display update
// ---------------------------------------------------------------------------

void free_display_bitmap(AppState& state) {
    if (state.hbmp_display) {
        DeleteObject(state.hbmp_display);
        state.hbmp_display = nullptr;
    }
    state.bmp_width = 0;
    state.bmp_height = 0;
}

void rebuild_display_bitmap(AppState& state) {
    free_display_bitmap(state);

    if (state.current_index < 0 || state.entries.empty()) return;

    const auto& entry = state.entries[state.current_index];
    const auto& img = state.showing_processed && entry.processed
                          ? *entry.processed : entry.original;

    if (img.empty()) return;

    HDC hdc = GetDC(state.hwnd_image_panel);
    state.hbmp_display = create_display_bitmap(hdc, img);
    state.bmp_width = img.width();
    state.bmp_height = img.height();
    ReleaseDC(state.hwnd_image_panel, hdc);
}

void update_display(AppState& state) {
    rebuild_display_bitmap(state);

    // Auto-fit if in fit mode.
    if (state.fit_mode && state.bmp_width > 0 && state.bmp_height > 0) {
        RECT rc;
        GetClientRect(state.hwnd_image_panel, &rc);
        int panel_w = rc.right - rc.left;
        int panel_h = rc.bottom - rc.top;
        if (panel_w > 0 && panel_h > 0) {
            double zx = static_cast<double>(panel_w) / state.bmp_width;
            double zy = static_cast<double>(panel_h) / state.bmp_height;
            state.zoom = std::min(zx, zy);
            if (state.zoom > 4.0) state.zoom = 4.0;
        }
    }

    state.scroll_x = 0;
    state.scroll_y = 0;

    InvalidateRect(state.hwnd_image_panel, nullptr, TRUE);
    update_statusbar(state);
    update_title(state);
    update_exception_list(state);
}

void update_title(AppState& state) {
    std::wstring title = L"PPP Job Viewer";
    if (!state.current_file.empty()) {
        title += L" - " + state.current_file.filename().wstring();
    }
    if (state.showing_processed) {
        title += L" [Processed]";
    }
    SetWindowTextW(state.hwnd_main, title.c_str());
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

void do_file_open(AppState& state) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = state.hwnd_main;
    ofn.lpstrFilter = L"Image Files\0*.tif;*.tiff;*.bmp\0"
                      L"TIFF Files\0*.tif;*.tiff\0"
                      L"BMP Files\0*.bmp\0"
                      L"All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = L"Open Image";

    if (!GetOpenFileNameW(&ofn)) return;

    fs::path path(filename);
    auto ext = to_lower(path.extension().string());

    state.entries.clear();
    state.current_index = -1;
    state.showing_processed = false;
    state.current_file = path;

    // Check for multi-page TIFF.
    std::size_t pages = 1;
    if (ext == ".tif" || ext == ".tiff") {
        pages = count_tiff_pages(path);
        if (pages == 0) pages = 1;
    }

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    for (std::size_t p = 0; p < pages; ++p) {
        auto img = load_image_file(path, p);
        if (img.empty() && p == 0) {
            MessageBoxW(state.hwnd_main,
                        L"Failed to load image file.",
                        L"Error", MB_ICONERROR | MB_OK);
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return;
        }
        if (img.empty()) break;

        ImageEntry entry;
        entry.source_path = path;
        entry.page_index = p;
        entry.original = std::move(img);
        state.entries.push_back(std::move(entry));
    }

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!state.entries.empty()) {
        state.current_index = 0;
        state.fit_mode = true;
        update_display(state);
    }
}

void do_file_save(AppState& state) {
    if (state.current_index < 0) {
        MessageBoxW(state.hwnd_main, L"No image to save.", L"Info", MB_OK);
        return;
    }

    const auto& entry = state.entries[state.current_index];
    const auto& img = entry.processed ? *entry.processed : entry.original;

    wchar_t filename[MAX_PATH] = {};
    auto default_name = entry.source_path.stem().wstring() + L"_processed.tif";
    wcsncpy_s(filename, default_name.c_str(), MAX_PATH - 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = state.hwnd_main;
    ofn.lpstrFilter = L"TIFF Files\0*.tif;*.tiff\0"
                      L"BMP Files\0*.bmp\0"
                      L"PDF Files\0*.pdf\0"
                      L"All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrTitle = L"Save Processed Image";
    ofn.lpstrDefExt = L"tif";

    if (!GetSaveFileNameW(&ofn)) return;

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto result = output::write_output_to(img, fs::path(filename), state.profile.output);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!result.success) {
        auto msg = L"Failed to save: " + widen(result.error);
        MessageBoxW(state.hwnd_main, msg.c_str(), L"Error", MB_ICONERROR | MB_OK);
    }
}

// ---------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------

void do_process_current(AppState& state) {
    if (state.current_index < 0) {
        MessageBoxW(state.hwnd_main, L"No image loaded.", L"Info", MB_OK);
        return;
    }

    auto& entry = state.entries[state.current_index];

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto result = run_pipeline(entry.original, state.profile, entry.page_index);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!result.success) {
        auto msg = L"Processing failed: " + widen(result.error);
        MessageBoxW(state.hwnd_main, msg.c_str(), L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    entry.processed = std::move(result.image);
    entry.result = std::move(result);
    state.showing_processed = true;
    update_display(state);
}

void do_process_all(AppState& state) {
    // If we have selected files but no entries loaded, load them first.
    if (state.entries.empty() && !state.selected_files.empty()) {
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        for (auto& path : state.selected_files) {
            auto ext = to_lower(path.extension().string());
            std::size_t pages = 1;
            if (ext == ".tif" || ext == ".tiff") {
                pages = count_tiff_pages(path);
                if (pages == 0) pages = 1;
            }
            for (std::size_t p = 0; p < pages; ++p) {
                auto img = load_image_file(path, p);
                if (img.empty()) break;
                ImageEntry entry;
                entry.source_path = path;
                entry.page_index = p;
                entry.original = std::move(img);
                state.entries.push_back(std::move(entry));
            }
        }
        if (!state.entries.empty()) state.current_index = 0;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    if (state.entries.empty()) {
        MessageBoxW(state.hwnd_main, L"No images loaded.", L"Info", MB_OK);
        return;
    }

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    int succeeded = 0, failed = 0;
    for (std::size_t i = 0; i < state.entries.size(); ++i) {
        auto& entry = state.entries[i];
        auto result = run_pipeline(entry.original, state.profile, entry.page_index);
        if (result.success) {
            entry.processed = std::move(result.image);
            entry.result = std::move(result);
            entry.is_exception = false;
            entry.exception_reason.clear();
            ++succeeded;
        } else {
            entry.is_exception = true;
            entry.exception_reason = result.error;
            ++failed;
        }
    }

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    state.showing_processed = true;

    // Show exception list if there are exceptions.
    bool has_exceptions = failed > 0;
    ShowWindow(state.hwnd_exception_list, has_exceptions ? SW_SHOW : SW_HIDE);
    layout_children(state);
    update_display(state);

    std::wostringstream msg;
    msg << L"Processed " << succeeded << L" of " << state.entries.size()
        << L" pages.";
    if (failed > 0) msg << L"\n" << failed << L" exceptions.";
    MessageBoxW(state.hwnd_main, msg.str().c_str(), L"Batch Complete", MB_OK);
}

void do_process_step(AppState& state, const std::string& step_name) {
    if (state.current_index < 0) {
        MessageBoxW(state.hwnd_main, L"No image loaded.", L"Info", MB_OK);
        return;
    }

    auto& entry = state.entries[state.current_index];

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto result = run_step(entry.original, state.profile, step_name,
                            entry.page_index);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    if (!result.success) {
        auto msg = L"Step failed: " + widen(result.error);
        MessageBoxW(state.hwnd_main, msg.c_str(), L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    entry.processed = std::move(result.image);
    entry.result = std::move(result);
    state.showing_processed = true;
    update_display(state);
}

// ---------------------------------------------------------------------------
// Profile operations
// ---------------------------------------------------------------------------

void do_profile_load(AppState& state) {
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = state.hwnd_main;
    ofn.lpstrFilter = L"JSON Profile\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST;
    ofn.lpstrTitle = L"Load Processing Profile";

    if (!GetOpenFileNameW(&ofn)) return;

    auto loaded = read_processing_profile(fs::path(filename));
    if (!loaded) {
        MessageBoxW(state.hwnd_main, L"Failed to load profile.",
                    L"Error", MB_ICONERROR | MB_OK);
        return;
    }

    state.profile = std::move(*loaded);
    state.profile_path = fs::path(filename);

    auto msg = L"Loaded profile: " + widen(state.profile.name);
    MessageBoxW(state.hwnd_main, msg.c_str(), L"Profile Loaded", MB_OK);
}

void do_profile_save(AppState& state) {
    wchar_t filename[MAX_PATH] = {};
    if (!state.profile.name.empty()) {
        auto default_name = widen(state.profile.name) + L".json";
        wcsncpy_s(filename, default_name.c_str(), MAX_PATH - 1);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = state.hwnd_main;
    ofn.lpstrFilter = L"JSON Profile\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    ofn.lpstrTitle = L"Save Processing Profile";
    ofn.lpstrDefExt = L"json";

    if (!GetSaveFileNameW(&ofn)) return;

    if (!write_processing_profile(state.profile, fs::path(filename))) {
        MessageBoxW(state.hwnd_main, L"Failed to save profile.",
                    L"Error", MB_ICONERROR | MB_OK);
    }
}

void do_profile_reset(AppState& state) {
    if (MessageBoxW(state.hwnd_main,
                    L"Reset all profile settings to defaults?",
                    L"Confirm Reset",
                    MB_YESNO | MB_ICONQUESTION) == IDYES) {
        state.profile = ProcessingProfile{};
        state.profile_path.clear();
        MessageBoxW(state.hwnd_main, L"Profile reset to defaults.",
                    L"Profile", MB_OK);
    }
}

// ---------------------------------------------------------------------------
// Profile edit dialog
// ---------------------------------------------------------------------------

struct ProfileDlgState {
    ProcessingProfile* profile;
    bool ok{false};
};

INT_PTR CALLBACK ProfileDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* pds = reinterpret_cast<ProfileDlgState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_INITDIALOG: {
        pds = reinterpret_cast<ProfileDlgState*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pds));

        auto& p = *pds->profile;

        // --- Populate controls ---
        // Name
        SetDlgItemTextW(hwnd, 101, widen(p.name).c_str());

        // Rotation combo (102)
        SendDlgItemMessageW(hwnd, 102, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
        SendDlgItemMessageW(hwnd, 102, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CW 90"));
        SendDlgItemMessageW(hwnd, 102, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CCW 90"));
        SendDlgItemMessageW(hwnd, 102, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"180"));
        SendDlgItemMessageW(hwnd, 102, CB_SETCURSEL, static_cast<int>(p.rotation), 0);

        // Canvas preset combo (103)
        const wchar_t* presets[] = {L"Autodetect", L"Letter", L"Legal",
                                     L"Tabloid", L"A4", L"A3", L"Custom"};
        for (auto* s : presets)
            SendDlgItemMessageW(hwnd, 103, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        SendDlgItemMessageW(hwnd, 103, CB_SETCURSEL, static_cast<int>(p.canvas.preset), 0);

        // Orientation combo (104)
        SendDlgItemMessageW(hwnd, 104, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Portrait"));
        SendDlgItemMessageW(hwnd, 104, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Landscape"));
        SendDlgItemMessageW(hwnd, 104, CB_SETCURSEL, static_cast<int>(p.canvas.orientation), 0);

        // Checkboxes
        CheckDlgButton(hwnd, 110, p.deskew.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 111, p.position_image ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 112, p.blank_page.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 113, p.edge_cleanup.enabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hwnd, 114, p.hole_cleanup.enabled ? BST_CHECKED : BST_UNCHECKED);

        // Despeckle combo (120)
        SendDlgItemMessageW(hwnd, 120, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
        SendDlgItemMessageW(hwnd, 120, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Single Pixel"));
        SendDlgItemMessageW(hwnd, 120, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Object"));
        SendDlgItemMessageW(hwnd, 120, CB_SETCURSEL, static_cast<int>(p.despeckle.mode), 0);

        // Color dropout combo (121)
        SendDlgItemMessageW(hwnd, 121, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
        SendDlgItemMessageW(hwnd, 121, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Red"));
        SendDlgItemMessageW(hwnd, 121, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Green"));
        SendDlgItemMessageW(hwnd, 121, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Blue"));
        SendDlgItemMessageW(hwnd, 121, CB_SETCURSEL, static_cast<int>(p.color_dropout.color), 0);

        // Margin values (130-133: top, left, right, bottom)
        auto set_margin = [&](int id, const MarginEdge& m) {
            wchar_t buf[32];
            swprintf_s(buf, L"%.3f", m.distance.value);
            SetDlgItemTextW(hwnd, id, buf);
        };
        set_margin(130, p.margins[0].top);
        set_margin(131, p.margins[0].left);
        set_margin(132, p.margins[0].right);
        set_margin(133, p.margins[0].bottom);

        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            auto& p = *pds->profile;

            // Read back values.
            wchar_t buf[256];
            GetDlgItemTextW(hwnd, 101, buf, 256);
            p.name = narrow(buf);

            p.rotation = static_cast<Rotation>(
                SendDlgItemMessageW(hwnd, 102, CB_GETCURSEL, 0, 0));
            p.canvas.preset = static_cast<CanvasPreset>(
                SendDlgItemMessageW(hwnd, 103, CB_GETCURSEL, 0, 0));
            p.canvas.orientation = static_cast<Orientation>(
                SendDlgItemMessageW(hwnd, 104, CB_GETCURSEL, 0, 0));

            p.deskew.enabled = IsDlgButtonChecked(hwnd, 110) == BST_CHECKED;
            p.position_image = IsDlgButtonChecked(hwnd, 111) == BST_CHECKED;
            p.blank_page.enabled = IsDlgButtonChecked(hwnd, 112) == BST_CHECKED;
            p.edge_cleanup.enabled = IsDlgButtonChecked(hwnd, 113) == BST_CHECKED;
            p.hole_cleanup.enabled = IsDlgButtonChecked(hwnd, 114) == BST_CHECKED;

            p.despeckle.mode = static_cast<DespeckleMode>(
                SendDlgItemMessageW(hwnd, 120, CB_GETCURSEL, 0, 0));

            auto dc_sel = SendDlgItemMessageW(hwnd, 121, CB_GETCURSEL, 0, 0);
            p.color_dropout.color = static_cast<DropoutColor>(dc_sel);
            p.color_dropout.enabled = (dc_sel > 0);

            // Read margins.
            auto read_margin = [&](int id, MarginEdge& m) {
                wchar_t b[64];
                GetDlgItemTextW(hwnd, id, b, 64);
                m.distance.value = _wtof(b);
            };
            read_margin(130, p.margins[0].top);
            read_margin(131, p.margins[0].left);
            read_margin(132, p.margins[0].right);
            read_margin(133, p.margins[0].bottom);

            pds->ok = true;
            EndDialog(hwnd, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// Build the profile dialog template in memory (no .rc needed).
void do_profile_edit(AppState& state) {
    // We build a modal dialog using CreateDialogIndirectParam-style template.
    // Template buffer.
    alignas(4) char dlg_buf[4096] = {};
    auto* ptr = dlg_buf;

    auto align4 = [&]() {
        auto addr = reinterpret_cast<std::uintptr_t>(ptr);
        if (addr % 4) ptr += 4 - (addr % 4);
    };

    // Dialog template header.
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(ptr);
    dlg->style = DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 0;  // Will count controls.
    dlg->x = 0; dlg->y = 0;
    dlg->cx = 310; dlg->cy = 320;
    ptr += sizeof(DLGTEMPLATE);

    // Menu (none), class (default), title.
    *reinterpret_cast<WORD*>(ptr) = 0; ptr += 2;  // No menu.
    *reinterpret_cast<WORD*>(ptr) = 0; ptr += 2;  // Default class.
    auto title = L"Edit Processing Profile";
    auto title_len = wcslen(title) + 1;
    std::memcpy(ptr, title, title_len * 2);
    ptr += title_len * 2;

    // Helper: add a control item.
    int ctrl_count = 0;
    auto add_ctrl = [&](DWORD style, short x, short y, short cx, short cy,
                        WORD id, const wchar_t* cls, const wchar_t* text) {
        align4();
        auto* item = reinterpret_cast<DLGITEMTEMPLATE*>(ptr);
        item->style = WS_CHILD | WS_VISIBLE | style;
        item->dwExtendedStyle = 0;
        item->x = x; item->y = y;
        item->cx = cx; item->cy = cy;
        item->id = id;
        ptr += sizeof(DLGITEMTEMPLATE);

        // Class.
        auto cls_len = wcslen(cls) + 1;
        std::memcpy(ptr, cls, cls_len * 2);
        ptr += cls_len * 2;

        // Title/text.
        auto txt_len = wcslen(text) + 1;
        std::memcpy(ptr, text, txt_len * 2);
        ptr += txt_len * 2;

        // Extra data.
        *reinterpret_cast<WORD*>(ptr) = 0; ptr += 2;

        ++ctrl_count;
    };

    // Layout: label + control pairs.
    short y = 8;
    short lx = 6, lw = 55;
    short cx = 65, cw = 100;
    short cx2 = 175, cw2 = 100;
    short row_h = 16;

    // Row 1: Name
    add_ctrl(0, lx, y + 2, lw, 10, 0xFFFF, L"STATIC", L"Name:");
    add_ctrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, cx, y, cw + cw2 + 10, 14, 101,
             L"EDIT", L"");
    y += row_h;

    // Row 2: Rotation | Canvas Preset
    add_ctrl(0, lx, y + 2, lw, 10, 0xFFFF, L"STATIC", L"Rotation:");
    add_ctrl(CBS_DROPDOWNLIST | WS_TABSTOP, cx, y, cw, 120, 102, L"COMBOBOX", L"");
    add_ctrl(0, cx2 - 10, y + 2, 40, 10, 0xFFFF, L"STATIC", L"Canvas:");
    add_ctrl(CBS_DROPDOWNLIST | WS_TABSTOP, cx2 + 30, y, cw2 - 30, 120, 103, L"COMBOBOX", L"");
    y += row_h;

    // Row 3: Orientation | Despeckle
    add_ctrl(0, lx, y + 2, lw, 10, 0xFFFF, L"STATIC", L"Orientation:");
    add_ctrl(CBS_DROPDOWNLIST | WS_TABSTOP, cx, y, cw, 80, 104, L"COMBOBOX", L"");
    add_ctrl(0, cx2 - 10, y + 2, 50, 10, 0xFFFF, L"STATIC", L"Despeckle:");
    add_ctrl(CBS_DROPDOWNLIST | WS_TABSTOP, cx2 + 40, y, cw2 - 40, 120, 120, L"COMBOBOX", L"");
    y += row_h;

    // Row 4: Color dropout
    add_ctrl(0, lx, y + 2, lw, 10, 0xFFFF, L"STATIC", L"Color Drop:");
    add_ctrl(CBS_DROPDOWNLIST | WS_TABSTOP, cx, y, cw, 100, 121, L"COMBOBOX", L"");
    y += row_h + 4;

    // Group: Checkboxes
    add_ctrl(BS_GROUPBOX, lx, y, 296, 68, 0xFFFF, L"BUTTON", L"Processing Steps");
    y += 12;
    add_ctrl(BS_AUTOCHECKBOX | WS_TABSTOP, 14, y, 80, 12, 110, L"BUTTON", L"Deskew");
    add_ctrl(BS_AUTOCHECKBOX | WS_TABSTOP, 100, y, 90, 12, 111, L"BUTTON", L"Position Image");
    add_ctrl(BS_AUTOCHECKBOX | WS_TABSTOP, 200, y, 90, 12, 112, L"BUTTON", L"Blank Page Det.");
    y += 14;
    add_ctrl(BS_AUTOCHECKBOX | WS_TABSTOP, 14, y, 80, 12, 113, L"BUTTON", L"Edge Cleanup");
    add_ctrl(BS_AUTOCHECKBOX | WS_TABSTOP, 100, y, 90, 12, 114, L"BUTTON", L"Hole Cleanup");
    y += 14 + 16;

    // Margins group
    add_ctrl(BS_GROUPBOX, lx, y, 296, 68, 0xFFFF, L"BUTTON", L"Margins (inches)");
    y += 14;
    add_ctrl(0, 14, y + 2, 25, 10, 0xFFFF, L"STATIC", L"Top:");
    add_ctrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 40, y, 50, 14, 130, L"EDIT", L"");
    add_ctrl(0, 100, y + 2, 25, 10, 0xFFFF, L"STATIC", L"Left:");
    add_ctrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 128, y, 50, 14, 131, L"EDIT", L"");
    y += 16;
    add_ctrl(0, 14, y + 2, 25, 10, 0xFFFF, L"STATIC", L"Right:");
    add_ctrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 40, y, 50, 14, 132, L"EDIT", L"");
    add_ctrl(0, 100, y + 2, 30, 10, 0xFFFF, L"STATIC", L"Bottom:");
    add_ctrl(WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 128, y, 50, 14, 133, L"EDIT", L"");
    y += 16 + 16;

    // OK / Cancel buttons
    add_ctrl(BS_DEFPUSHBUTTON | WS_TABSTOP, 150, y, 50, 16, IDOK, L"BUTTON", L"OK");
    add_ctrl(BS_PUSHBUTTON | WS_TABSTOP, 210, y, 50, 16, IDCANCEL, L"BUTTON", L"Cancel");

    // Update control count.
    dlg->cdit = static_cast<WORD>(ctrl_count);

    // Adjust dialog height.
    dlg->cy = y + 24;

    ProfileDlgState pds;
    pds.profile = &state.profile;

    DialogBoxIndirectParamW(
        state.hinstance,
        reinterpret_cast<LPCDLGTEMPLATEW>(dlg_buf),
        state.hwnd_main,
        ProfileDlgProc,
        reinterpret_cast<LPARAM>(&pds));
}

// ---------------------------------------------------------------------------
// Image panel window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK ImagePanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        if (state && state->hbmp_display) {
            HDC mem_dc = CreateCompatibleDC(hdc);
            auto old_bmp = SelectObject(mem_dc, state->hbmp_display);

            RECT rc;
            GetClientRect(hwnd, &rc);
            int panel_w = rc.right - rc.left;
            int panel_h = rc.bottom - rc.top;

            int dst_w = static_cast<int>(state->bmp_width * state->zoom);
            int dst_h = static_cast<int>(state->bmp_height * state->zoom);

            // Center if smaller than panel.
            int offset_x = (dst_w < panel_w) ? (panel_w - dst_w) / 2 : -state->scroll_x;
            int offset_y = (dst_h < panel_h) ? (panel_h - dst_h) / 2 : -state->scroll_y;

            // Fill background.
            HBRUSH bg = CreateSolidBrush(RGB(64, 64, 64));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);
            StretchBlt(hdc, offset_x, offset_y, dst_w, dst_h,
                       mem_dc, 0, 0, state->bmp_width, state->bmp_height, SRCCOPY);

            SelectObject(mem_dc, old_bmp);
            DeleteDC(mem_dc);

            // Update scroll info.
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            si.nMin = 0;
            si.nMax = dst_w;
            si.nPage = panel_w;
            si.nPos = state->scroll_x;
            SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);

            si.nMax = dst_h;
            si.nPage = panel_h;
            si.nPos = state->scroll_y;
            SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        } else {
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH bg = CreateSolidBrush(RGB(64, 64, 64));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(180, 180, 180));
            auto msg_text = L"Open an image file (Ctrl+O)";
            DrawTextW(hdc, msg_text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_HSCROLL: {
        if (!state) break;
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, SB_HORZ, &si);
        int old_pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LEFT:       si.nPos = si.nMin; break;
        case SB_RIGHT:      si.nPos = si.nMax; break;
        case SB_LINELEFT:   si.nPos -= 20; break;
        case SB_LINERIGHT:  si.nPos += 20; break;
        case SB_PAGELEFT:   si.nPos -= si.nPage; break;
        case SB_PAGERIGHT:  si.nPos += si.nPage; break;
        case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        si.nPos = std::clamp(si.nPos, si.nMin,
                              std::max(si.nMin, si.nMax - static_cast<int>(si.nPage)));
        state->scroll_x = si.nPos;
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_HORZ, &si, TRUE);
        if (si.nPos != old_pos) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_VSCROLL: {
        if (!state) break;
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &si);
        int old_pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_TOP:        si.nPos = si.nMin; break;
        case SB_BOTTOM:     si.nPos = si.nMax; break;
        case SB_LINEUP:     si.nPos -= 20; break;
        case SB_LINEDOWN:   si.nPos += 20; break;
        case SB_PAGEUP:     si.nPos -= si.nPage; break;
        case SB_PAGEDOWN:   si.nPos += si.nPage; break;
        case SB_THUMBTRACK: si.nPos = si.nTrackPos; break;
        }
        si.nPos = std::clamp(si.nPos, si.nMin,
                              std::max(si.nMin, si.nMax - static_cast<int>(si.nPage)));
        state->scroll_y = si.nPos;
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        if (si.nPos != old_pos) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (!state) break;
        auto delta = GET_WHEEL_DELTA_WPARAM(wp);
        auto keys = GET_KEYSTATE_WPARAM(wp);

        if (keys & MK_CONTROL) {
            // Ctrl+wheel = zoom.
            if (delta > 0 && state->zoom < 8.0) {
                state->zoom *= 1.25;
                state->fit_mode = false;
            } else if (delta < 0 && state->zoom > 0.05) {
                state->zoom /= 1.25;
                state->fit_mode = false;
            }
            update_statusbar(*state);
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            // Vertical scroll.
            state->scroll_y -= delta / 2;
            state->scroll_y = std::max(0, state->scroll_y);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Main window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        state = reinterpret_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->hwnd_main = hwnd;

        create_menus(hwnd);
        create_toolbar(*state);
        create_statusbar(*state);
        create_image_panel(*state);
        create_main_tabs(*state);

        // Exception list (bottom of image area, initially hidden).
        state->hwnd_exception_list = CreateWindowExW(
            WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
            WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
            0, 0, 100, 100, state->hwnd_main,
            reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_EX_LIST)),
            state->hinstance, nullptr);
        set_ui_font(state->hwnd_exception_list, state->hfont_ui);
        ListView_SetExtendedListViewStyle(state->hwnd_exception_list,
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = 200;
            col.pszText = const_cast<wchar_t*>(L"File");
            SendMessageW(state->hwnd_exception_list, LVM_INSERTCOLUMNW, 0,
                         reinterpret_cast<LPARAM>(&col));
            col.cx = 300;
            col.pszText = const_cast<wchar_t*>(L"Exception");
            SendMessageW(state->hwnd_exception_list, LVM_INSERTCOLUMNW, 1,
                         reinterpret_cast<LPARAM>(&col));
        }

        update_statusbar(*state);
        populate_file_list(*state);

        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }

    case WM_SIZE:
        if (state) layout_children(*state);
        return 0;

    case WM_DROPFILES: {
        if (!state) break;
        auto hdrop = reinterpret_cast<HDROP>(wp);
        wchar_t filename[MAX_PATH];
        if (DragQueryFileW(hdrop, 0, filename, MAX_PATH)) {
            fs::path path(filename);
            auto ext = to_lower(path.extension().string());

            state->entries.clear();
            state->current_index = -1;
            state->showing_processed = false;
            state->current_file = path;

            std::size_t pages = 1;
            if (ext == ".tif" || ext == ".tiff") {
                pages = count_tiff_pages(path);
                if (pages == 0) pages = 1;
            }

            for (std::size_t p = 0; p < pages; ++p) {
                auto img = load_image_file(path, p);
                if (img.empty()) break;
                ImageEntry entry;
                entry.source_path = path;
                entry.page_index = p;
                entry.original = std::move(img);
                state->entries.push_back(std::move(entry));
            }

            if (!state->entries.empty()) {
                state->current_index = 0;
                state->fit_mode = true;
                update_display(*state);
            }
        }
        DragFinish(hdrop);
        return 0;
    }

    case WM_NOTIFY: {
        if (!state) break;
        auto* hdr = reinterpret_cast<NMHDR*>(lp);

        // Toolbar tooltips.
        if (hdr->code == TBN_GETINFOTIPW) {
            auto* tip = reinterpret_cast<NMTBGETINFOTIPW*>(lp);
            const wchar_t* text = nullptr;
            switch (tip->iItem) {
            case IDM_FILE_OPEN:       text = L"Open Image (Ctrl+O)"; break;
            case IDM_FILE_SAVE:       text = L"Save Processed (Ctrl+S)"; break;
            case IDM_PROCESS_CURRENT: text = L"Process Current Page (F5)"; break;
            case IDM_VIEW_ZOOM_IN:    text = L"Zoom In (Ctrl++)"; break;
            case IDM_VIEW_ZOOM_OUT:   text = L"Zoom Out (Ctrl+-)"; break;
            case IDM_VIEW_TOGGLE:     text = L"Toggle Original/Processed (Space)"; break;
            case IDM_VIEW_PREV:       text = L"Previous Page (Page Up)"; break;
            case IDM_VIEW_NEXT:       text = L"Next Page (Page Down)"; break;
            }
            if (text) {
                wcsncpy_s(tip->pszText, static_cast<std::size_t>(tip->cchTextMax),
                           text, _TRUNCATE);
            }
        }

        // Main tab change.
        if (hdr->hwndFrom == state->hwnd_main_tab && hdr->code == TCN_SELCHANGE) {
            int sel = static_cast<int>(
                SendMessageW(state->hwnd_main_tab, TCM_GETCURSEL, 0, 0));
            switch_tab(*state, sel);
        }

        // Job Setup sub-tab change.
        if (hdr->hwndFrom == state->hwnd_js_sub_tab && hdr->code == TCN_SELCHANGE) {
            int sel = static_cast<int>(
                SendMessageW(state->hwnd_js_sub_tab, TCM_GETCURSEL, 0, 0));
            switch_js_sub_tab(*state, sel);
        }

        // Double-click on file list → navigate dir or preview image.
        if (hdr->hwndFrom == state->hwnd_js_file_list && hdr->code == NM_DBLCLK) {
            handle_file_list_dblclick(*state);
        }

        // Double-click on selected list → preview that file.
        if (hdr->hwndFrom == state->hwnd_js_selected_list && hdr->code == NM_DBLCLK) {
            int sel = ListView_GetNextItem(state->hwnd_js_selected_list, -1, LVNI_SELECTED);
            if (sel >= 0 && sel < static_cast<int>(state->selected_files.size())) {
                auto& path = state->selected_files[sel];
                state->entries.clear();
                state->current_index = -1;
                state->showing_processed = false;
                state->current_file = path;
                auto ext = to_lower(path.extension().string());
                std::size_t pages = 1;
                if (ext == ".tif" || ext == ".tiff") {
                    pages = count_tiff_pages(path);
                    if (pages == 0) pages = 1;
                }
                SetCursor(LoadCursorW(nullptr, IDC_WAIT));
                for (std::size_t p = 0; p < pages; ++p) {
                    auto img = load_image_file(path, p);
                    if (img.empty()) break;
                    ImageEntry entry;
                    entry.source_path = path;
                    entry.page_index = p;
                    entry.original = std::move(img);
                    state->entries.push_back(std::move(entry));
                }
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                if (!state->entries.empty()) {
                    state->current_index = 0;
                    state->fit_mode = true;
                    update_display(*state);
                }
            }
        }

        return 0;
    }

    case WM_COMMAND: {
        if (!state) break;
        auto id = LOWORD(wp);

        switch (id) {
        case IDM_FILE_OPEN:  do_file_open(*state); break;
        case IDM_FILE_SAVE:  do_file_save(*state); break;
        case IDM_FILE_EXIT:  DestroyWindow(hwnd); break;

        case IDM_PROFILE_LOAD:  do_profile_load(*state); break;
        case IDM_PROFILE_SAVE:  do_profile_save(*state); break;
        case IDM_PROFILE_EDIT:  do_profile_edit(*state); break;
        case IDM_PROFILE_RESET: do_profile_reset(*state); break;

        case IDM_PROCESS_CURRENT: do_process_current(*state); break;
        case IDM_PROCESS_ALL:     do_process_all(*state); break;

        case IDM_PROCESS_STEP_ROTATE:        do_process_step(*state, "rotate"); break;
        case IDM_PROCESS_STEP_COLOR_DROPOUT: do_process_step(*state, "color_dropout"); break;
        case IDM_PROCESS_STEP_EDGE_CLEANUP:  do_process_step(*state, "edge_cleanup"); break;
        case IDM_PROCESS_STEP_DESKEW:        do_process_step(*state, "deskew"); break;
        case IDM_PROCESS_STEP_HOLE_CLEANUP:  do_process_step(*state, "hole_cleanup"); break;
        case IDM_PROCESS_STEP_DESPECKLE:     do_process_step(*state, "despeckle"); break;
        case IDM_PROCESS_STEP_SUBIMAGE:      do_process_step(*state, "detect_subimage"); break;
        case IDM_PROCESS_STEP_BLANK_PAGE:    do_process_step(*state, "blank_page"); break;
        case IDM_PROCESS_STEP_MARGINS:       do_process_step(*state, "margins"); break;
        case IDM_PROCESS_STEP_RESIZE:        do_process_step(*state, "resize"); break;

        case IDM_VIEW_TOGGLE:
            if (state->current_index >= 0) {
                state->showing_processed = !state->showing_processed;
                update_display(*state);
            }
            break;
        case IDM_VIEW_ORIGINAL:
            state->showing_processed = false;
            update_display(*state);
            break;
        case IDM_VIEW_PROCESSED:
            state->showing_processed = true;
            update_display(*state);
            break;
        case IDM_VIEW_ZOOM_IN:
            if (state->zoom < 8.0) {
                state->zoom *= 1.5;
                state->fit_mode = false;
                update_statusbar(*state);
                InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
            }
            break;
        case IDM_VIEW_ZOOM_OUT:
            if (state->zoom > 0.05) {
                state->zoom /= 1.5;
                state->fit_mode = false;
                update_statusbar(*state);
                InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
            }
            break;
        case IDM_VIEW_FIT:
            state->fit_mode = true;
            state->scroll_x = 0;
            state->scroll_y = 0;
            update_display(*state);
            break;
        case IDM_VIEW_ACTUAL:
            state->zoom = 1.0;
            state->fit_mode = false;
            state->scroll_x = 0;
            state->scroll_y = 0;
            update_statusbar(*state);
            InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
            break;
        case IDM_VIEW_NEXT:
            if (state->current_index < static_cast<int>(state->entries.size()) - 1) {
                ++state->current_index;
                state->fit_mode = true;
                update_display(*state);
            }
            break;
        case IDM_VIEW_PREV:
            if (state->current_index > 0) {
                --state->current_index;
                state->fit_mode = true;
                update_display(*state);
            }
            break;
        case IDM_VIEW_FIRST:
            if (!state->entries.empty() && state->current_index != 0) {
                state->current_index = 0;
                state->fit_mode = true;
                update_display(*state);
            }
            break;
        case IDM_VIEW_LAST:
            if (!state->entries.empty()) {
                int last = static_cast<int>(state->entries.size()) - 1;
                if (state->current_index != last) {
                    state->current_index = last;
                    state->fit_mode = true;
                    update_display(*state);
                }
            }
            break;

        case IDM_IMAGE_ROTATE_CW:
            if (state->current_index >= 0) {
                auto& entry = state->entries[state->current_index];
                entry.original = ops::rotate_arbitrary(entry.original, 90.0);
                if (entry.processed)
                    *entry.processed = ops::rotate_arbitrary(*entry.processed, 90.0);
                state->fit_mode = true;
                update_display(*state);
            }
            break;
        case IDM_IMAGE_ROTATE_CCW:
            if (state->current_index >= 0) {
                auto& entry = state->entries[state->current_index];
                entry.original = ops::rotate_arbitrary(entry.original, 270.0);
                if (entry.processed)
                    *entry.processed = ops::rotate_arbitrary(*entry.processed, 270.0);
                state->fit_mode = true;
                update_display(*state);
            }
            break;
        case IDM_IMAGE_ROTATE_180:
            if (state->current_index >= 0) {
                auto& entry = state->entries[state->current_index];
                entry.original = ops::rotate_arbitrary(entry.original, 180.0);
                if (entry.processed)
                    *entry.processed = ops::rotate_arbitrary(*entry.processed, 180.0);
                state->fit_mode = true;
                update_display(*state);
            }
            break;
        case IDM_IMAGE_DELETE_PAGE:
            if (state->current_index >= 0 && state->entries.size() > 1) {
                state->entries.erase(state->entries.begin() + state->current_index);
                if (state->current_index >= static_cast<int>(state->entries.size()))
                    state->current_index = static_cast<int>(state->entries.size()) - 1;
                state->fit_mode = true;
                update_display(*state);
            } else if (state->current_index >= 0 && state->entries.size() == 1) {
                state->entries.clear();
                state->current_index = -1;
                free_display_bitmap(*state);
                InvalidateRect(state->hwnd_image_panel, nullptr, TRUE);
                update_statusbar(*state);
            }
            break;

        // --- Job Setup buttons ---
        case IDC_JS_LOOK_IN_BROWSE: {
            wchar_t path[MAX_PATH] = {};
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = path;
            bi.lpszTitle = L"Select folder to browse";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            auto* pidl = SHBrowseForFolderW(&bi);
            if (pidl && SHGetPathFromIDListW(pidl, path)) {
                state->browse_dir = path;
                populate_file_list(*state);
            }
            if (pidl) CoTaskMemFree(pidl);
            break;
        }
        case IDC_JS_UP_DIR_BTN: {
            auto parent_dir = state->browse_dir.parent_path();
            if (parent_dir != state->browse_dir && !parent_dir.empty()) {
                state->browse_dir = parent_dir;
                populate_file_list(*state);
            }
            break;
        }
        case IDC_JS_LIST_VIEW_BTN:
            // Switch file list to LVS_LIST style.
            state->js_detail_view = false;
            SetWindowLongPtrW(state->hwnd_js_file_list, GWL_STYLE,
                (GetWindowLongPtrW(state->hwnd_js_file_list, GWL_STYLE)
                    & ~(LVS_REPORT | LVS_LIST)) | LVS_LIST);
            InvalidateRect(state->hwnd_js_file_list, nullptr, TRUE);
            break;
        case IDC_JS_DETAIL_VIEW_BTN:
            // Switch file list to LVS_REPORT style.
            state->js_detail_view = true;
            SetWindowLongPtrW(state->hwnd_js_file_list, GWL_STYLE,
                (GetWindowLongPtrW(state->hwnd_js_file_list, GWL_STYLE)
                    & ~(LVS_REPORT | LVS_LIST)) | LVS_REPORT);
            InvalidateRect(state->hwnd_js_file_list, nullptr, TRUE);
            break;
        case IDC_JS_ADD_BTN:     do_add_selected(*state); break;
        case IDC_JS_REMOVE_BTN:  do_remove_selected(*state); break;
        case IDC_JS_CLEAR_BTN:   do_clear_selected(*state); break;
        case IDC_JS_PROCESS_BTN: do_process_all(*state); break;
        case IDC_JS_SAVE_LIST_BTN: {
            // Save selected files list to a text file.
            wchar_t filename[MAX_PATH] = L"selected_files.txt";
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Text Files\0*.txt\0All Files\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_OVERWRITEPROMPT;
            ofn.lpstrTitle = L"Save File List";
            ofn.lpstrDefExt = L"txt";
            if (GetSaveFileNameW(&ofn)) {
                std::ofstream out(filename);
                for (auto& f : state->selected_files)
                    out << f.string() << "\n";
            }
            break;
        }
        case IDC_JS_LOAD_LIST_BTN: {
            // Load selected files list from a text file.
            wchar_t filename[MAX_PATH] = {};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Text Files\0*.txt\0All Files\0*.*\0";
            ofn.lpstrFile = filename;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST;
            ofn.lpstrTitle = L"Load File List";
            if (GetOpenFileNameW(&ofn)) {
                std::ifstream in(filename);
                std::string line;
                while (std::getline(in, line)) {
                    if (line.empty()) continue;
                    auto p = fs::path(line);
                    if (fs::exists(p)) {
                        bool dup = false;
                        for (auto& f : state->selected_files)
                            if (f == p) { dup = true; break; }
                        if (!dup) state->selected_files.push_back(p);
                    }
                }
                populate_selected_list(*state);
            }
            break;
        }
        case IDC_JS_FILE_TYPE_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE)
                populate_file_list(*state);
            break;
        case IDC_JS_OUTDIR_BROWSE: {
            wchar_t path[MAX_PATH] = {};
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = path;
            bi.lpszTitle = L"Select output directory";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            auto* pidl = SHBrowseForFolderW(&bi);
            if (pidl && SHGetPathFromIDListW(pidl, path))
                SetDlgItemTextW(state->hwnd_js_batch_panel, IDC_JS_OUTDIR_COMBO, path);
            if (pidl) CoTaskMemFree(pidl);
            break;
        }

        case IDM_HELP_ABOUT:
            MessageBoxW(hwnd,
                L"PPP Job Viewer\n"
                L"Precise Page Positioning - Modern C++20\n\n"
                L"Image processing pipeline for document scanning.\n"
                L"Open images, configure profiles, process and save.",
                L"About PPP Job Viewer", MB_OK | MB_ICONINFORMATION);
            break;
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (!state) break;
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            int scroll_step = shift ? 100 : 30;  // Shift = fast scroll

            switch (wp) {
            // --- Processing ---
            case VK_F5:
                if (shift)
                    do_process_all(*state);
                else
                    do_process_current(*state);
                return 0;

            // --- View toggle ---
            case VK_F6:
                state->showing_processed = false;
                update_display(*state);
                return 0;
            case VK_F7:
                state->showing_processed = true;
                update_display(*state);
                return 0;
            case VK_SPACE:
                if (state->current_index >= 0) {
                    state->showing_processed = !state->showing_processed;
                    update_display(*state);
                }
                return 0;

            // --- Page navigation ---
            case VK_NEXT:   // Page Down
                SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_NEXT, 0);
                return 0;
            case VK_PRIOR:  // Page Up
                SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_PREV, 0);
                return 0;
            case VK_HOME:
                SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_FIRST, 0);
                return 0;
            case VK_END:
                SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_LAST, 0);
                return 0;

            // --- Arrow key scrolling ---
            case VK_UP:
                if (ctrl) {
                    // Ctrl+Up = previous page
                    SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_PREV, 0);
                } else {
                    state->scroll_y = std::max(0, state->scroll_y - scroll_step);
                    InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
                }
                return 0;
            case VK_DOWN:
                if (ctrl) {
                    // Ctrl+Down = next page
                    SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_NEXT, 0);
                } else {
                    state->scroll_y += scroll_step;
                    InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
                }
                return 0;
            case VK_LEFT:
                state->scroll_x = std::max(0, state->scroll_x - scroll_step);
                InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
                return 0;
            case VK_RIGHT:
                state->scroll_x += scroll_step;
                InvalidateRect(state->hwnd_image_panel, nullptr, FALSE);
                return 0;

            // --- Zoom via +/- keys (numpad and main) ---
            case VK_ADD:       // Numpad +
                SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_ZOOM_IN, 0);
                return 0;
            case VK_SUBTRACT:  // Numpad -
                SendMessageW(hwnd, WM_COMMAND, IDM_VIEW_ZOOM_OUT, 0);
                return 0;

            // --- Rotate ---
            case 'R':
                if (ctrl) {
                    SendMessageW(hwnd, WM_COMMAND, IDM_IMAGE_ROTATE_CW, 0);
                    return 0;
                }
                break;
            case 'L':
                if (ctrl) {
                    SendMessageW(hwnd, WM_COMMAND, IDM_IMAGE_ROTATE_CCW, 0);
                    return 0;
                }
                break;

            // --- Delete page ---
            case VK_DELETE:
                SendMessageW(hwnd, WM_COMMAND, IDM_IMAGE_DELETE_PAGE, 0);
                return 0;
            }
        }
        break;

    case WM_DESTROY:
        if (state) free_display_bitmap(*state);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
    // Initialize COM (needed for SHBrowseForFolder with BIF_NEWDIALOGSTYLE).
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Initialize common controls.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    register_tab_page_class(instance);
    register_image_panel_class(instance);

    constexpr wchar_t kWindowClassName[] = L"PPPJobViewerMainWindow";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.",
                    L"PPP Job Viewer", MB_ICONERROR | MB_OK);
        return 1;
    }

    AppState state;
    state.hinstance = instance;

    // Create UI fonts.
    state.hfont_ui = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    state.hfont_mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    FIXED_PITCH | FF_MODERN, L"Consolas");

    HWND hwnd = CreateWindowExW(
        0, kWindowClassName, L"PPP Job Viewer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
        nullptr, nullptr, instance, &state);

    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create main window.",
                    L"PPP Job Viewer", MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    // Create accelerator table for keyboard shortcuts.
    ACCEL accels[] = {
        // File
        {FCONTROL | FVIRTKEY, 'O', IDM_FILE_OPEN},
        {FCONTROL | FVIRTKEY, 'S', IDM_FILE_SAVE},
        // Process
        {FVIRTKEY, VK_F5, IDM_PROCESS_CURRENT},
        {FSHIFT | FVIRTKEY, VK_F5, IDM_PROCESS_ALL},
        // View / Zoom
        {FCONTROL | FVIRTKEY, '0', IDM_VIEW_FIT},
        {FCONTROL | FVIRTKEY, '1', IDM_VIEW_ACTUAL},
        {FCONTROL | FVIRTKEY, VK_OEM_PLUS, IDM_VIEW_ZOOM_IN},
        {FCONTROL | FVIRTKEY, VK_OEM_MINUS, IDM_VIEW_ZOOM_OUT},
        // Image
        {FCONTROL | FVIRTKEY, 'R', IDM_IMAGE_ROTATE_CW},
        {FCONTROL | FVIRTKEY, 'L', IDM_IMAGE_ROTATE_CCW},
        {FVIRTKEY, VK_DELETE, IDM_IMAGE_DELETE_PAGE},
        // Navigation
        {FVIRTKEY, VK_HOME, IDM_VIEW_FIRST},
        {FVIRTKEY, VK_END, IDM_VIEW_LAST},
    };
    HACCEL haccel = CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(hwnd, haccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (haccel) DestroyAcceleratorTable(haccel);

    return static_cast<int>(msg.wParam);
}

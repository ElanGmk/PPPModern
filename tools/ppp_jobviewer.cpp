// PPP Job Viewer — Win32 GUI for document image processing.
//
// Replicates the legacy VCL application workflow:
//   1. Open image files (TIFF/BMP)
//   2. Configure processing profile
//   3. Process images through the pipeline
//   4. View before/after results
//   5. Save processed output (TIFF/BMP/PDF)

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
    // Controls
    IDC_TOOLBAR = 2001,
    IDC_STATUSBAR,
    IDC_IMAGE_PANEL,
    IDC_STEP_LOG,
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
};

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

struct AppState {
    HINSTANCE hinstance{};

    // Window handles
    HWND hwnd_main{};
    HWND hwnd_toolbar{};
    HWND hwnd_statusbar{};
    HWND hwnd_image_panel{};
    HWND hwnd_step_log{};

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

    // File state
    fs::path current_file;
    fs::path profile_path;
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
void create_step_log(AppState& state);
void layout_children(AppState& state);
void update_title(AppState& state);
void update_statusbar(AppState& state);
void update_display(AppState& state);
void update_step_log(AppState& state);
void rebuild_display_bitmap(AppState& state);
void free_display_bitmap(AppState& state);

void do_file_open(AppState& state);
void do_file_save(AppState& state);
void do_process_current(AppState& state);
void do_process_all(AppState& state);
void do_process_step(AppState& state, const std::string& step_name);
void do_profile_load(AppState& state);
void do_profile_save(AppState& state);
void do_profile_edit(AppState& state);
void do_profile_reset(AppState& state);

Image load_image_file(const fs::path& path, std::size_t page = 0);

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

constexpr wchar_t kImagePanelClass[] = L"PPPImagePanel";

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
// Step log panel (read-only edit control)
// ---------------------------------------------------------------------------

void create_step_log(AppState& state) {
    state.hwnd_step_log = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 100, 100, state.hwnd_main,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_STEP_LOG)),
        state.hinstance, nullptr);

    // Set monospace font.
    HFONT font = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(state.hwnd_step_log, WM_SETFONT,
                 reinterpret_cast<WPARAM>(font), TRUE);
}

void update_step_log(AppState& state) {
    if (state.current_index < 0 || state.entries.empty()) {
        SetWindowTextW(state.hwnd_step_log, L"No image loaded.\r\n");
        return;
    }

    const auto& entry = state.entries[state.current_index];
    std::wostringstream oss;

    oss << L"--- Image Info ---\r\n";
    oss << L"File: " << entry.source_path.filename().wstring() << L"\r\n";
    oss << L"Original: " << entry.original.width() << L"x"
        << entry.original.height() << L"\r\n";
    oss << L"DPI: " << static_cast<int>(entry.original.dpi_x()) << L"x"
        << static_cast<int>(entry.original.dpi_y()) << L"\r\n\r\n";

    if (entry.result) {
        oss << L"--- Processing Steps ---\r\n";
        for (const auto& step : entry.result->steps) {
            oss << (step.applied ? L"[+] " : L"[-] ");
            oss << widen(step.name) << L": " << widen(step.detail) << L"\r\n";
        }

        if (entry.result->is_blank) {
            oss << L"\r\nResult: BLANK PAGE\r\n";
        }

        if (entry.processed) {
            oss << L"\r\nOutput: " << entry.processed->width() << L"x"
                << entry.processed->height() << L"\r\n";
        }
    } else {
        oss << L"Not yet processed.\r\n";
        oss << L"Press F5 or Process > Process Current.\r\n";
    }

    SetWindowTextW(state.hwnd_step_log, oss.str().c_str());
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
    int log_width = 280;
    int image_width = client_w - log_width;
    if (image_width < 200) {
        image_width = client_w;
        log_width = 0;
    }

    MoveWindow(state.hwnd_image_panel, 0, toolbar_h, image_width, avail_h, TRUE);
    MoveWindow(state.hwnd_step_log, image_width, toolbar_h, log_width, avail_h, TRUE);
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
    update_step_log(state);
    update_title(state);
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
            ++succeeded;
        } else {
            ++failed;
        }
    }

    SetCursor(LoadCursorW(nullptr, IDC_ARROW));

    state.showing_processed = true;
    update_display(state);

    std::wostringstream msg;
    msg << L"Processed " << succeeded << L" of " << state.entries.size()
        << L" pages.";
    if (failed > 0) msg << L"\n" << failed << L" failed.";
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
        create_step_log(*state);

        update_statusbar(*state);
        update_step_log(*state);

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
    // Initialize common controls.
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

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

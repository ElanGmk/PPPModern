#include "ppp/core/job_repository.h"
#include "ppp/core/job_service.h"

#include <windows.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return L"";
    }
    std::wstring widened(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), widened.data(), length);
    return widened;
}

std::unique_ptr<ppp::core::JobRepository> create_repository() {
#if PPP_CORE_HAVE_SQLSERVER
    if (const char* conn = std::getenv("PPP_JOBCTL_SQLSERVER"); conn && *conn) {
        return std::make_unique<ppp::core::SqlServerJobRepository>(std::string{conn});
    }
#endif
#if PPP_CORE_HAVE_SQLITE
    if (const char* db = std::getenv("PPP_JOBCTL_SQLITE"); db && *db) {
        return std::make_unique<ppp::core::SqliteJobRepository>(std::filesystem::path{db});
    }
#endif
    if (const char* env = std::getenv("PPP_JOBCTL_STORE"); env && *env) {
        return std::make_unique<ppp::core::FileJobRepository>(std::filesystem::path{env});
    }
    return std::make_unique<ppp::core::InMemoryJobRepository>();
}

constexpr wchar_t kWindowClassName[] = L"PPPJobViewerWindow";
constexpr wchar_t kWindowTitle[] = L"PPP Job Viewer";

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE: {
        try {
            auto repository = create_repository();
            ppp::core::JobService service{*repository};
            const auto summary = service.summarize();

            std::ostringstream oss;
            oss << "Total jobs: " << summary.total << "\r\n"
                << "Submitted: " << summary.submitted << "\r\n"
                << "Validating: " << summary.validating << "\r\n"
                << "Rendering: " << summary.rendering << "\r\n"
                << "Exceptions: " << summary.exception << "\r\n"
                << "Completed: " << summary.completed << "\r\n"
                << "Cancelled: " << summary.cancelled;

            const auto message = widen(oss.str());
            CreateWindowExW(0, L"STATIC", message.c_str(), WS_VISIBLE | WS_CHILD,
                            16, 16, 320, 160, hwnd, nullptr, nullptr, nullptr);
        } catch (const std::exception& ex) {
            const auto message = widen(std::string{"Failed to load job summary:\r\n"} + ex.what());
            CreateWindowExW(0, L"STATIC", message.c_str(), WS_VISIBLE | WS_CHILD,
                            16, 16, 360, 120, hwnd, nullptr, nullptr, nullptr);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class.", kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    const HWND hwnd = CreateWindowExW(0, kWindowClassName, kWindowTitle, WS_OVERLAPPEDWINDOW,
                                      CW_USEDEFAULT, CW_USEDEFAULT, 400, 260, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create main window.", kWindowTitle, MB_ICONERROR | MB_OK);
        return 1;
    }

    ShowWindow(hwnd, show_command);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

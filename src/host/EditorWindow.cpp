#include "host/EditorWindow.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace mimicry::host {

#if defined(_WIN32)

namespace {

constexpr const wchar_t* kClassName = L"MimicryEditorWindow";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

DWORD style() { return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX; }

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {  // hide it and tell the engine; the plugin's view is torn down before the window goes
        auto* closed = reinterpret_cast<bool*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (closed) *closed = true;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

struct EditorWindow::Impl {
    HWND hwnd = nullptr;
    bool closed = false;
};

EditorWindow::EditorWindow() : impl_(std::make_unique<Impl>()) {}
EditorWindow::~EditorWindow() { destroy(); }

expected<void, HostError> EditorWindow::create(const std::string& title, int width, int height) {
    if (impl_->hwnd) return {};
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) return unexpected(HostError{"cannot register the editor window class"});
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, style(), FALSE, 0);
    impl_->closed = false;
    impl_->hwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), style(), CW_USEDEFAULT, CW_USEDEFAULT,
                                  r.right - r.left, r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!impl_->hwnd) return unexpected(HostError{"cannot create the editor window"});
    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&impl_->closed));
    return {};
}

void EditorWindow::show() {
    if (!impl_->hwnd) return;
    ShowWindow(impl_->hwnd, SW_SHOW);
    SetForegroundWindow(impl_->hwnd);
}

void EditorWindow::resize_client(int width, int height) {
    if (!impl_->hwnd) return;
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, style(), FALSE, 0);
    SetWindowPos(impl_->hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void EditorWindow::destroy() {
    if (impl_ && impl_->hwnd) {
        SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(impl_->hwnd);
        impl_->hwnd = nullptr;
    }
}

void* EditorWindow::handle() const { return impl_ ? impl_->hwnd : nullptr; }
bool EditorWindow::closed_by_user() const { return impl_ && impl_->closed; }

#else  // no editor hosting off Windows (OQ12)

struct EditorWindow::Impl {};
EditorWindow::EditorWindow() = default;
EditorWindow::~EditorWindow() = default;
expected<void, HostError> EditorWindow::create(const std::string&, int, int) {
    return unexpected(HostError{"the plugin editor can only be shown on Windows"});
}
void EditorWindow::show() {}
void EditorWindow::resize_client(int, int) {}
void EditorWindow::destroy() {}
void* EditorWindow::handle() const { return nullptr; }
bool EditorWindow::closed_by_user() const { return false; }

#endif

}  // namespace mimicry::host

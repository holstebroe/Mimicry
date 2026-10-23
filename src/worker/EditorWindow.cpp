#include "worker/EditorWindow.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#endif

namespace mimicry::worker {

#if defined(_WIN32)

namespace {

constexpr wchar_t kFrameClass[] = L"MimicryCaptureFrame";
constexpr wchar_t kHostClass[] = L"MimicryEditorHost";
enum : int { kIdName = 101, kIdCapture, kIdTeach, kIdAuto, kIdDone, kIdStatus };
constexpr int kToolbar = 44;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

struct EditorWindow::Impl {
    host::Instance* instance = nullptr;
    HWND frame = nullptr, host = nullptr, name = nullptr, capture = nullptr, teach = nullptr, auto_run = nullptr, done = nullptr, status = nullptr;
    HFONT font = nullptr;
    bool resizable = false, in_plugin_resize = false, plugin_closed = false, teaching = false, hook_set = false;
    std::vector<std::string> actions;
    std::optional<ClickRecord> click;
    HHOOK hook = nullptr;

    static Impl*& current() {
        static Impl* c = nullptr;
        return c;
    }

    void layout() {
        RECT rc;
        GetClientRect(frame, &rc);
        const int w = rc.right, h = rc.bottom;
        const int pad = 6, bh = 28, y = (kToolbar - bh) / 2;
        int x = pad;
        MoveWindow(name, x, y, 150, bh - 4, TRUE);
        x += 156;
        for (HWND b : {capture, teach, auto_run, done}) {
            const int bw = b == teach ? 130 : b == auto_run ? 110 : b == done ? 70 : 100;
            MoveWindow(b, x, y, bw, bh, TRUE);
            x += bw + pad;
        }
        MoveWindow(status, x + pad, y, std::max(50, w - x - 2 * pad), bh, TRUE);
        MoveWindow(host, 0, kToolbar, w, std::max(1, h - kToolbar), TRUE);
    }

    void client_size_to_window(int w, int h) {
        RECT r{0, 0, w, h + kToolbar};
        const DWORD style = DWORD(GetWindowLongPtrW(frame, GWL_STYLE));
        AdjustWindowRectEx(&r, style, FALSE, 0);
        in_plugin_resize = true;
        SetWindowPos(frame, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        in_plugin_resize = false;
        layout();
    }

    static LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_SIZE:
                if (self->host) {
                    self->layout();
                    if (!self->in_plugin_resize && self->resizable && self->instance && wp != SIZE_MINIMIZED) {
                        RECT rc;
                        GetClientRect(self->host, &rc);
                        self->instance->editor_resized(rc.right, rc.bottom);
                    }
                }
                return 0;
            case WM_COMMAND:
                if (HIWORD(wp) == BN_CLICKED) {
                    switch (LOWORD(wp)) {
                        case kIdCapture: self->actions.emplace_back("capture"); break;
                        case kIdTeach: self->actions.emplace_back("teach"); break;
                        case kIdAuto: self->actions.emplace_back("auto"); break;
                        case kIdDone: self->actions.emplace_back("done"); break;
                    }
                    SetFocus(self->host);
                }
                return 0;
            case WM_CLOSE:  // the owner (the supervisor) ends the session; closing the window is "Done"
                self->actions.emplace_back("done");
                return 0;
            case WM_GETMINMAXINFO: {
                auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
                mm->ptMinTrackSize.x = 560;
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK mouse_hook(int code, WPARAM wp, LPARAM lp) {
        Impl* self = current();
        if (code >= 0 && self && self->teaching && wp == WM_LBUTTONDOWN) {
            const auto* mh = reinterpret_cast<const MOUSEHOOKSTRUCT*>(lp);
            if (mh->hwnd == self->host || IsChild(self->host, mh->hwnd)) {
                POINT pt = mh->pt;
                ScreenToClient(self->host, &pt);
                RECT rc;
                GetClientRect(self->host, &rc);
                ClickRecord rec;
                rec.fx = rc.right > 0 ? double(pt.x) / double(rc.right) : 0.0;
                rec.fy = rc.bottom > 0 ? double(pt.y) / double(rc.bottom) : 0.0;
                wchar_t cls[128] = {};
                GetClassNameW(mh->hwnd, cls, 127);
                rec.window_class = narrow(cls);
                self->click = rec;
                self->teaching = false;
                self->unhook();
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);  // the click still reaches the plugin
    }

    void unhook() {
        if (hook) UnhookWindowsHookEx(hook);
        hook = nullptr;
    }

    static void pump_for(int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(2);
        }
    }

    HWND make_button(const wchar_t* text, int id) {
        HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 10, 10, frame,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return b;
    }
};

EditorWindow::EditorWindow() : impl_(std::make_unique<Impl>()) {}

EditorWindow::~EditorWindow() { close(); }

expected<host::EditorSize, std::string> EditorWindow::open(host::Instance& instance, const std::string& title) {
    Impl& s = *impl_;
    if (s.frame) close();
    // Plugin editors are drawn for the screen's scale: without this Windows stretches a bitmap of the window.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtx = BOOL(WINAPI*)(HANDLE);
        if (auto fn = reinterpret_cast<SetCtx>(reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))))
            fn(reinterpret_cast<HANDLE>(-4));  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        else
            SetProcessDPIAware();
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW fc{};
        fc.lpfnWndProc = Impl::frame_proc;
        fc.hInstance = GetModuleHandleW(nullptr);
        fc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        fc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        fc.lpszClassName = kFrameClass;
        RegisterClassW(&fc);
        WNDCLASSW hc{};
        hc.lpfnWndProc = DefWindowProcW;
        hc.hInstance = GetModuleHandleW(nullptr);
        hc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        hc.lpszClassName = kHostClass;
        RegisterClassW(&hc);
        registered = true;
    }
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    s.font = CreateFontIndirectW(&ncm.lfMessageFont);
    s.instance = &instance;
    s.plugin_closed = false;
    s.actions.clear();
    s.click.reset();
    s.frame = CreateWindowExW(0, kFrameClass, widen("Mimicry capture: " + title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 640,
                              nullptr, nullptr, GetModuleHandleW(nullptr), &s);
    if (!s.frame) return mimicry::unexpected(std::string("cannot create the editor window"));
    s.name = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, 0, 0, 10, 10, s.frame,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdName)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(s.name, WM_SETFONT, reinterpret_cast<WPARAM>(s.font), TRUE);
    s.capture = s.make_button(L"Capture preset", kIdCapture);
    s.teach = s.make_button(L"Teach \"next\" click", kIdTeach);
    s.auto_run = s.make_button(L"Auto-capture", kIdAuto);
    s.done = s.make_button(L"Done", kIdDone);
    s.status = CreateWindowExW(0, L"STATIC", L"Pick a preset in the plugin, then press Capture.", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0, 0, 10, 10,
                               s.frame, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(s.status, WM_SETFONT, reinterpret_cast<WPARAM>(s.font), TRUE);
    s.host = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, s.frame, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    s.layout();

    host::EditorEvents events;
    events.resize = [&s](int w, int h) { s.client_size_to_window(w, h); };
    events.closed = [&s] { s.plugin_closed = true; };
    auto size = instance.open_editor(s.host, std::move(events));
    if (!size) {
        DestroyWindow(s.frame);
        s.frame = s.host = nullptr;
        return mimicry::unexpected(size.error().message);
    }
    s.resizable = size->resizable;
    if (!s.resizable) {  // a fixed-size editor: no maximise, no resize border
        LONG_PTR st = GetWindowLongPtrW(s.frame, GWL_STYLE);
        st &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongPtrW(s.frame, GWL_STYLE, st);
    }
    s.client_size_to_window(std::max(size->width, 320), std::max(size->height, 120));
    ShowWindow(s.frame, SW_SHOW);
    // A window of a background process does not come to the front by itself: topmost, then back to normal.
    SetWindowPos(s.frame, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPos(s.frame, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(s.frame);
    SetFocus(s.host);
    return *size;
}

void EditorWindow::close() {
    Impl& s = *impl_;
    s.unhook();
    s.teaching = false;
    if (s.instance && s.frame) s.instance->close_editor();
    if (s.frame) DestroyWindow(s.frame);
    s.frame = s.host = nullptr;
    s.instance = nullptr;
    if (s.font) DeleteObject(s.font);
    s.font = nullptr;
}

bool EditorWindow::is_open() const { return impl_->frame != nullptr; }

std::vector<std::string> EditorWindow::take_actions() {
    std::vector<std::string> out;
    out.swap(impl_->actions);
    return out;
}

std::string EditorWindow::take_name() {
    Impl& s = *impl_;
    if (!s.name) return {};
    wchar_t buf[256] = {};
    GetWindowTextW(s.name, buf, 255);
    SetWindowTextW(s.name, L"");
    return narrow(buf);
}

void EditorWindow::set_status(const std::string& text) {
    if (impl_->status) SetWindowTextW(impl_->status, widen(text).c_str());
}

void EditorWindow::set_counts(int captures, bool taught, bool auto_running) {
    Impl& s = *impl_;
    if (!s.frame) return;
    SetWindowTextW(s.capture, widen("Capture preset (" + std::to_string(captures) + ")").c_str());
    SetWindowTextW(s.auto_run, auto_running ? L"Stop auto" : L"Auto-capture");
    EnableWindow(s.auto_run, taught || auto_running);
    (void)taught;
}

bool EditorWindow::take_plugin_closed() {
    const bool c = impl_->plugin_closed;
    impl_->plugin_closed = false;
    return c;
}

void EditorWindow::begin_teach() {
    Impl& s = *impl_;
    if (!s.frame) return;
    s.click.reset();
    s.teaching = true;
    Impl::current() = &s;
    if (!s.hook) s.hook = SetWindowsHookExW(WH_MOUSE, Impl::mouse_hook, nullptr, GetCurrentThreadId());
}

void EditorWindow::cancel_teach() {
    impl_->teaching = false;
    impl_->unhook();
}

bool EditorWindow::teaching() const { return impl_->teaching; }

std::optional<ClickRecord> EditorWindow::take_click() {
    auto c = impl_->click;
    impl_->click.reset();
    return c;
}

bool EditorWindow::replay_click(const ClickRecord& click, bool real_input) {
    Impl& s = *impl_;
    if (!s.frame || !s.host) return false;
    RECT rc;
    GetClientRect(s.host, &rc);
    POINT pt{LONG(click.fx * rc.right), LONG(click.fy * rc.bottom)};
    // The deepest child window under the point (the plugin's own windows are children of the editor area).
    HWND target = s.host;
    POINT local = pt;
    for (int depth = 0; depth < 8; ++depth) {
        HWND child = ChildWindowFromPointEx(target, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == target) break;
        MapWindowPoints(target, child, &local, 1);
        target = child;
    }
    if (real_input) {
        POINT screen = pt;
        ClientToScreen(s.host, &screen);
        SetForegroundWindow(s.frame);
        SetCursorPos(screen.x, screen.y);
        Impl::pump_for(30);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        Impl::pump_for(40);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    } else {
        const LPARAM lp = MAKELPARAM(local.x, local.y);
        PostMessageW(target, WM_MOUSEMOVE, 0, lp);
        Impl::pump_for(20);
        PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
        Impl::pump_for(40);
        PostMessageW(target, WM_LBUTTONUP, 0, lp);
    }
    Impl::pump_for(10);
    return true;
}

#else  // not Windows: editors are not hosted

struct EditorWindow::Impl {};

EditorWindow::EditorWindow() : impl_(std::make_unique<Impl>()) {}
EditorWindow::~EditorWindow() = default;
expected<host::EditorSize, std::string> EditorWindow::open(host::Instance&, const std::string&) {
    return mimicry::unexpected(std::string("plugin editors are hosted on Windows only"));
}
void EditorWindow::close() {}
bool EditorWindow::is_open() const { return false; }
std::vector<std::string> EditorWindow::take_actions() { return {}; }
std::string EditorWindow::take_name() { return {}; }
void EditorWindow::set_status(const std::string&) {}
void EditorWindow::set_counts(int, bool, bool) {}
bool EditorWindow::take_plugin_closed() { return false; }
void EditorWindow::begin_teach() {}
void EditorWindow::cancel_teach() {}
bool EditorWindow::teaching() const { return false; }
std::optional<ClickRecord> EditorWindow::take_click() { return std::nullopt; }
bool EditorWindow::replay_click(const ClickRecord&, bool) { return false; }

#endif

}  // namespace mimicry::worker

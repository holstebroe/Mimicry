#pragma once
// Small Win32 helpers for the front end: ANSI <-> UTF-16, DPI scaling, window text.

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

#include <string>

namespace mimicry::gui::win {

// Text crossing to or from mimicry.exe, a file or a list is in the ANSI code page, as the command line's own
// arguments, paths and JSON are (its `main` receives the ANSI command line): one encoding end to end, so a
// folder name the code page can write survives a round trip. Names outside it are a known limit (plan OQ16).
inline constexpr UINT kCodePage = CP_ACP;

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(kCodePage, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(kCodePage, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(kCodePage, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(std::size_t(n), '\0');
    WideCharToMultiByte(kCodePage, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline std::string get_text(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring w(std::size_t(n) + 1, L'\0');
    const int got = GetWindowTextW(h, w.data(), n + 1);
    w.resize(std::size_t(got > 0 ? got : 0));
    return narrow(w);
}

inline void set_text(HWND h, const std::string& s) { SetWindowTextW(h, widen(s).c_str()); }

// Screen scale (1.0 at 96 dpi); the process is system-DPI aware.
inline double dpi_scale() {
    static const double scale = [] {
        HDC dc = GetDC(nullptr);
        const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        return dpi > 0 ? dpi / 96.0 : 1.0;
    }();
    return scale;
}

inline int S(int px) { return int(px * dpi_scale() + 0.5); }

}  // namespace mimicry::gui::win

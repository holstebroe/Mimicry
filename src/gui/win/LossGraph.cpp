#include "gui/win/LossGraph.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace mimicry::gui::win {

namespace {

struct State {
    const std::vector<LossPoint>* history = nullptr;
    double time_cap_s = 0;
    std::string caption;
};

constexpr wchar_t kClass[] = L"MimicryLossGraph";

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    // Double buffer.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT));
    const COLORREF text = GetSysColor(COLOR_WINDOWTEXT), grid = GetSysColor(COLOR_BTNFACE), faint = GetSysColor(COLOR_GRAYTEXT);

    const auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const int left = S(46), right = S(12), top = S(22), bottom = S(22);
    RECT plot{left, top, rc.right - right, rc.bottom - bottom};
    SetTextColor(mem, faint);
    if (st && !st->caption.empty()) {
        RECT cap{S(8), S(3), rc.right - S(8), top};
        const std::wstring w = widen(st->caption);
        DrawTextW(mem, w.c_str(), -1, &cap, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (plot.right - plot.left > S(40) && plot.bottom - plot.top > S(40)) {
        HPEN grid_pen = CreatePen(PS_SOLID, 1, grid);
        HGDIOBJ old_pen = SelectObject(mem, grid_pen);
        Rectangle(mem, plot.left, plot.top, plot.right, plot.bottom);
        const auto* h = st ? st->history : nullptr;
        if (h && h->size() >= 1) {
            double tmax = h->back().seconds, lo = 1e300, hi = -1e300;
            for (const auto& p : *h) {
                lo = std::min(lo, p.best_db);
                hi = std::max(hi, p.best_db);
            }
            if (st->time_cap_s > 0) tmax = std::max(tmax, st->time_cap_s);
            tmax = std::max(tmax, 1.0);
            // The curve starts high and flattens: a log axis keeps the late refinement visible.
            const bool use_log = lo > 0 && hi / std::max(lo, 1e-3) > 8;
            auto ty = [&](double v) { return use_log ? std::log10(std::max(v, 1e-3)) : v; };
            double ylo = ty(lo), yhi = ty(hi);
            if (yhi - ylo < 1e-9) {
                ylo -= 0.5;
                yhi += 0.5;
            }
            const double pad = 0.06 * (yhi - ylo);
            ylo -= pad;
            yhi += pad;
            auto X = [&](double t) { return plot.left + int((plot.right - plot.left) * std::clamp(t / tmax, 0.0, 1.0)); };
            auto Y = [&](double v) { return plot.bottom - int((plot.bottom - plot.top) * (ty(v) - ylo) / (yhi - ylo)); };
            // Horizontal grid with labels.
            SetTextColor(mem, faint);
            for (int i = 0; i <= 4; ++i) {
                const double yv = ylo + (yhi - ylo) * i / 4.0;
                const int y = plot.bottom - int((plot.bottom - plot.top) * i / 4.0);
                if (i > 0 && i < 4) {
                    MoveToEx(mem, plot.left, y, nullptr);
                    LineTo(mem, plot.right, y);
                }
                const double v = use_log ? std::pow(10.0, yv) : yv;
                const std::wstring lab = widen(std::format("{:.2f}", v));
                RECT lr{S(2), y - S(8), left - S(4), y + S(8)};
                DrawTextW(mem, lab.c_str(), -1, &lr, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
            }
            const std::wstring t0 = L"0:00", t1 = widen(format_duration(tmax));
            RECT a{plot.left, plot.bottom + S(3), plot.left + S(60), rc.bottom}, b{plot.right - S(80), plot.bottom + S(3), plot.right, rc.bottom};
            DrawTextW(mem, t0.c_str(), -1, &a, DT_LEFT | DT_SINGLELINE);
            DrawTextW(mem, t1.c_str(), -1, &b, DT_RIGHT | DT_SINGLELINE);
            // The curve.
            HPEN line = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(0x1f, 0x6f, 0xeb));
            SelectObject(mem, line);
            bool first = true;
            for (const auto& p : *h) {
                if (first) MoveToEx(mem, X(p.seconds), Y(p.best_db), nullptr);
                else LineTo(mem, X(p.seconds), Y(p.best_db));
                first = false;
            }
            SelectObject(mem, grid_pen);
            DeleteObject(line);
            const auto& last = h->back();
            const std::wstring lab = widen(std::format("{:.3f} dB", last.best_db));
            SetTextColor(mem, text);
            RECT lr{plot.left + S(6), plot.top + S(4), plot.right - S(6), plot.top + S(22)};
            DrawTextW(mem, lab.c_str(), -1, &lr, DT_RIGHT | DT_SINGLELINE);
        } else {
            SetTextColor(mem, faint);
            const std::wstring msg = L"The loss of the best candidate appears here.";
            DrawTextW(mem, msg.c_str(), -1, &plot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(mem, old_pen);
        DeleteObject(grid_pen);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_font);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new State())); return 0;
        case WM_DESTROY: delete reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: paint(hwnd); return 0;
        case WM_SIZE: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void LossGraph::register_class(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc);
}

HWND LossGraph::create(HWND parent, HINSTANCE instance, int id) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

void LossGraph::set_data(HWND graph, const std::vector<LossPoint>* history, double time_cap_s, const std::string& caption) {
    auto* st = reinterpret_cast<State*>(GetWindowLongPtrW(graph, GWLP_USERDATA));
    if (!st) return;
    st->history = history;
    st->time_cap_s = time_cap_s;
    st->caption = caption;
    InvalidateRect(graph, nullptr, FALSE);
}

}  // namespace mimicry::gui::win

// mimicry_tones_gui (Windows): a small host window for the tone harvest (docs/spec/06_TARGETS.md section 6a).
//
// Pick the plugin, the preset folder and the output folder; "Open plugin window" shows the plugin's own editor
// (the worker process hosts it, CLAUDE.md rule 4) so you can activate or sign in by hand; "Start" closes that
// window and renders the tones. Progress and failures are listed in the log. The work is `targets::run_tones`,
// the same code as `mimicry tones`. Plain Win32, no framework.

#if !defined(_WIN32)
#error "mimicry_tones_gui is Windows only"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "features/Pitch.hpp"
#include "host/PluginHost.hpp"
#include "supervisor/WorkerClient.hpp"
#include "targets/Tones.hpp"

namespace {

using namespace mimicry;
namespace fs = std::filesystem;

enum Id : int {
    kPlugin = 100, kPresets, kOut, kNotes, kVelocities, kTakes,
    kBrowsePlugin, kBrowsePresets, kBrowseOut, kOpen, kStart, kStop, kProgress, kLog,
};
constexpr UINT kMsgLog = WM_APP + 1;   // lParam: new std::wstring*
constexpr UINT kMsgIdle = WM_APP + 2;  // the background job ended

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::vector<std::string> split_commas(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : text) {
        if (c == ',' || c == ';' || c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance) {}

    bool create() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &App::proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"MimicryGui";
        if (!RegisterClassExW(&wc)) return false;
        RECT r{0, 0, 716, 530};
        AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Mimicry tone harvest", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, instance_, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOW);
        return true;
    }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        App* self = nullptr;
        if (msg == WM_NCCREATE) {
            self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return c;
    }
    void label(const wchar_t* text, int x, int y, int w) { control(L"STATIC", text, SS_LEFT, x, y + 3, w, 18, -1); }
    void field(int id, const wchar_t* text, int x, int y, int w) {
        control(L"EDIT", text, ES_AUTOHSCROLL | WS_TABSTOP, x, y, w, 22, id, WS_EX_CLIENTEDGE);
    }
    void button(int id, const wchar_t* text, int x, int y, int w) { control(L"BUTTON", text, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, 26, id); }

    void build() {
        label(L"Plugin", 12, 12, 90);
        field(kPlugin, L"", 110, 12, 490);
        button(kBrowsePlugin, L"Browse...", 610, 10, 90);
        label(L"Preset folder", 12, 44, 90);
        field(kPresets, L"", 110, 44, 490);
        button(kBrowsePresets, L"Browse...", 610, 42, 90);
        label(L"Output folder", 12, 76, 90);
        field(kOut, L"tones", 110, 76, 490);
        button(kBrowseOut, L"Browse...", 610, 74, 90);
        label(L"Notes", 12, 112, 90);
        field(kNotes, L"C2,C3,C4", 110, 112, 150);
        label(L"Velocities", 280, 112, 70);
        field(kVelocities, L"50,110", 355, 112, 120);
        label(L"Takes", 500, 112, 45);
        field(kTakes, L"5", 550, 112, 40);
        button(kOpen, L"Open plugin window", 12, 148, 170);
        button(kStart, L"Start", 192, 148, 100);
        button(kStop, L"Stop", 302, 148, 100);
        control(PROGRESS_CLASSW, L"", 0, 12, 186, 688, 18, kProgress);
        control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 12, 212, 688, 306, kLog, WS_EX_CLIENTEDGE);
        EnableWindow(item(kStop), FALSE);
        SendMessageW(item(kLog), EM_SETLIMITTEXT, 1 << 24, 0);  // the default limit is 32 KB
        log("1. Choose the plugin and the preset folder (the output of 'mimicry harvest').\r\n"
            "2. 'Open plugin window' shows the plugin's own window: activate or sign in there.\r\n"
            "3. 'Start' closes that window and renders every preset at the chosen notes and velocities.\r\n");
    }

    HWND item(int id) const { return GetDlgItem(hwnd_, id); }
    std::wstring text(int id) const {
        const int n = GetWindowTextLengthW(item(id));
        std::wstring s(static_cast<std::size_t>(n) + 1, L'\0');
        GetWindowTextW(item(id), s.data(), n + 1);
        s.resize(static_cast<std::size_t>(n));
        return s;
    }

    // Safe from any thread.
    void log(const std::string& line) const { PostMessageW(hwnd_, kMsgLog, 0, reinterpret_cast<LPARAM>(new std::wstring(to_wide(line)))); }

    void append(const std::wstring& line) {
        HWND edit = item(kLog);
        const int end = GetWindowTextLengthW(edit);
        SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
        SendMessageW(edit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    }

    void set_busy(bool busy) {
        busy_ = busy;
        for (const int id : {kOpen, kStart, kBrowsePlugin, kBrowsePresets, kBrowseOut}) EnableWindow(item(id), busy ? FALSE : TRUE);
        EnableWindow(item(kStop), busy && running_ ? TRUE : FALSE);
    }

    void browse_plugin() {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"Plugins (*.vst3;*.clap)\0*.vst3;*.clap\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) SetWindowTextW(item(kPlugin), file);
    }

    void browse_folder(int id, const wchar_t* title) {
        BROWSEINFOW bi{};
        bi.hwndOwner = hwnd_;
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
            wchar_t path[MAX_PATH] = {};
            if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(item(id), path);
            CoTaskMemFree(pidl);
        }
    }

    // Runs `fn` on a background thread; the buttons stay off until it ends.
    template <typename F>
    void run_async(bool cancellable, F fn) {
        if (busy_) return;
        join();
        running_ = cancellable;
        cancel_ = false;
        set_busy(true);
        thread_ = std::thread([this, fn = std::move(fn)]() mutable {
            fn();
            PostMessageW(hwnd_, kMsgIdle, 0, 0);
        });
    }
    void join() {
        if (thread_.joinable()) thread_.join();
    }

    // Opens the plugin in a worker, once per path. Background thread.
    bool ensure_plugin(const std::string& path) {
        if (inst_ && loaded_path_ == path) return true;
        inst_.reset();
        if (auto f = host::format_from_path(path); !f) {
            log("error: " + f.error() + "\r\n");
            return false;
        }
        log("Loading " + path + " ...\r\n");
        // A plugin activated by hand keeps its session in the worker's memory: never recycle it.
        supervisor::WorkerOptions wo;
        wo.recycle_after = std::numeric_limits<int>::max();
        auto r = supervisor::WorkerClient::open(path, {}, wo);
        if (!r) {
            log("error: " + r.error().message + "\r\n");
            return false;
        }
        inst_ = std::move(*r);
        loaded_path_ = path;
        log(std::format("Loaded {} ({}).\r\n", inst_->info().name, host::to_string(inst_->info().format)));
        return true;
    }

    void open_plugin_window() {
        const std::string path = to_utf8(text(kPlugin));
        if (path.empty()) {
            log("Choose a plugin first.\r\n");
            return;
        }
        run_async(false, [this, path] {
            if (!ensure_plugin(path)) return;
            if (auto e = inst_->open_editor(); !e) log("error: " + e.error().message + "\r\n");
            else log("The plugin window is open. Activate or sign in there, then press Start here.\r\n");
        });
    }

    void start() {
        const std::string path = to_utf8(text(kPlugin));
        const std::wstring presets = text(kPresets), out = text(kOut);
        if (path.empty() || presets.empty() || out.empty()) {
            log("Choose the plugin, the preset folder and the output folder first.\r\n");
            return;
        }
        std::vector<int> keys, velocities;
        for (const auto& n : split_commas(to_utf8(text(kNotes)))) {
            const auto k = features::parse_note_name(n);
            if (!k) {
                log("Bad note '" + n + "' (use names like C3 or F#2).\r\n");
                return;
            }
            keys.push_back(*k);
        }
        for (const auto& v : split_commas(to_utf8(text(kVelocities)))) {
            const int x = std::atoi(v.c_str());
            if (x < 1 || x > 127) {
                log("Velocity '" + v + "' is not 1..127.\r\n");
                return;
            }
            velocities.push_back(x);
        }
        const int takes = std::atoi(to_utf8(text(kTakes)).c_str());
        if (keys.empty() || velocities.empty() || takes < 1) {
            log("Need at least one note, one velocity and one take.\r\n");
            return;
        }
        run_async(true, [this, path, presets, out, keys, velocities, takes] {
            auto library = targets::list_library(fs::path(presets));
            if (!library) {
                log("error: " + library.error() + "\r\n");
                return;
            }
            if (!ensure_plugin(path)) return;
            inst_->close_editor();  // the session stays in the worker; renders reset the plugin, they do not reload it
            targets::ToneRun run;
            run.presets = std::move(*library);
            run.keys = keys;
            run.velocities = velocities;
            run.tone.takes = takes;
            run.out = fs::path(out);
            run.plugin_name = inst_->info().name;
            run.plugin_uid = inst_->info().uid;
            run.plugin_version_hash = inst_->info().version_hash;
            run.plugin_format = std::string(host::to_string(inst_->info().format));
            log(std::format("Rendering {} preset(s) x {} note(s) x {} velocity(ies) x {} take(s) ...\r\n", run.presets.size(),
                            keys.size(), velocities.size(), takes));
            // Posted, never sent: the window thread may be waiting for this thread in join() (WM_CLOSE).
            PostMessageW(item(kProgress), PBM_SETRANGE32, 0,
                         static_cast<LPARAM>(run.presets.size() * keys.size() * velocities.size()));
            PostMessageW(item(kProgress), PBM_SETPOS, 0, 0);
            auto res = targets::run_tones(
                *inst_, run,
                [this](const targets::ToneRunEntry& e) {
                    PostMessageW(item(kProgress), PBM_SETPOS, static_cast<WPARAM>(e.done), 0);
                    log(std::format("{}  {}  {}{}\r\n", e.preset, e.stem, e.status,
                                    e.chosen > 0 ? std::format("  (take {} of {}, spread {:.2f})", e.chosen, e.takes, e.spread) : ""));
                },
                &cancel_);
            if (!res) {
                log("error: " + res.error() + "\r\n");
                return;
            }
            log(std::format("{}{} tone(s) written, {} unstable, {} failed. Manifest: {}\r\n", res->cancelled ? "Stopped. " : "Done. ",
                            res->written, res->unstable, res->failed, res->manifest.string()));
        });
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE:
                build();
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wp)) {
                    case kBrowsePlugin: browse_plugin(); break;
                    case kBrowsePresets: browse_folder(kPresets, L"Preset folder (mimicry harvest output)"); break;
                    case kBrowseOut: browse_folder(kOut, L"Output folder for the tones"); break;
                    case kOpen: open_plugin_window(); break;
                    case kStart: start(); break;
                    case kStop: cancel_ = true; break;
                    default: break;
                }
                return 0;
            case kMsgLog: {
                std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
                append(*line);
                return 0;
            }
            case kMsgIdle:
                join();
                running_ = false;
                set_busy(false);
                return 0;
            case WM_CLOSE:
                cancel_ = true;
                join();
                inst_.reset();  // ends the worker and the plugin
                DestroyWindow(hwnd_);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                return DefWindowProcW(hwnd_, msg, wp, lp);
        }
    }

    HINSTANCE instance_;
    HWND hwnd_ = nullptr;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    bool busy_ = false, running_ = false;
    std::unique_ptr<host::Instance> inst_;
    std::string loaded_path_;
};

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    (void)CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&icc);
    App app(instance);
    if (!app.create()) {
        MessageBoxW(nullptr, L"Cannot create the window.", L"Mimicry", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(GetActiveWindow(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

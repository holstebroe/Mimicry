// Mimicry for Windows (docs/spec/09_GUI.md): forms for the commands, a monitor with progress, a loss curve,
// stage / preset / bank tables and a log, cancel and resume. It runs `mimicry.exe --events` as a child
// process and shows what it reports, so the command line stays the one implementation (CLAUDE.md rule 3
// and 4: this program loads no plugin).

#include "gui/win/WinUtil.hpp"  // windows.h first

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <memory>
#include <thread>

#include "gui/ChildProcess.hpp"
#include "gui/Forms.hpp"
#include "gui/RunModel.hpp"
#include "gui/win/FormPanel.hpp"
#include "gui/win/LossGraph.hpp"

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

using namespace mimicry;
using namespace mimicry::gui;
using namespace mimicry::gui::win;
namespace fs = std::filesystem;

enum : UINT {
    WM_APP_LINE = WM_APP + 1,   // wParam: from stderr, lParam: std::string* (owned by the receiver)
    WM_APP_EXIT = WM_APP + 2,   // wParam: exit code
    WM_APP_RUNS = WM_APP + 3,   // lParam: std::string* (JSON of `mimicry runs --json`)
};

enum : int {
    IDC_WORKDIR = 1001, IDC_WORKBROWSE, IDC_TAB, IDC_MORE_OPTIONS,
    IDC_START = 1010, IDC_CANCEL, IDC_RESUME, IDC_OPEN_FOLDER, IDC_PLAY, IDC_REPORT, IDC_MORE_MIN, IDC_MORE_MIN_LABEL, IDC_SPECTROGRAM,
    IDC_STATUS = 1020, IDC_PROGRESS, IDC_INFO, IDC_GRAPH, IDC_TABLE, IDC_LOG,
    IDC_RUNS_LIST = 1030, IDC_RUNS_REFRESH,
    IDC_PANEL_BASE = 2000,
};

constexpr UINT_PTR kTimer = 1;
constexpr UINT_PTR kForceTimer = 2;

enum class TableMode { Stages, Presets, Bank };

class App {
public:
    int run(HINSTANCE instance, int show);

private:
    static LRESULT CALLBACK static_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT proc(HWND, UINT, WPARAM, LPARAM);

    void create_controls();
    void layout();
    void select_tab(int index);
    [[nodiscard]] bool on_runs_tab() const { return tab_ == int(forms().size()); }
    [[nodiscard]] const Form* current_form() const { return on_runs_tab() ? nullptr : &forms()[std::size_t(tab_)]; }
    [[nodiscard]] Values current_values() const;

    void start_run(bool resume, const std::string& run_dir);
    void send_cancel();
    void on_line(bool err, std::string* line);
    void on_exit(int code);
    void refresh();
    void update_buttons();
    void set_table_mode(TableMode mode);
    [[nodiscard]] std::string cell_text(int row, int col) const;
    [[nodiscard]] int row_count() const;
    void refresh_runs();
    void fill_runs(const std::string& json_text);
    void open_path(const std::string& path);
    [[nodiscard]] fs::path work_dir() const;
    [[nodiscard]] fs::path mimicry_exe() const;
    void load_settings();
    void save_settings();
    void on_close();
    void log_append_all();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr, tab_ctl_ = nullptr, workdir_ = nullptr, work_label_ = nullptr, work_browse_ = nullptr, more_opts_ = nullptr;
    HWND status_ = nullptr, progress_ = nullptr, info_ = nullptr, graph_ = nullptr, table_ = nullptr, log_ = nullptr;
    HWND b_start_ = nullptr, b_cancel_ = nullptr, b_resume_ = nullptr, b_folder_ = nullptr, b_play_ = nullptr, b_report_ = nullptr, b_spectrogram_ = nullptr;
    HWND more_min_ = nullptr, more_min_label_ = nullptr, tooltip_ = nullptr;
    HWND runs_list_ = nullptr, runs_refresh_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr;
    std::vector<FormPanel> panels_;  // one per form, then the runs panel
    int tab_ = 0;
    bool advanced_ = false;
    int form_bottom_ = 0;

    Settings settings_;
    RunModel model_;
    std::unique_ptr<ChildProcess> child_;
    std::string active_form_id_;
    TableMode table_mode_ = TableMode::Stages;
    bool dirty_ = true;
    bool closing_ = false;
    std::size_t log_shown_ = 0;
    std::vector<json::Value> runs_;
    std::atomic<bool> runs_busy_{false};
};

App* g_app = nullptr;

fs::path App::work_dir() const {
    std::string w = get_text(workdir_);
    if (w.empty()) w = "."; 
    return fs::path(widen(w));
}

fs::path App::mimicry_exe() const {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return fs::path(buf).parent_path() / L"mimicry.exe";
}

void App::load_settings() {
    settings_ = Settings::load(Settings::default_path());
    std::string wd = settings_.get("workdir");
    if (wd.empty()) {
        PWSTR docs = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
            wd = narrow((fs::path(docs) / L"Mimicry").wstring());
            CoTaskMemFree(docs);
        }
    }
    set_text(workdir_, wd);
    for (auto& p : panels_) p.set_values(settings_.values());
    advanced_ = settings_.get("advanced") == "1";
    SendMessageW(more_opts_, BM_SETCHECK, advanced_ ? BST_CHECKED : BST_UNCHECKED, 0);
    const int tab = std::atoi(settings_.get("tab", "0").c_str());
    tab_ = std::clamp(tab, 0, int(forms().size()));
}

void App::save_settings() {
    for (auto& p : panels_)
        for (const auto& [k, v] : p.values()) settings_.set(k, v);
    settings_.set("workdir", get_text(workdir_));
    settings_.set("advanced", advanced_ ? "1" : "0");
    settings_.set("tab", std::to_string(tab_));
    settings_.save();
}

Values App::current_values() const { return panels_[std::size_t(tab_)].values(); }

void App::create_controls() {
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };
    work_label_ = make(L"STATIC", L"Work folder", SS_LEFT | SS_CENTERIMAGE, 0, -1);
    workdir_ = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_WORKDIR);
    work_browse_ = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_WORKBROWSE);
    tab_ctl_ = make(WC_TABCONTROLW, L"", WS_TABSTOP, 0, IDC_TAB);
    int i = 0;
    for (const auto& f : forms()) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        const std::wstring t = widen(f.title);
        item.pszText = const_cast<LPWSTR>(t.c_str());
        TabCtrl_InsertItem(tab_ctl_, i++, &item);
    }
    {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        wchar_t runs[] = L"Runs";
        item.pszText = runs;
        TabCtrl_InsertItem(tab_ctl_, i, &item);
    }
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0, hwnd_, nullptr, inst_, nullptr);
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, S(360));
    const auto plugins = scan_plugins();
    panels_.resize(forms().size() + 1);
    int base = IDC_PANEL_BASE;
    for (std::size_t k = 0; k < forms().size(); ++k, base += 100) panels_[k].create(hwnd_, inst_, forms()[k], base, font_, plugins, tooltip_);
    {
        Form runs;
        runs.id = "runs";
        runs.title = "Runs";
        Field dir;
        dir.key = "runs.dir";
        dir.label = "Runs folder";
        dir.kind = FieldKind::Folder;
        dir.def = "runs";
        runs.fields.push_back(dir);
        panels_.back().create(hwnd_, inst_, runs, base, font_, plugins, tooltip_);
    }
    more_opts_ = make(L"BUTTON", L"More options", BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_MORE_OPTIONS);

    status_ = make(L"STATIC", L"Ready.", SS_LEFT | SS_ENDELLIPSIS | SS_CENTERIMAGE, 0, IDC_STATUS);
    SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), TRUE);
    progress_ = make(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, IDC_PROGRESS);
    SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
    info_ = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS | SS_CENTERIMAGE, 0, IDC_INFO);
    b_start_ = make(L"BUTTON", L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDC_START);
    b_cancel_ = make(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_CANCEL);
    b_resume_ = make(L"BUTTON", L"Resume", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_RESUME);
    more_min_label_ = make(L"STATIC", L"+ minutes", SS_RIGHT | SS_CENTERIMAGE, 0, IDC_MORE_MIN_LABEL);
    more_min_ = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, WS_EX_CLIENTEDGE, IDC_MORE_MIN);
    b_folder_ = make(L"BUTTON", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_OPEN_FOLDER);
    b_play_ = make(L"BUTTON", L"Play A/B", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_PLAY);
    b_report_ = make(L"BUTTON", L"Report", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_REPORT);
    b_spectrogram_ = make(L"BUTTON", L"Spectrogram", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_SPECTROGRAM);
    {
        SendMessageW(tooltip_, TTM_ACTIVATE, TRUE, 0);
        auto tip = [&](HWND target, const wchar_t* text) {
            TOOLINFOW ti{};
            ti.cbSize = sizeof ti;
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd_;
            ti.uId = reinterpret_cast<UINT_PTR>(target);
            ti.lpszText = const_cast<LPWSTR>(text);
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        };
        tip(b_cancel_, L"Stops after the batch in flight and saves the results so far. Press again to force-stop.");
        tip(b_resume_, L"Continues the cancelled run where it stopped. \"+ minutes\" adds time to the cap.");
        tip(more_min_, L"Extra minutes for a resumed fit or transfer. Empty: what is left of the time cap.");
        tip(b_play_, L"Opens ab.wav: the target, a pause, then the match.");
        tip(b_spectrogram_, L"Opens diff.bmp: spectrograms of the target, the match and their difference (blue: match quieter, red: louder).");
        tip(workdir_, L"Runs, profiles and preset libraries are made in this folder.");
    }
    graph_ = LossGraph::create(hwnd_, inst_, IDC_GRAPH);
    table_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP,
                             0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TABLE)), inst_, nullptr);
    SendMessageW(table_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ListView_SetExtendedListViewStyle(table_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    log_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
                           0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LOG)), inst_, nullptr);
    SendMessageW(log_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(log_, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);

    runs_list_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                                 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_RUNS_LIST)), inst_, nullptr);
    SendMessageW(runs_list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ListView_SetExtendedListViewStyle(runs_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const struct { const wchar_t* t; int w; } rc[] = {{L"Status", 90}, {L"Kind", 70}, {L"Plugin", 220}, {L"Target", 220}, {L"Best dB", 70}, {L"Evals", 70}, {L"Time", 70}, {L"Folder", 300}};
    for (int c = 0; c < 8; ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(rc[c].t);
        col.cx = S(rc[c].w);
        SendMessageW(runs_list_, LVM_INSERTCOLUMNW, c, reinterpret_cast<LPARAM>(&col));
    }
    runs_refresh_ = make(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_RUNS_REFRESH);
    set_table_mode(TableMode::Stages);
}

void App::set_table_mode(TableMode mode) {
    table_mode_ = mode;
    while (SendMessageW(table_, LVM_DELETECOLUMN, 0, 0)) {}
    struct Col { const wchar_t* t; int w; };
    std::vector<Col> cols;
    switch (mode) {
        case TableMode::Stages: cols = {{L"Stage", 150}, {L"Free", 50}, {L"Before dB", 90}, {L"After dB", 90}, {L"Evals", 70}, {L"Seconds", 70}}; break;
        case TableMode::Presets: cols = {{L"#", 40}, {L"Method", 100}, {L"Preset", 170}, {L"Outcome", 110}, {L"Note", 260}}; break;
        case TableMode::Bank: cols = {{L"#", 40}, {L"Preset", 170}, {L"Status", 90}, {L"Loss dB", 70}, {L"Evals", 60}, {L"Time", 60}, {L"Note", 200}}; break;
    }
    for (std::size_t c = 0; c < cols.size(); ++c) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(cols[c].t);
        col.cx = S(cols[c].w);
        SendMessageW(table_, LVM_INSERTCOLUMNW, c, reinterpret_cast<LPARAM>(&col));
    }
    ListView_SetItemCount(table_, 0);
}

int App::row_count() const {
    switch (table_mode_) {
        case TableMode::Stages: return int(model_.stages.size());
        case TableMode::Presets: return int(model_.harvested.size());
        case TableMode::Bank: return int(model_.bank.size());
    }
    return 0;
}

std::string App::cell_text(int row, int col) const {
    if (row < 0 || row >= row_count()) return {};
    switch (table_mode_) {
        case TableMode::Stages: {
            const auto& s = model_.stages[std::size_t(row)];
            switch (col) {
                case 0: return s.name;
                case 1: return std::to_string(s.free);
                case 2: return std::format("{:.3f}", s.before_db);
                case 3: return s.finished ? std::format("{:.3f}", s.after_db) : std::string("...");
                case 4: return s.finished ? std::to_string(s.evals) : std::string();
                case 5: return s.finished ? std::format("{:.0f}", s.seconds) : std::string();
            }
            break;
        }
        case TableMode::Presets: {
            const auto& p = model_.harvested[std::size_t(row)];
            switch (col) {
                case 0: return p.number > 0 ? std::to_string(p.number) : std::string("-");
                case 1: return p.method;
                case 2: return p.name;
                case 3: return p.outcome;
                case 4: return p.message;
            }
            break;
        }
        case TableMode::Bank: {
            const auto& b = model_.bank[std::size_t(row)];
            switch (col) {
                case 0: return std::to_string(b.index);
                case 1: return b.name;
                case 2: return b.status;
                case 3: return b.status == "done" || b.status == "running" ? std::format("{:.3f}", b.loss_db) : std::string();
                case 4: return b.evals ? std::to_string(b.evals) : std::string();
                case 5: return b.seconds > 0 ? format_duration(b.seconds) : std::string();
                case 6: return b.note;
            }
            break;
        }
    }
    return {};
}

void App::select_tab(int index) {
    tab_ = std::clamp(index, 0, int(forms().size()));
    TabCtrl_SetCurSel(tab_ctl_, tab_);
    for (std::size_t k = 0; k < panels_.size(); ++k) panels_[k].show(int(k) == tab_, advanced_);
    ShowWindow(runs_list_, on_runs_tab() ? SW_SHOW : SW_HIDE);
    ShowWindow(runs_refresh_, on_runs_tab() ? SW_SHOW : SW_HIDE);
    ShowWindow(more_opts_, !on_runs_tab() && panels_[std::size_t(tab_)].has_advanced() ? SW_SHOW : SW_HIDE);
    layout();
    update_buttons();
    if (on_runs_tab()) refresh_runs();
}

void App::layout() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom, m = S(10), row = S(26);
    int y = m;
    MoveWindow(work_label_, m, y, S(130), row - S(2), TRUE);
    MoveWindow(workdir_, m + S(136), y, W - 2 * m - S(136) - S(90), row - S(2), TRUE);
    MoveWindow(work_browse_, W - m - S(84), y, S(84), row - S(2), TRUE);
    y += row + S(4);
    {
        RECT item{};
        TabCtrl_GetItemRect(tab_ctl_, 0, &item);  // only the strip of tabs: the forms sit on the window itself
        const int strip = std::max<int>(item.bottom + S(4), S(26));
        MoveWindow(tab_ctl_, m, y, W - 2 * m, strip, TRUE);
        y += strip + S(8);
    }
    int used = 0;
    if (on_runs_tab()) {
        used = panels_.back().layout(m, y, W - 2 * m, false);
        const int list_h = std::max(S(120), H - y - used - S(60) - S(8) - m - S(190));
        MoveWindow(runs_list_, m, y + used + S(4), W - 2 * m, list_h, TRUE);
        MoveWindow(runs_refresh_, m, y + used + S(8) + list_h, S(84), S(26), TRUE);
        used += list_h + S(40);
    } else {
        used = panels_[std::size_t(tab_)].layout(m, y, W - 2 * m, advanced_);
        if (panels_[std::size_t(tab_)].has_advanced()) {
            MoveWindow(more_opts_, m, y + used, S(160), S(22), TRUE);
            used += S(26);
        }
    }
    form_bottom_ = y + used + S(4);
    y = form_bottom_;
    // Monitor.
    MoveWindow(status_, m, y, W - 2 * m, S(22), TRUE);
    y += S(24);
    MoveWindow(progress_, m, y, W - 2 * m, S(16), TRUE);
    y += S(20);
    MoveWindow(info_, m, y, W - 2 * m, S(20), TRUE);
    y += S(24);
    const int bw = S(92), bh = S(28), gap = S(6);
    int x = m;
    for (HWND b : {b_start_, b_cancel_, b_resume_}) {
        MoveWindow(b, x, y, bw, bh, TRUE);
        x += bw + gap;
    }
    MoveWindow(more_min_label_, x, y, S(64), bh, TRUE);
    x += S(68);
    MoveWindow(more_min_, x, y + S(2), S(52), bh - S(4), TRUE);
    x += S(52) + gap * 2;
    for (HWND b : {b_folder_, b_play_, b_report_, b_spectrogram_}) {
        MoveWindow(b, x, y, bw, bh, TRUE);
        x += bw + gap;
    }
    y += bh + S(8);
    const int area_h = std::max(S(120), H - y - m);
    const int log_h = std::max(S(60), area_h * 3 / 10);
    const int top_h = area_h - log_h - S(6);
    const int graph_w = (W - 2 * m - S(6)) * 5 / 11;
    MoveWindow(graph_, m, y, graph_w, top_h, TRUE);
    MoveWindow(table_, m + graph_w + S(6), y, W - 2 * m - graph_w - S(6), top_h, TRUE);
    MoveWindow(log_, m, y + top_h + S(6), W - 2 * m, log_h, TRUE);
}

void App::update_buttons() {
    const bool active = model_.active();
    const bool form_tab = !on_runs_tab();
    EnableWindow(b_start_, !active && form_tab);
    EnableWindow(b_cancel_, active);
    SetWindowTextW(b_cancel_, model_.state == RunState::Cancelling ? L"Force stop" : L"Cancel");
    const bool resumable = !active && (model_.state == RunState::Cancelled || model_.state == RunState::Failed) && model_.resumable &&
                           (!model_.run_dir.empty() || model_.command == "harvest");
    EnableWindow(b_resume_, resumable);
    const std::string dir = !model_.run_dir.empty() ? model_.run_dir : model_.library;
    EnableWindow(b_folder_, !dir.empty());
    EnableWindow(b_play_, model_.files.count("ab") != 0);
    EnableWindow(b_report_, model_.files.count("report") != 0);
    EnableWindow(b_spectrogram_, model_.files.count("spectrogram") != 0);
}

void App::refresh() {
    set_text(status_, model_.status_line());
    SendMessageW(progress_, PBM_SETPOS, WPARAM(model_.overall_fraction() * 1000), 0);
    std::string info;
    if (model_.state != RunState::Idle) {
        if (model_.command == "fit" || model_.command == "transfer") {
            const double eta = model_.eta_seconds();
            info = std::format("Elapsed {}{}   |   {:.0f} evals/s   |   {} evaluations", format_duration(model_.seconds),
                               model_.time_cap_s > 0 ? " of up to " + format_duration(model_.time_cap_s) : std::string(), model_.evals_per_s, model_.evals);
            if (eta >= 0) info += "   |   about " + format_duration(eta) + " left";
            if (model_.command == "transfer" && model_.presets > 0) info = std::format("{} of {} presets done   |   ", model_.presets_done, model_.presets) + info;
        } else if (model_.command == "harvest") {
            info = std::format("{} presets verified   |   {} tried", model_.harvest_ok, model_.harvested.size());
        }
    }
    set_text(info_, info);
    // Table mode follows the command.
    const TableMode want = model_.command == "harvest" ? TableMode::Presets : model_.command == "transfer" ? TableMode::Bank : TableMode::Stages;
    if (want != table_mode_) set_table_mode(want);
    ListView_SetItemCountEx(table_, row_count(), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(table_, nullptr, FALSE);
    if (table_mode_ != TableMode::Presets && row_count() > 0) ListView_EnsureVisible(table_, row_count() - 1, FALSE);
    LossGraph::set_data(graph_, &model_.history, model_.time_cap_s,
                        model_.command == "transfer" && !model_.preset_name.empty() ? std::format("preset {}: {}", model_.preset, model_.preset_name) : std::string("best loss (dB) over time"));
    log_append_all();
    update_buttons();
    dirty_ = false;
}

void App::log_append_all() {
    // The model's log is capped and may have dropped old lines; show it whole when it shrank, else append.
    if (log_shown_ > model_.log.size()) {
        set_text(log_, "");
        log_shown_ = 0;
    }
    if (log_shown_ == model_.log.size()) return;
    std::wstring chunk;
    for (std::size_t i = log_shown_; i < model_.log.size(); ++i) chunk += widen(model_.log[i]) + L"\r\n";
    log_shown_ = model_.log.size();
    const int len = GetWindowTextLengthW(log_);
    SendMessageW(log_, EM_SETSEL, len, len);
    SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(chunk.c_str()));
    SendMessageW(log_, WM_VSCROLL, SB_BOTTOM, 0);
}

void App::start_run(bool resume, const std::string& run_dir) {
    if (model_.active()) return;
    const Form* form = nullptr;
    Values values;
    if (!run_dir.empty() && on_runs_tab()) {
        // A run chosen in the list: its kind says which form resumes it.
        for (const auto& r : runs_)
            if (r["dir"].as_string() == run_dir) form = find_form(r["kind"].as_string());
        if (!form) return;
        for (std::size_t k = 0; k < forms().size(); ++k)
            if (&forms()[k] == form) values = panels_[k].values();
    } else if (resume && !model_.command.empty()) {
        form = find_form(active_form_id_);
        if (!form) return;
        for (std::size_t k = 0; k < forms().size(); ++k)
            if (&forms()[k] == form) values = panels_[k].values();
    } else {
        form = current_form();
        if (!form) return;
        values = current_values();
    }
    if (!resume) {
        const auto missing = missing_fields(*form, values);
        if (!missing.empty()) {
            std::string m = "Please fill in:\n";
            for (const auto& s : missing) m += "  " + s + "\n";
            MessageBoxW(hwnd_, widen(m).c_str(), L"Mimicry", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    const fs::path exe = mimicry_exe();
    std::error_code ec;
    if (!fs::exists(exe, ec)) {
        MessageBoxW(hwnd_, widen("mimicry.exe was not found next to this program:\n" + exe.string()).c_str(), L"Mimicry", MB_OK | MB_ICONERROR);
        return;
    }
    fs::create_directories(work_dir(), ec);
    double more = 0;
    {
        const std::string t = get_text(more_min_);
        if (!t.empty()) more = std::atof(t.c_str());
    }
    std::vector<std::string> args;
    if (resume) args = build_resume_args(*form, run_dir.empty() ? model_.run_dir : run_dir, values, more);
    else args = build_args(*form, values);
    save_settings();
    model_.begin(form->command);
    active_form_id_ = form->id;
    log_shown_ = 0;
    set_text(log_, "");
    model_.log.push_back("> mimicry " + [&] { std::string s; for (const auto& a : args) s += (s.empty() ? "" : " ") + a; return s; }());
    child_ = std::make_unique<ChildProcess>();
    HWND h = hwnd_;
    auto r = child_->start(exe, args,
                           [h](const std::string& line, bool err) { PostMessageW(h, WM_APP_LINE, err ? 1 : 0, reinterpret_cast<LPARAM>(new std::string(line))); },
                           [h](int code) { PostMessageW(h, WM_APP_EXIT, WPARAM(code), 0); }, work_dir());
    if (!r) {
        model_.state = RunState::Failed;
        model_.error = r.error();
        child_.reset();
    }
    refresh();
}

void App::send_cancel() {
    if (!child_ || !model_.active()) return;
    if (model_.state == RunState::Cancelling) {  // the second press: no more waiting
        if (MessageBoxW(hwnd_, L"Force-stop the run? The results of the current batch are lost; the last checkpoint stays and the run can be resumed.",
                        L"Mimicry", MB_YESNO | MB_ICONWARNING) == IDYES)
            child_->terminate();
        return;
    }
    model_.state = RunState::Cancelling;
    child_->send_line("cancel");
    refresh();
}

void App::on_line(bool err, std::string* line) {
    std::unique_ptr<std::string> owned(line);
    model_.apply_line(*owned, err);
    // A cancel request in flight keeps its state until the done event.
    dirty_ = true;
}

void App::on_exit(int code) {
    model_.process_exited(code);
    child_.reset();  // joins the finished threads
    dirty_ = true;
    refresh();
    if (closing_) DestroyWindow(hwnd_);
    else if (model_.state == RunState::Finished || model_.state == RunState::Cancelled) FlashWindow(hwnd_, TRUE);
}

void App::open_path(const std::string& path) {
    if (path.empty()) return;
    ShellExecuteW(hwnd_, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void App::refresh_runs() {
    if (runs_busy_.exchange(true)) return;
    const fs::path exe = mimicry_exe();
    std::string dir = panels_.back().values()["runs.dir"];
    if (dir.empty()) dir = "runs";
    const fs::path cwd = work_dir();
    HWND h = hwnd_;
    std::thread([this, exe, dir, cwd, h] {
        int code = 0;
        // `runs` lists the folder relative to the work folder, like every other command the window starts.
        auto out = ChildProcess::capture(exe, {"runs", "--json", "--out", dir}, &code, cwd);
        PostMessageW(h, WM_APP_RUNS, 0, reinterpret_cast<LPARAM>(new std::string(out && code == 0 ? *out : std::string("[]"))));
        runs_busy_ = false;
    }).detach();
}

void App::fill_runs(const std::string& json_text) {
    ListView_DeleteAllItems(runs_list_);
    runs_.clear();
    auto v = json::parse(json_text);
    if (!v) return;
    for (const auto& r : v->as_array()) runs_.push_back(r);
    int i = 0;
    for (const auto& r : runs_) {
        auto add = [&](int col, const std::string& t) {
            const std::wstring w = widen(t);
            LVITEMW it{};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.iSubItem = col;
            it.pszText = const_cast<LPWSTR>(w.c_str());
            if (col == 0) SendMessageW(runs_list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
            else SendMessageW(runs_list_, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&it));
        };
        add(0, r["status"].as_string() + (r["resumable"].as_bool() ? " (can resume)" : ""));
        add(1, r["kind"].as_string());
        add(2, fs::path(r["plugin"].as_string()).filename().string());
        add(3, fs::path(r["target"].as_string()).filename().string());
        add(4, r["loss_db"].is_number() ? std::format("{:.3f}", r["loss_db"].as_number()) : std::string());
        add(5, std::to_string(long(r["evals"].as_number())));
        add(6, format_duration(r["elapsed_s"].as_number()));
        add(7, r["dir"].as_string());
        ++i;
    }
}

void App::on_close() {
    if (model_.active()) {
        if (MessageBoxW(hwnd_, L"A run is still active. Cancel it, keep the results so far and close?", L"Mimicry", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        closing_ = true;
        if (model_.state != RunState::Cancelling) send_cancel();
        SetTimer(hwnd_, kForceTimer, 45000, nullptr);  // a run that does not answer is ended
        return;
    }
    save_settings();
    DestroyWindow(hwnd_);
}

LRESULT App::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: return 0;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED && tab_ctl_) layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = S(820);
            mm->ptMinTrackSize.y = S(640);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimer && (dirty_ || model_.active())) refresh();
            else if (wp == kForceTimer && child_) {
                KillTimer(hwnd, kForceTimer);
                child_->terminate();
            }
            return 0;
        case WM_APP_LINE: on_line(wp != 0, reinterpret_cast<std::string*>(lp)); return 0;
        case WM_APP_EXIT: on_exit(int(wp)); return 0;
        case WM_APP_RUNS: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
            fill_runs(*text);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n && !on_runs_tab(); ++i) {
                wchar_t path[MAX_PATH * 2];
                DragQueryFileW(drop, i, path, MAX_PATH * 2);
                panels_[std::size_t(tab_)].drop(fs::path(path));
            }
            DragFinish(drop);
            return 0;
        }
        case WM_NOTIFY: {
            auto* nh = reinterpret_cast<NMHDR*>(lp);
            if (nh->idFrom == IDC_TAB && nh->code == TCN_SELCHANGE) {
                select_tab(TabCtrl_GetCurSel(tab_ctl_));
                return 0;
            }
            if (nh->idFrom == IDC_TABLE && nh->code == LVN_GETDISPINFOW) {
                auto* di = reinterpret_cast<NMLVDISPINFOW*>(lp);
                if (di->item.mask & LVIF_TEXT) {
                    static thread_local std::wstring buf;
                    buf = widen(cell_text(di->item.iItem, di->item.iSubItem));
                    di->item.pszText = buf.data();
                }
                return 0;
            }
            if (nh->idFrom == IDC_RUNS_LIST && nh->code == NM_DBLCLK) {
                const int sel = ListView_GetNextItem(runs_list_, -1, LVNI_SELECTED);
                if (sel >= 0 && std::size_t(sel) < runs_.size() && runs_[std::size_t(sel)]["resumable"].as_bool()) {
                    // The run's own folder is relative to the work folder.
                    start_run(true, runs_[std::size_t(sel)]["dir"].as_string());
                } else if (sel >= 0 && std::size_t(sel) < runs_.size()) {
                    open_path((work_dir() / fs::path(widen(runs_[std::size_t(sel)]["dir"].as_string()))).string());
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == 0) {
                for (auto& p : panels_)
                    if (p.owns(id) && p.on_browse(id, hwnd)) return 0;
                switch (id) {
                    case IDC_WORKBROWSE: {
                        wchar_t path[MAX_PATH] = {};
                        BROWSEINFOW bi{};
                        bi.hwndOwner = hwnd;
                        bi.pszDisplayName = path;
                        bi.lpszTitle = L"Choose the work folder";
                        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                        if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
                            if (SHGetPathFromIDListW(pidl, path)) set_text(workdir_, narrow(path));
                            CoTaskMemFree(pidl);
                        }
                        return 0;
                    }
                    case IDC_MORE_OPTIONS:
                        advanced_ = SendMessageW(more_opts_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                        select_tab(tab_);
                        return 0;
                    case IDC_START: start_run(false, {}); return 0;
                    case IDC_CANCEL: send_cancel(); return 0;
                    case IDC_RESUME: start_run(true, {}); return 0;
                    case IDC_OPEN_FOLDER: {
                        const std::string d = !model_.run_dir.empty() ? model_.run_dir : model_.library;
                        open_path((work_dir() / fs::path(widen(d))).string());
                        return 0;
                    }
                    case IDC_PLAY: {
                        const auto it = model_.files.find("ab");
                        if (it != model_.files.end()) open_path((work_dir() / fs::path(widen(it->second))).string());
                        return 0;
                    }
                    case IDC_SPECTROGRAM: {
                        const auto it = model_.files.find("spectrogram");
                        if (it != model_.files.end()) open_path((work_dir() / fs::path(widen(it->second))).string());
                        return 0;
                    }
                    case IDC_REPORT: {
                        const auto it = model_.files.find("report");
                        if (it != model_.files.end()) open_path((work_dir() / fs::path(widen(it->second))).string());
                        return 0;
                    }
                    case IDC_RUNS_REFRESH: refresh_runs(); return 0;
                }
            }
            break;
        }
        case WM_CLOSE: on_close(); return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimer);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK App::static_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) g_app->hwnd_ = hwnd;
    return g_app ? g_app->proc(hwnd, msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

int App::run(HINSTANCE instance, int show) {
    inst_ = instance;
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    OleInitialize(nullptr);
    LossGraph::register_class(instance);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW bold = ncm.lfMessageFont;
    bold.lfWeight = FW_SEMIBOLD;
    bold_ = CreateFontIndirectW(&bold);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = static_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MimicryMain";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(WS_EX_ACCEPTFILES, L"MimicryMain", L"Mimicry", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, S(1120), S(860), nullptr,
                            nullptr, instance, nullptr);
    if (!hwnd_) return 1;
    create_controls();
    load_settings();
    select_tab(tab_);
    refresh();
    SetTimer(hwnd_, kTimer, 250, nullptr);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    OleUninitialize();
    return int(msg.wParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    App app;
    g_app = &app;
    return app.run(instance, show);
}

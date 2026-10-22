#include "gui/win/FormPanel.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>

namespace mimicry::gui::win {

namespace {

bool is_path_kind(FieldKind k) {
    return k == FieldKind::PluginFile || k == FieldKind::WavFile || k == FieldKind::StateFile || k == FieldKind::Folder;
}

bool has_ext(const std::filesystem::path& p, std::initializer_list<const char*> exts) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* x : exts)
        if (e == x) return true;
    return false;
}

}  // namespace

bool FormPanel::has_advanced() const {
    return std::any_of(form_.fields.begin(), form_.fields.end(), [](const Field& f) { return f.advanced; });
}

void FormPanel::create(HWND parent, HINSTANCE instance, const Form& form, int id_base, HFONT font, const std::vector<std::filesystem::path>& plugins, HWND tooltip) {
    form_ = form;  // the controls point at this copy's fields
    id_base_ = id_base;
    ctls_.assign(form_.fields.size(), Ctl{});
    for (std::size_t i = 0; i < form_.fields.size(); ++i) {
        const Field& f = form_.fields[i];
        Ctl& c = ctls_[i];
        c.field = &f;
        const int id = id_base + int(i);
        auto child = [&](const wchar_t* cls, const std::wstring& text, DWORD style, DWORD ex, int cid) {
            HWND h = CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, parent,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(cid)), instance, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return h;
        };
        if (f.kind != FieldKind::Check) c.label = child(L"STATIC", widen(f.label + (f.required ? " *" : "")), SS_LEFT | SS_CENTERIMAGE, 0, -1);
        switch (f.kind) {
            case FieldKind::Check: c.input = child(L"BUTTON", widen(f.label), BS_AUTOCHECKBOX | WS_TABSTOP, 0, id); break;
            case FieldKind::Choice: {
                c.input = child(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 0, id);
                for (const auto& ch : f.choices) SendMessageW(c.input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(widen(ch.empty() ? "(auto)" : ch).c_str()));
                break;
            }
            case FieldKind::PluginFile: {
                c.input = child(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_TABSTOP | WS_VSCROLL, 0, id);
                for (const auto& p : plugins) SendMessageW(c.input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.wstring().c_str()));
                break;
            }
            default: c.input = child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, id); break;
        }
        if (is_path_kind(f.kind)) c.browse = child(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 0, id_base + 50 + int(i));
        if (!f.hint.empty() && tooltip) {
            const std::wstring hint = widen(f.hint);
            for (HWND target : {c.input, c.label}) {
                if (!target) continue;
                TOOLINFOW ti{};
                ti.cbSize = sizeof ti;
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd = parent;
                ti.uId = reinterpret_cast<UINT_PTR>(target);
                ti.lpszText = const_cast<LPWSTR>(hint.c_str());  // the tooltip keeps its own copy
                SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
            }
        }
    }
}

void FormPanel::show(bool visible, bool advanced) {
    for (auto& c : ctls_) {
        const bool on = visible && (!c.field->advanced || advanced);
        for (HWND h : {c.label, c.input, c.browse})
            if (h) ShowWindow(h, on ? SW_SHOW : SW_HIDE);
    }
}

int FormPanel::layout(int x, int y, int width, bool advanced) {
    const int row = S(28), label_w = S(130), browse_w = S(84), gap = S(6);
    int cy = y;
    // Main fields one per row; advanced ones in two columns.
    std::vector<std::size_t> main_rows, adv;
    for (std::size_t i = 0; i < ctls_.size(); ++i) (ctls_[i].field->advanced ? adv : main_rows).push_back(i);
    for (std::size_t i : main_rows) {
        Ctl& c = ctls_[i];
        const int ih = S(24);
        if (c.field->kind == FieldKind::Check) {
            MoveWindow(c.input, x + label_w + gap, cy, width - label_w - gap, ih, TRUE);
        } else {
            MoveWindow(c.label, x, cy, label_w, ih, TRUE);
            const int iw = width - label_w - gap - (c.browse ? browse_w + gap : 0);
            MoveWindow(c.input, x + label_w + gap, cy, iw, c.field->kind == FieldKind::Choice || c.field->kind == FieldKind::PluginFile ? S(200) : ih, TRUE);
            if (c.browse) MoveWindow(c.browse, x + width - browse_w, cy, browse_w, ih, TRUE);
        }
        cy += row;
    }
    if (advanced && !adv.empty()) {
        const int col_w = (width - gap * 2) / 2;
        int col = 0, ry = cy;
        for (std::size_t i : adv) {
            Ctl& c = ctls_[i];
            const bool full = c.field->kind == FieldKind::Folder || c.field->option.empty() || c.field->kind == FieldKind::Text;
            if (full && col == 1) {
                col = 0;
                ry += row;
            }
            const int cx = x + (full ? 0 : col * (col_w + gap * 2));
            const int w = full ? width : col_w;
            const int ih = S(24);
            const int lw = full ? label_w : S(120);
            if (c.field->kind == FieldKind::Check) {
                MoveWindow(c.input, cx + lw + gap, ry, w - lw - gap, ih, TRUE);
            } else {
                MoveWindow(c.label, cx, ry, lw, ih, TRUE);
                const int iw = w - lw - gap - (c.browse ? browse_w + gap : 0);
                MoveWindow(c.input, cx + lw + gap, ry, iw, c.field->kind == FieldKind::Choice ? S(200) : ih, TRUE);
                if (c.browse) MoveWindow(c.browse, cx + w - browse_w, ry, browse_w, ih, TRUE);
            }
            if (full) {
                ry += row;
                col = 0;
            } else if (col == 0) {
                col = 1;
            } else {
                col = 0;
                ry += row;
            }
        }
        if (col == 1) ry += row;
        cy = ry;
    }
    return cy - y;
}

Values FormPanel::values() const {
    Values v;
    for (const auto& c : ctls_) {
        const Field& f = *c.field;
        switch (f.kind) {
            case FieldKind::Check: v[f.key] = SendMessageW(c.input, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : ""; break;
            case FieldKind::Choice: {
                const LRESULT sel = SendMessageW(c.input, CB_GETCURSEL, 0, 0);
                v[f.key] = sel >= 0 && std::size_t(sel) < f.choices.size() ? f.choices[std::size_t(sel)] : std::string();
                break;
            }
            default: v[f.key] = get_text(c.input); break;
        }
    }
    return v;
}

void FormPanel::set_values(const Values& values) {
    for (auto& c : ctls_) {
        const Field& f = *c.field;
        const auto it = values.find(f.key);
        const std::string v = it == values.end() ? f.def : it->second;
        switch (f.kind) {
            case FieldKind::Check: SendMessageW(c.input, BM_SETCHECK, v.empty() || v == "0" ? BST_UNCHECKED : BST_CHECKED, 0); break;
            case FieldKind::Choice: {
                const auto pos = std::find(f.choices.begin(), f.choices.end(), v);
                SendMessageW(c.input, CB_SETCURSEL, pos == f.choices.end() ? 0 : std::size_t(pos - f.choices.begin()), 0);
                break;
            }
            default: set_text(c.input, v); break;
        }
    }
}

bool FormPanel::on_browse(int command_id, HWND owner) {
    const int i = command_id - id_base_ - 50;
    if (i < 0 || std::size_t(i) >= ctls_.size() || !ctls_[std::size_t(i)].browse) return false;
    Ctl& c = ctls_[std::size_t(i)];
    const Field& f = *c.field;
    if (f.kind == FieldKind::Folder) {
        wchar_t path[MAX_PATH] = {};
        BROWSEINFOW bi{};
        bi.hwndOwner = owner;
        bi.pszDisplayName = path;
        bi.lpszTitle = L"Choose a folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
            if (SHGetPathFromIDListW(pidl, path)) set_text(c.input, narrow(path));
            CoTaskMemFree(pidl);
        }
        return true;
    }
    // Filters are "description\0pattern\0...\0\0".
    const wchar_t* filter = f.kind == FieldKind::PluginFile ? L"Plugins (*.clap;*.vst3)\0*.clap;*.vst3\0All files\0*.*\0"
                            : f.kind == FieldKind::WavFile  ? L"WAV files (*.wav)\0*.wav\0All files\0*.*\0"
                                                            : L"Presets (*.state;*.vstpreset)\0*.state;*.vstpreset\0All files\0*.*\0";
    wchar_t file[MAX_PATH * 2] = {};
    const std::wstring current = widen(get_text(c.input));
    std::copy_n(current.c_str(), std::min<std::size_t>(current.size(), MAX_PATH * 2 - 1), file);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    // A VST3 plugin can be a folder (a bundle); the dialog picks files, so the edit box also takes a typed or dropped path.
    if (GetOpenFileNameW(&ofn)) set_text(c.input, narrow(file));
    return true;
}

void FormPanel::drop(const std::filesystem::path& path) {
    std::error_code ec;
    const bool dir = std::filesystem::is_directory(path, ec);
    const bool bundle = dir && has_ext(path, {".vst3", ".clap"});
    for (int pass = 0; pass < 2; ++pass) {  // first an empty fitting field, then the first fitting one
        for (auto& c : ctls_) {
            const FieldKind k = c.field->kind;
            const bool fits = ((bundle || has_ext(path, {".clap", ".vst3"})) && k == FieldKind::PluginFile) || (has_ext(path, {".wav"}) && k == FieldKind::WavFile) ||
                              (has_ext(path, {".state", ".vstpreset"}) && k == FieldKind::StateFile) || (dir && !bundle && k == FieldKind::Folder && pass == 1 && false);
            if (!fits) continue;
            if (pass == 0 && !get_text(c.input).empty()) continue;
            set_text(c.input, path.string());
            return;
        }
    }
}

}  // namespace mimicry::gui::win

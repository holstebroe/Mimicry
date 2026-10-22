#pragma once
// The controls of one Form (gui/Forms.hpp) in the main window: made from the field table, laid out in rows,
// read and written as Values.

#include <filesystem>
#include <string>
#include <vector>

#include "gui/Forms.hpp"
#include "gui/win/WinUtil.hpp"

namespace mimicry::gui::win {

class FormPanel {
public:
    // `id_base` is the first control id of this panel (it uses id_base .. id_base + 99).
    void create(HWND parent, HINSTANCE instance, const Form& form, int id_base, HFONT font, const std::vector<std::filesystem::path>& plugins,
                HWND tooltip = nullptr);
    void show(bool visible, bool advanced);
    // Places the controls inside the box starting at (x, y) with this width; returns the height used.
    int layout(int x, int y, int width, bool advanced);
    [[nodiscard]] Values values() const;
    void set_values(const Values& values);
    // A Browse button was pressed (command id); true when it was this panel's.
    bool on_browse(int command_id, HWND owner);
    // A file dropped on the window: put it into the first fitting field.
    void drop(const std::filesystem::path& path);
    [[nodiscard]] const Form& form() const noexcept { return form_; }
    [[nodiscard]] bool has_advanced() const;
    [[nodiscard]] bool owns(int command_id) const { return command_id >= id_base_ && command_id < id_base_ + 100; }

private:
    struct Ctl {
        const Field* field = nullptr;
        HWND label = nullptr, input = nullptr, browse = nullptr;
    };
    Form form_;
    int id_base_ = 0;
    std::vector<Ctl> ctls_;
};

}  // namespace mimicry::gui::win

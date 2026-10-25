#pragma once
// A small GDI control: the best loss against run time (docs/spec/09_GUI.md section 5).

#include <vector>

#include "gui/RunModel.hpp"
#include "gui/win/WinUtil.hpp"

namespace mimicry::gui::win {

class LossGraph {
public:
    static void register_class(HINSTANCE instance);
    static HWND create(HWND parent, HINSTANCE instance, int id);

    // The window keeps no copy: `history` must outlive it (it lives in the RunModel).
    static void set_data(HWND graph, const std::vector<LossPoint>* history, double time_cap_s, const std::string& caption);
};

}  // namespace mimicry::gui::win

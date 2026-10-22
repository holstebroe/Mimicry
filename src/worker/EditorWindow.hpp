#pragma once
// The plugin's editor in a window of the worker (docs/spec/02_HOSTING.md section 3, 06_TARGETS.md section 6).
// Windows only: elsewhere `open` says so. A top-level window with a small toolbar (Name, Capture, Teach click,
// Auto, Done) above the area the plugin's view attaches to. Everything runs on the worker's main thread; its
// message pump is the worker's idle loop.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Expected.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::worker {

// Where the user clicked the plugin's "next preset" button, as fractions of the editor area so it survives the
// window being resized, with the class of the child window it hit (information for the report).
struct ClickRecord {
    double fx = 0, fy = 0;
    std::string window_class;
};

class EditorWindow {
public:
    EditorWindow();
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Creates the window and attaches the plugin's editor (`instance.open_editor`).
    [[nodiscard]] expected<host::EditorSize, std::string> open(host::Instance& instance, const std::string& title);
    void close();
    [[nodiscard]] bool is_open() const;

    // What the user asked for with the toolbar since the last call: "capture", "teach", "auto", "stop", "done".
    [[nodiscard]] std::vector<std::string> take_actions();
    // The text of the Name box; cleared by the call.
    [[nodiscard]] std::string take_name();
    void set_status(const std::string& text);
    void set_counts(int captures, bool taught, bool auto_running);
    // The plugin closed its own view (CLAP gui closed): once.
    [[nodiscard]] bool take_plugin_closed();

    // Click teaching: the next left click inside the editor is recorded (and still reaches the plugin).
    void begin_teach();
    void cancel_teach();
    [[nodiscard]] bool teaching() const;
    [[nodiscard]] std::optional<ClickRecord> take_click();
    // Replays a click inside the editor: posted mouse messages to the child window under the point, or with
    // `real_input` the real mouse (the window is brought to the front). Pumps messages for a short while.
    bool replay_click(const ClickRecord& click, bool real_input);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mimicry::worker

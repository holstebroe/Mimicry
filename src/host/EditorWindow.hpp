#pragma once
// A Win32 top-level window that holds a plugin's editor (docs/spec/02_HOSTING.md section 3). Windows only: on
// other platforms `create` returns an error. Format-agnostic (no CLAP or VST3 types); the engines give it the
// size the plugin asks for and attach the plugin's view to `handle()`.
//
// Main thread only. The window is serviced by the worker's message pump (MessagePump::pump), so it stays
// alive while the worker waits for its next command. A click on the close button hides the window and
// sets `closed_by_user()`; the engine then tears the editor down on its next `pump_main_thread()`.

#include <memory>
#include <string>

#include "core/Expected.hpp"
#include "host/PluginHost.hpp"

namespace mimicry::host {

class EditorWindow {
public:
    EditorWindow();
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // A window whose client area is `width` x `height` pixels. Hidden until show().
    [[nodiscard]] expected<void, HostError> create(const std::string& title, int width, int height);
    void show();
    // Client area size, e.g. when the plugin asks for a bigger editor.
    void resize_client(int width, int height);
    void destroy();

    [[nodiscard]] void* handle() const;          // the HWND, null if there is no window
    [[nodiscard]] bool exists() const { return handle() != nullptr; }
    [[nodiscard]] bool closed_by_user() const;   // the user pressed the close button

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mimicry::host

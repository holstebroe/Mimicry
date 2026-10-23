#pragma once
// Console of the command line (docs/spec/09_GUI.md sections 2-3): human text, or with --events one JSON
// object per line on stdout for a program to read (the GUI, an agent), plus the cancel request.
//
// Cancel (spec 04 section 10) comes from SIGINT/SIGTERM/SIGBREAK, a line "cancel" on stdin (events mode),
// the end of stdin with --cancel-on-eof (the GUI died), or a file `cancel` in the run directory
// (`mimicry cancel <run>`). A second SIGINT ends the process at once.

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "core/Json.hpp"

namespace mimicry::app {

class Console {
public:
    [[nodiscard]] static Console& get();

    void set_events(bool on) { events_ = on; }
    [[nodiscard]] bool events() const noexcept { return events_; }

    // Text for the person: stdout as is, or a {"event":"log"} line in events mode.
    void info(const std::string& text);
    // A note on stderr ("note: ..."), or {"event":"log","level":"warn"} in events mode.
    void warn(const std::string& text);
    // One event object; the "event" member names it. Does nothing outside events mode.
    void emit(json::Value event);
    // Final result of a command in events mode: {"event":"done", "status": ..., ...}.
    void done(const std::string& status, json::Value details = {});

    void install_signal_handlers();
    // Reads stdin on a thread: "cancel" requests a cancel; with `cancel_on_eof`, so does the end of input.
    void start_stdin_watcher(bool cancel_on_eof);
    // A `cancel` file in a run directory requests a cancel while it exists (polled once per render batch).
    // Several files can be watched; any of them counts.
    void watch_cancel_file(std::filesystem::path path);

    void request_cancel() { cancel_ = true; }
    [[nodiscard]] bool cancel_requested();
    void clear_cancel() { cancel_ = false; }

private:
    bool events_ = false;
    std::mutex out_mutex_;
    std::atomic<bool> cancel_{false};
    std::mutex file_mutex_;
    std::vector<std::filesystem::path> cancel_files_;
};

}  // namespace mimicry::app

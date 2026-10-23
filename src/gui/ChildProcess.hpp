#pragma once
// A child process with its standard streams as lines: the front end's way to run `mimicry --events`
// (docs/spec/09_GUI.md section 4). Windows: CreateProcess with anonymous pipes. POSIX: fork/exec (the same
// class lets the front end's logic be tested against the real command line on Linux and macOS).
//
// Handlers run on reader threads, not the caller's.

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/Expected.hpp"

namespace mimicry::gui {

class ChildProcess {
public:
    using LineHandler = std::function<void(const std::string& line, bool from_stderr)>;
    using ExitHandler = std::function<void(int exit_code)>;

    ChildProcess();
    ~ChildProcess();  // ends the process if it still runs, joins the threads
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts `exe args...` (no shell). `on_exit` runs once, after the last line has been delivered.
    [[nodiscard]] expected<void, std::string> start(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                                    LineHandler on_line, ExitHandler on_exit,
                                                    const std::filesystem::path& working_dir = {});
    // A line to the child's standard input (a "cancel" request).
    void send_line(const std::string& line);
    void close_stdin();
    // Ends the process at once (a run that does not answer a cancel). Its workers end with it.
    void terminate();
    [[nodiscard]] bool running() const noexcept { return running_.load(); }
    // Waits for the process and the threads; the exit code (-1 if it never started).
    int wait();

    // Runs to the end and returns everything printed on stdout (short tools such as `mimicry runs --json`).
    [[nodiscard]] static expected<std::string, std::string> capture(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                                                    int* exit_code = nullptr, const std::filesystem::path& working_dir = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

// The command line Windows wants for `exe args...` (CommandLineToArgvW rules), for CreateProcess.
[[nodiscard]] std::string windows_command_line(const std::string& exe, const std::vector<std::string>& args);

}  // namespace mimicry::gui

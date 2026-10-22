#include "gui/ChildProcess.hpp"

#include <cstring>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mimicry::gui {

std::string windows_command_line(const std::string& exe, const std::vector<std::string>& args) {
    auto quote = [](const std::string& a) {
        if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
        std::string out = "\"";
        std::size_t backslashes = 0;
        for (const char c : a) {
            if (c == '\\') {
                ++backslashes;
                continue;
            }
            if (c == '"') out.append(backslashes * 2 + 1, '\\');
            else out.append(backslashes, '\\');
            backslashes = 0;
            out += c;
        }
        out.append(backslashes * 2, '\\');  // before the closing quote
        return out + "\"";
    };
    std::string line = quote(exe);
    for (const auto& a : args) line += " " + quote(a);
    return line;
}

struct ChildProcess::Impl {
    std::thread out_thread, err_thread, wait_thread;
    std::mutex write_mutex;
    int exit_code = -1;
    bool started = false;
#if defined(_WIN32)
    HANDLE process = nullptr, in_write = nullptr, out_read = nullptr, err_read = nullptr;
#else
    pid_t pid = -1;
    int in_write = -1, out_read = -1, err_read = -1;
#endif
};

namespace {

// Delivers complete lines from a blocking read function; a trailing partial line at the end of the stream too.
template <class Read>
void pump_lines(Read read, bool from_stderr, const ChildProcess::LineHandler& handler) {
    std::string pending;
    char buf[4096];
    for (;;) {
        const long n = read(buf, sizeof buf);
        if (n <= 0) break;
        pending.append(buf, std::size_t(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (handler) handler(line, from_stderr);
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty() && handler) handler(pending, from_stderr);
}

}  // namespace

ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}

ChildProcess::~ChildProcess() {
    if (running_) terminate();
    wait();
}

#if defined(_WIN32)

expected<void, std::string> ChildProcess::start(const std::filesystem::path& exe, const std::vector<std::string>& args, LineHandler on_line,
                                                ExitHandler on_exit, const std::filesystem::path& working_dir) {
    // Only the child's three ends are inheritable, and the child inherits exactly those (an explicit handle
    // list): another process started at the same time by this program must not hold on to our pipes.
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
    auto close_all = [&] {
        for (HANDLE h : {in_r, in_w, out_r, out_w, err_r, err_w})
            if (h) CloseHandle(h);
    };
    if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0)) {
        close_all();
        return unexpected(std::string("cannot create pipes"));
    }
    for (HANDLE h : {in_r, out_w, err_w}) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherit[3] = {in_r, out_w, err_w};
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr)) {
        close_all();
        return unexpected(std::string("cannot prepare the child's handles"));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = out_w;
    si.StartupInfo.hStdError = err_w;
    si.lpAttributeList = attrs;
    const std::string cmd8 = windows_command_line(exe.string(), args);
    std::wstring cmd;
    {  // the ANSI code page, like the rest of the command line (see gui/win/WinUtil.hpp)
        const int n = MultiByteToWideChar(CP_ACP, 0, cmd8.c_str(), -1, nullptr, 0);
        cmd.assign(std::size_t(n > 0 ? n : 1), L'\0');
        MultiByteToWideChar(CP_ACP, 0, cmd8.c_str(), -1, cmd.data(), n);
    }
    const std::wstring cwd = working_dir.wstring();
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        const DWORD err = GetLastError();
        CloseHandle(in_w);
        CloseHandle(out_r);
        CloseHandle(err_r);
        return unexpected("cannot start " + exe.string() + " (error " + std::to_string(err) + ")");
    }
    CloseHandle(pi.hThread);
    impl_->process = pi.hProcess;
    impl_->in_write = in_w;
    impl_->out_read = out_r;
    impl_->err_read = err_r;
    impl_->started = true;
    running_ = true;
    auto reader = [](HANDLE h) {
        return [h](char* buf, std::size_t n) -> long {
            DWORD got = 0;
            if (!ReadFile(h, buf, DWORD(n), &got, nullptr) || got == 0) return -1;
            return long(got);
        };
    };
    impl_->out_thread = std::thread([=, this] { pump_lines(reader(impl_->out_read), false, on_line); });
    impl_->err_thread = std::thread([=, this] { pump_lines(reader(impl_->err_read), true, on_line); });
    impl_->wait_thread = std::thread([=, this] {
        WaitForSingleObject(impl_->process, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(impl_->process, &code);
        impl_->out_thread.join();
        impl_->err_thread.join();
        impl_->exit_code = int(code);
        running_ = false;
        if (on_exit) on_exit(int(code));
    });
    return {};
}

void ChildProcess::send_line(const std::string& line) {
    std::lock_guard lock(impl_->write_mutex);
    if (!impl_->in_write) return;
    const std::string text = line + "\n";
    DWORD wrote = 0;
    WriteFile(impl_->in_write, text.data(), DWORD(text.size()), &wrote, nullptr);
}

void ChildProcess::close_stdin() {
    std::lock_guard lock(impl_->write_mutex);
    if (impl_->in_write) CloseHandle(impl_->in_write);
    impl_->in_write = nullptr;
}

void ChildProcess::terminate() {
    if (impl_->started && running_) TerminateProcess(impl_->process, 1);
}

int ChildProcess::wait() {
    if (impl_->wait_thread.joinable()) impl_->wait_thread.join();
    if (impl_->started) {
        close_stdin();
        if (impl_->out_read) CloseHandle(impl_->out_read);
        if (impl_->err_read) CloseHandle(impl_->err_read);
        if (impl_->process) CloseHandle(impl_->process);
        impl_->out_read = impl_->err_read = impl_->process = nullptr;
        impl_->started = false;
    }
    return impl_->exit_code;
}

#else  // POSIX

expected<void, std::string> ChildProcess::start(const std::filesystem::path& exe, const std::vector<std::string>& args, LineHandler on_line,
                                                ExitHandler on_exit, const std::filesystem::path& working_dir) {
    int in_p[2], out_p[2], err_p[2];
    if (pipe(in_p) != 0 || pipe(out_p) != 0 || pipe(err_p) != 0) return unexpected(std::string("cannot create pipes"));
    for (int fd : {in_p[1], out_p[0], err_p[0]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<std::string> argv_s{exe.string()};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : argv_s) argv.push_back(s.data());
    argv.push_back(nullptr);
    const std::string cwd = working_dir.string();
    const pid_t pid = fork();
    if (pid < 0) return unexpected(std::string("fork failed"));
    if (pid == 0) {
        dup2(in_p[0], 0);
        dup2(out_p[1], 1);
        dup2(err_p[1], 2);
        for (int fd : {in_p[0], in_p[1], out_p[0], out_p[1], err_p[0], err_p[1]}) close(fd);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(in_p[0]);
    close(out_p[1]);
    close(err_p[1]);
    signal(SIGPIPE, SIG_IGN);
    impl_->pid = pid;
    impl_->in_write = in_p[1];
    impl_->out_read = out_p[0];
    impl_->err_read = err_p[0];
    impl_->started = true;
    running_ = true;
    auto reader = [](int fd) {
        return [fd](char* buf, std::size_t n) -> long { return long(read(fd, buf, n)); };
    };
    impl_->out_thread = std::thread([=, this] { pump_lines(reader(impl_->out_read), false, on_line); });
    impl_->err_thread = std::thread([=, this] { pump_lines(reader(impl_->err_read), true, on_line); });
    impl_->wait_thread = std::thread([=, this] {
        int status = 0;
        while (waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {}
        impl_->out_thread.join();
        impl_->err_thread.join();
        impl_->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        running_ = false;
        if (on_exit) on_exit(impl_->exit_code);
    });
    return {};
}

void ChildProcess::send_line(const std::string& line) {
    std::lock_guard lock(impl_->write_mutex);
    if (impl_->in_write < 0) return;
    const std::string text = line + "\n";
    const ssize_t n = write(impl_->in_write, text.data(), text.size());
    (void)n;
}

void ChildProcess::close_stdin() {
    std::lock_guard lock(impl_->write_mutex);
    if (impl_->in_write >= 0) close(impl_->in_write);
    impl_->in_write = -1;
}

void ChildProcess::terminate() {
    if (impl_->started && running_) kill(impl_->pid, SIGKILL);
}

int ChildProcess::wait() {
    if (impl_->wait_thread.joinable()) impl_->wait_thread.join();
    if (impl_->started) {
        close_stdin();
        if (impl_->out_read >= 0) close(impl_->out_read);
        if (impl_->err_read >= 0) close(impl_->err_read);
        impl_->out_read = impl_->err_read = -1;
        impl_->started = false;
    }
    return impl_->exit_code;
}

#endif

expected<std::string, std::string> ChildProcess::capture(const std::filesystem::path& exe, const std::vector<std::string>& args, int* exit_code,
                                                const std::filesystem::path& working_dir) {
    ChildProcess p;
    std::mutex m;
    std::string out;
    auto r = p.start(exe, args, [&](const std::string& line, bool from_stderr) {
        if (from_stderr) return;
        std::lock_guard lock(m);
        out += line + "\n";
    }, nullptr, working_dir);
    if (!r) return unexpected(r.error());
    p.close_stdin();
    const int code = p.wait();
    if (exit_code) *exit_code = code;
    return out;
}

}  // namespace mimicry::gui

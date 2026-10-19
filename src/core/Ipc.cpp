#include "core/Ipc.hpp"

#include <atomic>
#include <format>
#include <mutex>

#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace mimicry::ipc {

namespace {
int fd_of(Handle h) { return static_cast<int>(h); }

#if !defined(__linux__)
// Serialises descriptor creation and spawning where descriptors cannot be made close-on-exec atomically.
std::mutex& spawn_mutex() {
    static std::mutex m;
    return m;
}
#endif
}  // namespace

void close_handle(Handle h) {
    if (h >= 0) ::close(fd_of(h));
}

bool LineChannel::write_line(std::string_view line) {
    std::string msg(line);
    msg += '\n';
    std::size_t done = 0;
    while (done < msg.size()) {
        const ssize_t n = ::write(fd_of(write_), msg.data() + done, msg.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

LineChannel::Read LineChannel::read_line(std::string& out, int timeout_ms) {
    for (;;) {
        const auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            out = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return Read::Line;
        }
        pollfd p{fd_of(read_), POLLIN, 0};
        const int r = ::poll(&p, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return Read::Closed;
        }
        if (r == 0) return Read::Timeout;
        char chunk[4096];
        const ssize_t n = ::read(fd_of(read_), chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Read::Closed;
        }
        if (n == 0) return Read::Closed;
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

void LineChannel::close() {
    close_handle(read_);
    if (write_ != read_) close_handle(write_);
    read_ = write_ = kNoHandle;
    buffer_.clear();
}

// --- SharedMemory ----------------------------------------------------------------------------

SharedMemory::~SharedMemory() { release(); }

void SharedMemory::release() {
    if (data_) ::munmap(data_, size_);
    close_handle(handle_);
    data_ = nullptr;
    handle_ = kNoHandle;
    size_ = 0;
}

expected<SharedMemory, std::string> SharedMemory::from_handle(Handle h, std::size_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_of(h), 0);
    if (p == MAP_FAILED) return unexpected(std::format("mmap failed: {}", std::strerror(errno)));
    SharedMemory m;
    m.handle_ = h;
    m.data_ = p;
    m.size_ = bytes;
    return m;
}

expected<SharedMemory, std::string> SharedMemory::create(std::size_t bytes) {
    static std::atomic<unsigned> counter{0};
    const std::string name = std::format("/mimicry-{}-{}", ::getpid(), counter++);
#if defined(__linux__)
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
#else
    // macOS shm_open rejects O_CLOEXEC (EINVAL): set it right after, under the lock spawning also takes.
    int fd = -1;
    {
        std::lock_guard lock(spawn_mutex());
        fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (fd < 0) return unexpected(std::format("shm_open failed: {}", std::strerror(errno)));
    ::shm_unlink(name.c_str());  // nothing can leak from here on: the memory lives while a descriptor is open
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        const std::string e = std::strerror(errno);
        ::close(fd);
        return unexpected(std::format("ftruncate failed: {}", e));
    }
    auto m = from_handle(fd, bytes);
    if (!m) ::close(fd);
    return m;
}

// --- Process ---------------------------------------------------------------------------------

namespace {
std::string describe_status(int status) {
    if (WIFEXITED(status)) return std::format("exited with status {}", WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return std::format("killed by signal {}", WTERMSIG(status));
    return "ended";
}
}  // namespace

bool make_pipe(Handle& read_end, Handle& write_end) {
    int fds[2];
#if defined(__linux__)
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
#else
    {
        std::lock_guard lock(spawn_mutex());
        if (::pipe(fds) != 0) return false;
        ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    }
#endif
    read_end = fds[0];
    write_end = fds[1];
    return true;
}

std::map<int, Handle> inherited_handles(int, char**) {
    return {{3, 3}, {4, 4}, {5, 5}};
}

Process::~Process() {
    if (pid_ > 0) (void)kill_and_reap();
}

Process& Process::operator=(Process&& o) noexcept {
    if (this != &o) {
        if (pid_ > 0) (void)kill_and_reap();
        pid_ = o.pid_;
        o.pid_ = -1;
    }
    return *this;
}

expected<Process, std::string> Process::spawn(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                              const std::vector<HandleMapping>& handles) {
    // Move every source above the target range first, so no mapping can overwrite another's source.
    std::vector<int> staged;
    auto cleanup = [&] {
        for (const int fd : staged) ::close(fd);
    };
    for (const auto& m : handles) {
        const int d = ::fcntl(fd_of(m.from), F_DUPFD_CLOEXEC, 32);
        if (d < 0) {
            cleanup();
            return unexpected(std::format("fcntl failed: {}", std::strerror(errno)));
        }
        staged.push_back(d);
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    for (std::size_t i = 0; i < handles.size(); ++i) posix_spawn_file_actions_adddup2(&actions, staged[i], handles[i].to);

    std::vector<std::string> storage{exe.string()};
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = 0;
    int rc = 0;
    {
#if !defined(__linux__)
        std::lock_guard lock(spawn_mutex());
#endif
        rc = ::posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    cleanup();
    if (rc != 0) return unexpected(std::format("cannot start '{}': {}", exe.string(), std::strerror(rc)));
    Process p;
    p.pid_ = pid;
    return p;
}

bool Process::running() {
    if (pid_ <= 0) return false;
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == 0) return true;
    pid_ = -1;  // reaped (or gone)
    return false;
}

std::string Process::kill_and_reap() {
    if (pid_ <= 0) return "not running";
    ::kill(static_cast<pid_t>(pid_), SIGKILL);
    return reap();
}

std::string Process::reap() {
    if (pid_ <= 0) return "not running";
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    } while (r < 0 && errno == EINTR);
    pid_ = -1;
    return r < 0 ? "gone" : describe_status(status);
}

std::filesystem::path executable_dir() {
#if defined(__linux__)
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    std::uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) return std::filesystem::path(buf).parent_path();
#endif
    return std::filesystem::current_path();
}

}  // namespace mimicry::ipc

#else  // _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace mimicry::ipc {

namespace {

HANDLE win(Handle h) { return reinterpret_cast<HANDLE>(h); }
Handle ours(HANDLE h) { return reinterpret_cast<Handle>(h); }
bool valid(Handle h) { return h != kNoHandle && h != 0; }

std::string last_error(const char* what) { return std::format("{} failed (error {})", what, ::GetLastError()); }

// Overlapped I/O on one handle, waiting at most `timeout_ms` (< 0: forever). Returns bytes moved, 0 on timeout,
// -1 when the pipe is broken. A timed-out read is cancelled; data that arrived meanwhile still counts.
long overlapped_io(Handle h, bool write, void* data, DWORD size, int timeout_ms) {
    OVERLAPPED ov{};
    ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;
    DWORD n = 0;
    const BOOL ok = write ? ::WriteFile(win(h), data, size, nullptr, &ov) : ::ReadFile(win(h), data, size, nullptr, &ov);
    long result = -1;
    if (ok || ::GetLastError() == ERROR_IO_PENDING) {
        const DWORD wait = ::WaitForSingleObject(ov.hEvent, timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
        if (wait == WAIT_TIMEOUT) {
            ::CancelIoEx(win(h), &ov);
            if (::GetOverlappedResult(win(h), &ov, &n, TRUE)) result = long(n);
            else result = ::GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
        } else if (::GetOverlappedResult(win(h), &ov, &n, FALSE)) {
            result = long(n);
        }
    }
    ::CloseHandle(ov.hEvent);
    return result;
}

// Workers belong to one job object that kills them when the supervisor's last handle to it closes, i.e. when
// the supervisor dies (the counterpart of PR_SET_PDEATHSIG).
HANDLE worker_job() {
    static HANDLE job = [] {
        HANDLE j = ::CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(j, JobObjectExtendedLimitInformation, &info, sizeof info);
        }
        return j;
    }();
    return job;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// One argument quoted for CommandLineToArgvW.
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') q.append(backslashes * 2 + 1, L'\\');
        else q.append(backslashes, L'\\');
        backslashes = 0;
        q.push_back(c);
    }
    q.append(backslashes * 2, L'\\');
    return q + L"\"";
}

std::string describe_exit(DWORD code) {
    if (code == 0xC0000005u) return "access violation (0xC0000005)";
    if (code == 0xC00000FDu) return "stack overflow (0xC00000FD)";
    if (code >= 0xC0000000u) return std::format("exception 0x{:08X}", code);
    return std::format("exited with status {}", code);
}

}  // namespace

void close_handle(Handle h) {
    if (valid(h)) ::CloseHandle(win(h));
}

bool LineChannel::write_line(std::string_view line) {
    std::string msg(line);
    msg += '\n';
    std::size_t done = 0;
    while (done < msg.size()) {
        const long n = overlapped_io(write_, true, msg.data() + done, DWORD(msg.size() - done), -1);
        if (n <= 0) return false;
        done += std::size_t(n);
    }
    return true;
}

LineChannel::Read LineChannel::read_line(std::string& out, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        const auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            out = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            return Read::Line;
        }
        int wait = -1;
        if (timeout_ms >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) return Read::Timeout;
            wait = int(left);
        }
        char chunk[4096];
        const long n = overlapped_io(read_, false, chunk, sizeof chunk, wait);
        if (n < 0) return Read::Closed;
        if (n == 0) {
            if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= deadline) return Read::Timeout;
            continue;
        }
        buffer_.append(chunk, std::size_t(n));
    }
}

void LineChannel::close() {
    close_handle(read_);
    if (write_ != read_) close_handle(write_);
    read_ = write_ = kNoHandle;
    buffer_.clear();
}

// --- SharedMemory ----------------------------------------------------------------------------

SharedMemory::~SharedMemory() { release(); }

void SharedMemory::release() {
    if (data_) ::UnmapViewOfFile(data_);
    close_handle(handle_);
    data_ = nullptr;
    handle_ = kNoHandle;
    size_ = 0;
}

expected<SharedMemory, std::string> SharedMemory::from_handle(Handle h, std::size_t bytes) {
    void* p = ::MapViewOfFile(win(h), FILE_MAP_ALL_ACCESS, 0, 0, bytes);
    if (!p) return unexpected(last_error("MapViewOfFile"));
    SharedMemory m;
    m.handle_ = h;
    m.data_ = p;
    m.size_ = bytes;
    return m;
}

expected<SharedMemory, std::string> SharedMemory::create(std::size_t bytes) {
    const auto size = static_cast<std::uint64_t>(bytes);
    HANDLE h = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(size >> 32), DWORD(size & 0xffffffffu),
                                    nullptr);  // unnamed: gone with its last handle
    if (!h) return unexpected(last_error("CreateFileMapping"));
    auto m = from_handle(ours(h), bytes);
    if (!m) ::CloseHandle(h);
    return m;
}

// --- Process ---------------------------------------------------------------------------------

bool make_pipe(Handle& read_end, Handle& write_end) {
    // A named pipe, so both ends can do overlapped I/O (timeouts); not inheritable: Process::spawn passes the
    // child's ends through an explicit handle list.
    static std::atomic<unsigned> counter{0};
    const std::wstring name =
        widen(std::format("\\\\.\\pipe\\mimicry-{}-{}-{}", ::GetCurrentProcessId(), counter++, ::GetTickCount64()));
    HANDLE r = ::CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1 << 16,
                                  1 << 16, 0, nullptr);
    if (r == INVALID_HANDLE_VALUE) return false;
    HANDLE w = ::CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        ::CloseHandle(r);
        return false;
    }
    read_end = ours(r);
    write_end = ours(w);
    return true;
}

std::map<int, Handle> inherited_handles(int argc, char** argv) {
    std::map<int, Handle> out;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--handle") != 0) continue;
        const char* v = argv[++i];
        char* end = nullptr;
        const long to = std::strtol(v, &end, 10);
        if (!end || *end != '=') continue;
        out[int(to)] = Handle(std::strtoll(end + 1, nullptr, 10));
    }
    return out;
}

Process::~Process() {
    if (valid(handle_)) (void)kill_and_reap();
}

Process& Process::operator=(Process&& o) noexcept {
    if (this != &o) {
        if (valid(handle_)) (void)kill_and_reap();
        pid_ = o.pid_;
        handle_ = o.handle_;
        o.pid_ = -1;
        o.handle_ = kNoHandle;
    }
    return *this;
}

expected<Process, std::string> Process::spawn(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                              const std::vector<HandleMapping>& handles) {
    std::wstring cmd = quote(exe.wstring());
    for (const auto& a : args) cmd += L" " + quote(widen(a));
    std::vector<HANDLE> list;
    for (const auto& m : handles) {
        cmd += L" --handle " + widen(std::format("{}={}", m.to, static_cast<long long>(m.from)));
        list.push_back(win(m.from));
        // Inheritable, but only the handles in this child's list are inherited (no race with other spawns).
        ::SetHandleInformation(win(m.from), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    }
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    std::string error;
    PROCESS_INFORMATION pi{};
    if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        error = last_error("InitializeProcThreadAttributeList");
    } else {
        if (!list.empty() && !::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list.data(),
                                                          list.size() * sizeof(HANDLE), nullptr, nullptr)) {
            error = last_error("UpdateProcThreadAttribute");
        } else {
            STARTUPINFOEXW si{};
            si.StartupInfo.cb = sizeof si;
            si.lpAttributeList = attrs;
            if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, list.empty() ? FALSE : TRUE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
                                  &si.StartupInfo, &pi))
                error = std::format("cannot start '{}': {}", exe.string(), last_error("CreateProcess"));
        }
        ::DeleteProcThreadAttributeList(attrs);
    }
    for (const auto& m : handles) ::SetHandleInformation(win(m.from), HANDLE_FLAG_INHERIT, 0);
    if (!error.empty()) return unexpected(error);
    if (HANDLE job = worker_job()) ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    Process p;
    p.pid_ = long(pi.dwProcessId);
    p.handle_ = ours(pi.hProcess);
    return p;
}

bool Process::running() {
    if (!valid(handle_)) return false;
    if (::WaitForSingleObject(win(handle_), 0) == WAIT_TIMEOUT) return true;
    (void)reap();
    return false;
}

std::string Process::kill_and_reap() {
    if (!valid(handle_)) return "not running";
    ::TerminateProcess(win(handle_), 1);
    return reap();
}

std::string Process::reap() {
    if (!valid(handle_)) return "not running";
    ::WaitForSingleObject(win(handle_), INFINITE);
    DWORD code = 0;
    const bool got = ::GetExitCodeProcess(win(handle_), &code) != 0;
    ::CloseHandle(win(handle_));
    handle_ = kNoHandle;
    pid_ = -1;
    return got ? describe_exit(code) : "gone";
}

std::filesystem::path executable_dir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    if (n > 0 && n < buf.size()) {
        buf.resize(n);
        return std::filesystem::path(buf).parent_path();
    }
    return std::filesystem::current_path();
}

}  // namespace mimicry::ipc

#endif

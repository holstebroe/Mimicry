#include "core/Ipc.hpp"

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

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace mimicry::ipc {

bool LineChannel::write_line(std::string_view line) {
    std::string msg(line);
    msg += '\n';
    std::size_t done = 0;
    while (done < msg.size()) {
        const ssize_t n = ::write(write_, msg.data() + done, msg.size() - done);
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
        pollfd p{read_, POLLIN, 0};
        const int r = ::poll(&p, 1, timeout_ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return Read::Closed;
        }
        if (r == 0) return Read::Timeout;
        char chunk[4096];
        const ssize_t n = ::read(read_, chunk, sizeof chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            return Read::Closed;
        }
        if (n == 0) return Read::Closed;
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

void LineChannel::close() {
    if (read_ >= 0) ::close(read_);
    if (write_ >= 0 && write_ != read_) ::close(write_);
    read_ = write_ = -1;
    buffer_.clear();
}

// --- SharedMemory ----------------------------------------------------------------------------

SharedMemory::~SharedMemory() { release(); }

void SharedMemory::release() {
    if (data_) ::munmap(data_, size_);
    if (fd_ >= 0) ::close(fd_);
    data_ = nullptr;
    fd_ = -1;
    size_ = 0;
}

SharedMemory::SharedMemory(SharedMemory&& o) noexcept : fd_(o.fd_), data_(o.data_), size_(o.size_) {
    o.fd_ = -1;
    o.data_ = nullptr;
    o.size_ = 0;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& o) noexcept {
    if (this != &o) {
        release();
        fd_ = o.fd_;
        data_ = o.data_;
        size_ = o.size_;
        o.fd_ = -1;
        o.data_ = nullptr;
        o.size_ = 0;
    }
    return *this;
}

#if !defined(__linux__)
namespace {
// Serialises pipe creation and spawning where pipes cannot be made close-on-exec atomically.
std::mutex& spawn_mutex() {
    static std::mutex m;
    return m;
}
}  // namespace
#endif

expected<SharedMemory, std::string> SharedMemory::from_fd(int fd, std::size_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return unexpected(std::format("mmap failed: {}", std::strerror(errno)));
    SharedMemory m;
    m.fd_ = fd;
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
    auto m = from_fd(fd, bytes);
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


bool make_cloexec_pipe(int fds[2]) {
#if defined(__linux__)
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    std::lock_guard lock(spawn_mutex());
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

expected<Process, std::string> Process::spawn(const std::filesystem::path& exe, const std::vector<std::string>& args,
                                              const std::vector<FdMapping>& fds) {
    // Move every source above the target range first, so no mapping can overwrite another's source.
    std::vector<int> staged;
    auto cleanup = [&] {
        for (const int fd : staged) ::close(fd);
    };
    for (const auto& m : fds) {
        const int d = ::fcntl(m.from, F_DUPFD_CLOEXEC, 32);
        if (d < 0) {
            cleanup();
            return unexpected(std::format("fcntl failed: {}", std::strerror(errno)));
        }
        staged.push_back(d);
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    for (std::size_t i = 0; i < fds.size(); ++i) posix_spawn_file_actions_adddup2(&actions, staged[i], fds[i].to);

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

#else  // _WIN32: process isolation is not implemented yet

namespace mimicry::ipc {

bool LineChannel::write_line(std::string_view) { return false; }
LineChannel::Read LineChannel::read_line(std::string&, int) { return Read::Closed; }
void LineChannel::close() {}
SharedMemory::~SharedMemory() = default;
void SharedMemory::release() {}
SharedMemory::SharedMemory(SharedMemory&&) noexcept {}
SharedMemory& SharedMemory::operator=(SharedMemory&&) noexcept { return *this; }
expected<SharedMemory, std::string> SharedMemory::create(std::size_t) {
    return unexpected(std::string("process isolation is not implemented on Windows yet (plan WP1.5)"));
}
expected<SharedMemory, std::string> SharedMemory::from_fd(int, std::size_t) {
    return unexpected(std::string("process isolation is not implemented on Windows yet (plan WP1.5)"));
}
Process::~Process() = default;
Process& Process::operator=(Process&&) noexcept { return *this; }
expected<Process, std::string> Process::spawn(const std::filesystem::path&, const std::vector<std::string>&,
                                              const std::vector<FdMapping>&) {
    return unexpected(std::string("process isolation is not implemented on Windows yet (plan WP1.5)"));
}
bool Process::running() { return false; }
std::string Process::kill_and_reap() { return "not running"; }
std::string Process::reap() { return "not running"; }
std::filesystem::path executable_dir() { return std::filesystem::current_path(); }

}  // namespace mimicry::ipc

#endif

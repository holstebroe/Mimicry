#pragma once
// Process plumbing for the supervisor / worker split (docs/spec/05_RESILIENCE.md section 1): a line-framed
// channel over a pair of pipe descriptors, anonymous shared memory for audio, and process spawn / kill.
// POSIX only for now; the Windows versions return an error (plan WP1.5).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"

namespace mimicry::ipc {

// Newline-terminated messages over two descriptors. Owns neither unless close() is called.
class LineChannel {
public:
    enum class Read { Line, Timeout, Closed };

    LineChannel() = default;
    LineChannel(int read_fd, int write_fd) : read_(read_fd), write_(write_fd) {}

    [[nodiscard]] bool write_line(std::string_view line);  // false if the peer is gone
    [[nodiscard]] Read read_line(std::string& out, int timeout_ms);  // timeout_ms < 0 waits forever
    void close();

private:
    int read_ = -1, write_ = -1;
    std::string buffer_;
};

// Anonymous shared memory. The name is unlinked at creation, so a crash cannot leak it; the worker gets
// the descriptor by inheritance.
// The worker's shared memory: payload (audio, state blobs) from offset 0, then a progress area of this
// many bytes at the end. Word 0 counts the blocks started in the current render, so the supervisor can
// tell after a timeout whether the plugin ever reached its first block (spec 05 section 9).
inline constexpr std::size_t kShmProgressBytes = 64;

class SharedMemory {
public:
    SharedMemory() = default;
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    SharedMemory(SharedMemory&& o) noexcept;
    SharedMemory& operator=(SharedMemory&& o) noexcept;

    [[nodiscard]] static expected<SharedMemory, std::string> create(std::size_t bytes);
    [[nodiscard]] static expected<SharedMemory, std::string> from_fd(int fd, std::size_t bytes);

    [[nodiscard]] void* data() const { return data_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] int fd() const { return fd_; }

private:
    void release();
    int fd_ = -1;
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

struct FdMapping {
    int from;  // descriptor in this process
    int to;    // descriptor number in the child
};

// A pipe whose two ends are close-on-exec from the start, so a worker spawned concurrently on another thread
// cannot inherit them (an inherited write end would hide a dead worker's EOF until the watchdog fires).
// Linux: pipe2(O_CLOEXEC); elsewhere pipe + FD_CLOEXEC under the lock Process::spawn also takes.
[[nodiscard]] bool make_cloexec_pipe(int fds[2]);

class Process {
public:
    Process() = default;
    ~Process();  // kills and reaps a child that is still running
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& o) noexcept : pid_(o.pid_) { o.pid_ = -1; }
    Process& operator=(Process&& o) noexcept;

    [[nodiscard]] static expected<Process, std::string> spawn(const std::filesystem::path& exe,
                                                              const std::vector<std::string>& args,
                                                              const std::vector<FdMapping>& fds);

    [[nodiscard]] bool running();                // non-blocking
    [[nodiscard]] std::string kill_and_reap();   // SIGKILL, wait; returns how it ended
    [[nodiscard]] std::string reap();            // wait for a child that closed its pipe; returns how it ended
    [[nodiscard]] long id() const { return pid_; }

private:
    long pid_ = -1;
};

// Directory of the running executable, to find mimicry_worker next to mimicry.
[[nodiscard]] std::filesystem::path executable_dir();

}  // namespace mimicry::ipc

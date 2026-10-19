#pragma once
// Process plumbing for the supervisor / worker split (docs/spec/05_RESILIENCE.md section 1): a line-framed
// channel over a pair of pipe ends, anonymous shared memory for audio, and process spawn / kill.
// POSIX: pipes, shm_open (unlinked at once), posix_spawn, descriptors 3/4/5 in the child.
// Windows: overlapped named pipes, an unnamed file mapping, CreateProcess with an explicit handle list (the
// child inherits exactly its own handles), workers in a kill-on-close job object.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/Expected.hpp"

namespace mimicry::ipc {

// A descriptor (POSIX) or a HANDLE (Windows).
using Handle = std::intptr_t;
inline constexpr Handle kNoHandle = -1;

void close_handle(Handle h);

// Newline-terminated messages over two pipe ends. Owns neither unless close() is called.
class LineChannel {
public:
    enum class Read { Line, Timeout, Closed };

    LineChannel() = default;
    LineChannel(Handle read_end, Handle write_end) : read_(read_end), write_(write_end) {}

    [[nodiscard]] bool write_line(std::string_view line);  // false if the peer is gone
    [[nodiscard]] Read read_line(std::string& out, int timeout_ms);  // timeout_ms < 0 waits forever
    void close();

private:
    Handle read_ = kNoHandle, write_ = kNoHandle;
    std::string buffer_;
};

// Anonymous shared memory: nothing named outlives the processes, so a crash cannot leak it; the worker gets
// the handle by inheritance.
//
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
    SharedMemory(SharedMemory&& o) noexcept : handle_(o.handle_), data_(o.data_), size_(o.size_) {
        o.handle_ = kNoHandle;
        o.data_ = nullptr;
        o.size_ = 0;
    }
    SharedMemory& operator=(SharedMemory&& o) noexcept {
        if (this != &o) {
            release();
            handle_ = o.handle_;
            data_ = o.data_;
            size_ = o.size_;
            o.handle_ = kNoHandle;
            o.data_ = nullptr;
            o.size_ = 0;
        }
        return *this;
    }

    [[nodiscard]] static expected<SharedMemory, std::string> create(std::size_t bytes);
    [[nodiscard]] static expected<SharedMemory, std::string> from_handle(Handle h, std::size_t bytes);

    [[nodiscard]] void* data() const { return data_; }
    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] Handle handle() const { return handle_; }

private:
    void release();
    Handle handle_ = kNoHandle;
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

struct HandleMapping {
    Handle from;  // in this process
    int to;       // the number the child knows it by: its descriptor on POSIX, `--handle to=value` on Windows
};

// A pipe (read end, write end) that no other child can inherit: a worker spawned concurrently on another
// thread must not hold a copy, or a dead worker's EOF would hide until the watchdog fires.
// Linux: pipe2(O_CLOEXEC); macOS: pipe + FD_CLOEXEC under the lock Process::spawn also takes; Windows: an
// overlapped named pipe, inherited only through an explicit handle list.
[[nodiscard]] bool make_pipe(Handle& read_end, Handle& write_end);

// The handles a worker was given (HandleMapping::to -> handle): POSIX uses the numbers themselves, Windows
// reads `--handle to=value` from the command line.
[[nodiscard]] std::map<int, Handle> inherited_handles(int argc, char** argv);

class Process {
public:
    Process() = default;
    ~Process();  // kills and reaps a child that is still running
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&& o) noexcept : pid_(o.pid_), handle_(o.handle_) {
        o.pid_ = -1;
        o.handle_ = kNoHandle;
    }
    Process& operator=(Process&& o) noexcept;

    [[nodiscard]] static expected<Process, std::string> spawn(const std::filesystem::path& exe,
                                                              const std::vector<std::string>& args,
                                                              const std::vector<HandleMapping>& handles);

    [[nodiscard]] bool running();                // non-blocking
    [[nodiscard]] std::string kill_and_reap();   // SIGKILL / TerminateProcess, wait; returns how it ended
    [[nodiscard]] std::string reap();            // wait for a child that closed its pipe; returns how it ended
    [[nodiscard]] long id() const { return pid_; }

private:
    long pid_ = -1;
    Handle handle_ = kNoHandle;  // the process handle (Windows)
};

// Directory of the running executable, to find mimicry_worker next to mimicry.
[[nodiscard]] std::filesystem::path executable_dir();

}  // namespace mimicry::ipc

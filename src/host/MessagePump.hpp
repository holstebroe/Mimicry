#pragma once
// Minimal main-thread message pump for the worker (docs/spec/02_HOSTING.md section 3, WP1.9).
//
// Many plugins (JUCE-based ones especially) expect timers and async updates to run on the main thread.
// The worker calls pump() between jobs and while it waits for the next command. No window is created.
// The portable part is a callback queue (CLAP request_callback and the VST3 run loop post into it); the
// platform part drains the operating system's queue: Win32 messages, the macOS run loop. On Linux
// there is no system queue to drain.

#include <functional>
#include <mutex>
#include <vector>

namespace mimicry::host {

class MessagePump {
public:
    // Thread-safe: any thread may post, the main thread runs the callbacks.
    void post(std::function<void()> fn);
    // Main thread: runs queued callbacks, then drains the system queue. Returns true if anything ran.
    bool pump();

private:
    std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
};

}  // namespace mimicry::host

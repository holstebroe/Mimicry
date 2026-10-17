#include "host/MessagePump.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace mimicry::host {

void MessagePump::post(std::function<void()> fn) {
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(fn));
}

bool MessagePump::pump() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard lock(mutex_);
        work.swap(queue_);
    }
    bool ran = !work.empty();
    for (auto& fn : work) {
        try {
            fn();
        } catch (...) {
            // A plugin callback that throws must not take the worker down; the render path reports errors.
        }
    }
#if defined(_WIN32)
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        ran = true;
    }
#elif defined(__APPLE__)
    // Run the main run loop once without blocking.
    ran = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true) == kCFRunLoopRunHandledSource || ran;
#endif
    return ran;
}

}  // namespace mimicry::host

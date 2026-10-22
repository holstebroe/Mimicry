#include "app/Console.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace mimicry::app {

namespace {

std::atomic<int> g_signals{0};
std::atomic<bool>* g_cancel = nullptr;

extern "C" void on_signal(int) {
    if (g_signals.fetch_add(1) >= 1) std::_Exit(130);  // the second time: out now
    if (g_cancel) g_cancel->store(true);
}

}  // namespace

Console& Console::get() {
    static Console c;
    return c;
}

void Console::info(const std::string& text) {
    std::lock_guard lock(out_mutex_);
    if (!events_) {
        std::fputs(text.c_str(), stdout);
        if (text.empty() || text.back() != '\n') std::fputc('\n', stdout);
        std::fflush(stdout);
        return;
    }
    std::string t = text;
    while (!t.empty() && t.back() == '\n') t.pop_back();
    json::Value e;
    e.set("event", "log").set("level", "info").set("text", t);
    std::fputs((e.dump() + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void Console::warn(const std::string& text) {
    std::lock_guard lock(out_mutex_);
    if (!events_) {
        std::fputs(("note: " + text + (text.empty() || text.back() != '\n' ? "\n" : "")).c_str(), stderr);
        return;
    }
    std::string t = text;
    while (!t.empty() && t.back() == '\n') t.pop_back();
    json::Value e;
    e.set("event", "log").set("level", "warn").set("text", t);
    std::fputs((e.dump() + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void Console::emit(json::Value event) {
    if (!events_) return;
    std::lock_guard lock(out_mutex_);
    std::fputs((event.dump() + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void Console::done(const std::string& status, json::Value details) {
    if (!events_) return;
    details.set("event", "done").set("status", status);
    emit(std::move(details));
}

void Console::install_signal_handlers() {
    g_cancel = &cancel_;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGBREAK
    std::signal(SIGBREAK, on_signal);
#endif
}

// Raw reads, not std::cin: a thread blocked inside the C library's stdin lock would stop the process from
// exiting while the front end still holds the pipe open.
void Console::start_stdin_watcher(bool cancel_on_eof) {
    std::thread([this, cancel_on_eof] {
        std::string pending;
        char buf[256];
        for (;;) {
#if defined(_WIN32)
            DWORD got = 0;
            if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof buf, &got, nullptr) || got == 0) break;
            const long n = long(got);
#else
            const long n = long(::read(0, buf, sizeof buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
#endif
            pending.append(buf, std::size_t(n));
            std::size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line == "cancel") {
                    cancel_ = true;
                    return;
                }
            }
        }
        if (cancel_on_eof) cancel_ = true;
    }).detach();
}

void Console::watch_cancel_file(std::filesystem::path path) {
    std::lock_guard lock(file_mutex_);
    cancel_files_.push_back(std::move(path));
}

bool Console::cancel_requested() {
    if (cancel_) return true;
    std::lock_guard lock(file_mutex_);
    if (cancel_files_.empty()) return false;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - last_file_check_ms_ < 250) return false;
    last_file_check_ms_ = now;
    std::error_code ec;
    for (const auto& f : cancel_files_)
        if (std::filesystem::exists(f, ec)) cancel_ = true;
    return cancel_;
}

}  // namespace mimicry::app

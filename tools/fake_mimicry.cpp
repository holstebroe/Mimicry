// A stand-in for `mimicry --events` that plays a scripted run, so the front end (src/gui/win) can be tried and
// screenshot-tested without plugins, on Windows or under Wine:
//   x86_64-w64-mingw32-g++ -std=c++20 -static tools/fake_mimicry.cpp -o mimicry.exe   (or any C++20 compiler)
// Put it next to mimicry_gui.exe in place of the real mimicry.exe. fit, transfer and harvest run for about
// 20 s and answer a "cancel" line on stdin with a cancelled done event; runs lists two runs.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::atomic<bool> g_cancel{false};

void emit(const std::string& json) {
    std::printf("%s\n", json.c_str());
    std::fflush(stdout);
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

bool has(const std::vector<std::string>& a, const std::string& k) {
    for (const auto& s : a)
        if (s == k) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return 1;
    const std::string cmd = args[0];
    if (cmd == "runs") {
        emit(R"([{"kind":"fit","status":"cancelled","plugin":"C:\\Plugins\\Synth.vst3","target":"C:\\Audio\\lead.wav","loss_db":2.41,"evals":5120,"elapsed_s":412,"dir":"runs\\20261021-101500-Synth-lead","resumable":true},)"
             R"({"kind":"transfer","status":"finished","plugin":"C:\\Plugins\\Other.clap","target":"C:\\Plugins\\Other.clap","loss_db":1.12,"evals":22000,"elapsed_s":3100,"dir":"runs\\20261020-220000-transfer","resumable":false}])");
        return 0;
    }
    std::thread([] {
        std::string line;
        while (std::getline(std::cin, line))
            if (line.rfind("cancel", 0) == 0) g_cancel = true;
    }).detach();
    const bool resumed = has(args, "--resume");
    const std::string dir = "runs\\20261021-120000-demo";
    emit(R"({"event":"start","command":")" + cmd + R"(","run_dir":")" + (cmd == "harvest" ? "" : "runs\\\\20261021-120000-demo") + R"(","plugin":"demo.clap","target":"demo.wav","resumed":)" + (resumed ? "true" : "false") + "}");
    emit(R"({"event":"phase","name":"opening the plugin"})");
    sleep_ms(500);
    if (resumed) emit(R"J({"event":"log","level":"info","text":"resuming at 4 s and 166 evaluations, best 2.729 dB (1 session(s) before)"})J");
    const double cap = 300;
    if (cmd == "harvest") {
        emit(R"({"event":"phase","name":"harvesting"})");
        const char* names[] = {"Init", "Soft Pad", "Lead", "Pluck", "Lead Copy", "Vibrato"};
        int ok = 0;
        for (int i = 0; i < 6 && !g_cancel; ++i) {
            sleep_ms(900);
            const bool good = i != 0 && i != 4;
            ok += good;
            emit(R"({"event":"preset","method":"clap-preset","index":)" + std::to_string(i) + R"(,"position":)" + std::to_string(i + 1) + R"(,"of":6,"name":")" + names[i] +
                 R"(","outcome":")" + (good ? "ok" : i == 0 ? "no-change" : "duplicate") + R"(","number":)" + std::to_string(good ? ok : 0) + R"(,"message":")" +
                 (good ? "" : "the state equals the defaults") + R"(","ok":)" + std::to_string(ok) + "}");
        }
        emit(std::string(R"({"event":"done","status":")") + (g_cancel ? "cancelled" : "finished") + R"(","library":"presets\\demo","presets":)" + std::to_string(ok) + R"(,"resumable":)" + (g_cancel ? "true" : "false") + "}");
        return g_cancel ? 4 : 0;
    }
    const int presets = cmd == "transfer" ? 3 : 0;
    emit(R"({"event":"phase","name":"fitting","active":17,"categorical":1,"time_cap_s":)" + std::to_string(int(cap)) + R"(,"workers":3})");
    if (presets) emit(R"({"event":"bank","presets":3,"done":0})");
    double seconds = resumed ? 4 : 0;
    long evals = resumed ? 166 : 0;
    for (int p = 1; p <= std::max(presets, 1) && !g_cancel; ++p) {
        const std::string pj = presets ? R"(,"preset":)" + std::to_string(p) + R"(,"presets":3,"preset_name":"Preset )" + std::to_string(p) + "\"" : "";
        if (presets) emit(R"({"event":"preset_start","preset":)" + std::to_string(p) + R"(,"presets":3,"name":"Preset )" + std::to_string(p) + R"(","resumed":false})");
        const char* stages[] = {"S1 seed", "S2 osc+amp", "S5 run 1", "S5 run 2", "S7 refine"};
        for (int step = 0; step < (presets ? 20 : 60) && !g_cancel; ++step) {
            const int si = step * 5 / (presets ? 20 : 60);
            if (step % (presets ? 4 : 12) == 0)
                emit(R"({"event":"stage","name":")" + std::string(stages[si]) + R"(","free":18,"finished":false,"before_db":8,"after_db":0,"evals":0,"seconds":)" + std::to_string(seconds) + pj + "}");
            sleep_ms(300);
            seconds += presets ? 6 : 4;
            evals += 90;
            const double loss = 0.6 + 12.0 * std::exp(-seconds / (presets ? 20.0 : 60.0));
            const double frac = std::min(0.99, seconds / (presets ? 120.0 : cap));
            emit(R"({"event":"progress","stage":")" + std::string(stages[si]) + R"(","evals":)" + std::to_string(evals) + R"(,"best_db":)" + std::to_string(loss) + R"(,"seconds":)" +
                 std::to_string(seconds) + R"(,"time_cap_s":)" + std::to_string(presets ? 120 : int(cap)) + R"(,"fraction":)" + std::to_string(frac) + R"(,"evals_per_s":88.5)" + pj + "}");
            if (step % (presets ? 4 : 12) == (presets ? 3 : 11))
                emit(R"({"event":"stage","name":")" + std::string(stages[si]) + R"(","free":18,"finished":true,"before_db":8,"after_db":)" + std::to_string(loss) + R"(,"evals":900,"seconds":24)" + pj + "}");
        }
        if (presets && !g_cancel)
            emit(R"({"event":"preset_done","preset":)" + std::to_string(p) + R"(,"presets":3,"name":"Preset )" + std::to_string(p) + R"(","status":"done","loss_db":1.25,"evals":1800,"seconds":120,"note":""})");
        if (presets) emit(R"({"event":"bank","presets":3,"done":)" + std::to_string(g_cancel ? p - 1 : p) + "}");
    }
    emit(R"({"event":"phase","name":"writing results"})");
    sleep_ms(300);
    const std::string files = R"("files":{"ab":"runs\\20261021-120000-demo\\ab.wav","report":"runs\\20261021-120000-demo\\report.md","spectrogram":"runs\\20261021-120000-demo\\diff.bmp"})";
    emit(std::string(R"({"event":"done","status":")") + (g_cancel ? "cancelled" : "finished") + R"(","run_dir":"runs\\20261021-120000-demo","resumable":)" + (g_cancel ? "true" : "false") +
         R"(,"loss_db":1.84,"evals":)" + std::to_string(evals) + R"(,"elapsed_s":)" + std::to_string(int(seconds)) + R"(,"presets_done":3,)" + files + "}");
    return g_cancel ? 4 : 0;
}

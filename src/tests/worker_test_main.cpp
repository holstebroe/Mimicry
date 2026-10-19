// Isolation tests (T1 through the worker, T7, T9; docs/spec/07_OUTPUT_VALIDATION.md section 4), and the
// recovery and format handling of spec 05 sections 5 and 9: canary restart after sticky poison, plugin
// exceptions, the "never reached the first block" diagnosis, and a parameter table that changes mid-run.
// Usage: mimicry_worker_test <plugin> <mimicry_worker>.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>

#include "host/PluginHost.hpp"
#include "supervisor/WorkerClient.hpp"
#include "supervisor/WorkerPool.hpp"
#include "tests/test_synth/TestSynthCore.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <cerrno>
#endif

namespace {

using namespace mimicry;
using namespace mimicry::supervisor;
namespace ts = mimicry::testsynth;

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

host::Job job_for(const host::Instance& inst) {
    host::Job job;
    for (const auto& p : inst.parameters()) job.coords.push_back(p.default_coord);
    job.coords[ts::kLfoSeed] = 1.0 / 255.0;
    job.notes.push_back({60, 0, 0.8f, 0, 300});
    job.tail_max_ms = 2000;
    return job;
}

void make_crash(host::Job& job) {
    job.coords[ts::kHazA] = 0.9;
    job.coords[ts::kHazB] = 0.95;
}

int open_fds() {
    int n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/fd", ec)) { (void)e; ++n; }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fputs("usage: mimicry_worker_test <plugin> <mimicry_worker>\n", stderr);
        return 2;
    }
    const std::string plugin = argv[1];
    WorkerOptions opt;
    opt.worker_exe = argv[2];
    opt.min_timeout_s = 1.0;
    opt.timeout_factor = 0;  // the tests use the floor; the production factor is 20x audio duration
    opt.shm_bytes = 16u << 20;

#if defined(_WIN32)
    std::puts("mimicry_worker_test: skipped (process isolation not implemented on Windows yet)");
    return 0;
#else
    // The same job in-process and through a worker gives identical audio.
    auto direct = host::open(plugin);
    auto client = WorkerClient::open(plugin, {}, opt);
    check(direct.has_value(), "in-process open");
    check(client.has_value(), "worker open: " + (client ? std::string() : client.error().message));
    if (!direct || !client) return 1;
    WorkerClient& w = **client;
    check(w.info().uid == (*direct)->info().uid && w.info().version_hash == (*direct)->info().version_hash, "info crosses the process boundary");
    check(w.parameters().size() == (*direct)->parameters().size() && w.parameters()[ts::kCutoff].log_scale == (w.info().format == host::PluginFormat::Clap) &&
              w.parameters()[ts::kCutoff].units == "Hz" && w.parameters()[ts::kProgram].flags.program_change,
          "parameters cross the process boundary");
    check(w.value_text(ts::kCutoff, 1.0) == (*direct)->value_text(ts::kCutoff, 1.0), "value_text through the worker");

    const auto job = job_for(*direct.value());
    auto a = (*direct)->render(job);
    auto b = w.render(job);
    check(a && b && a->samples == b->samples && a->sample_rate == b->sample_rate, "worker audio equals in-process audio");

    // Failure outcomes cross the boundary; a crash costs one candidate.
    auto nan = job;
    nan.coords[ts::kHazNan] = 1.0;
    auto n = w.render(nan);
    check(!n && n.error() == Outcome::NonFinite, "NonFinite through the worker");
    auto crash = job;
    make_crash(crash);
    auto c = w.render(crash);
    check(!c && c.error() == Outcome::Crash, "planted null dereference is a Crash");
    check(w.stats().crashes == 1, "crash counted");
    auto after = w.render(job);
    check(after && a && after->samples == a->samples, "next render after a crash is correct (worker respawned)");

    auto hang = job;
    hang.coords[ts::kHazHang] = 1.0;
    const auto t0 = std::chrono::steady_clock::now();
    auto h = w.render(hang);
    const double hang_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(!h && h.error() == Outcome::Timeout, "planted hang is a Timeout");
    check(w.last_message().find("in block 1") != std::string::npos, "the timeout says where it hung: " + w.last_message());
    check(hang_s < 5.0, "watchdog fires within bounds: " + std::to_string(hang_s));
    check(w.render(job).has_value(), "next render after a timeout works");

    // State goes through the shared memory.
    auto blob = w.save_state();
    check(blob.has_value() && !blob->empty(), "save_state through the worker");
    if (blob) check(w.load_state(*blob).has_value(), "load_state through the worker");
    check(!w.load_state({9, 9, 9}).has_value(), "bad state is an error, not a crash");

    // Poisoned state does not survive into the next job.
    auto poison = job;
    poison.coords[ts::kHazPoison] = 1.0;
    check(!w.render(poison).has_value(), "poison hazard fails");
    auto clean = w.render(job);
    check(clean && a && clean->samples == a->samples, "render after poison is clean");

    // Spec 05 section 5: poison that survives reset and state load. The canary (the last job that passed)
    // fails too, so the worker restarts and the next render is clean.
    {
        const auto before = w.stats();
        auto sticky = job;
        sticky.coords[ts::kHazSticky] = 1.0;
        auto r = w.render(sticky);
        check(!r && r.error() == Outcome::NonFinite, "sticky poison fails");
        check(w.stats().canaries == before.canaries + 1 && w.stats().canary_restarts == before.canary_restarts + 1,
              "the canary fails after sticky poison and the worker restarts");
        auto next = w.render(job);
        check(next && a && next->samples == a->samples, "render after sticky poison is clean");
        // Ordinary poison is cleared by the reset: the canary passes, no restart.
        const auto mid = w.stats();
        check(!w.render(poison).has_value() && w.stats().canaries == mid.canaries + 1 &&
                  w.stats().canary_restarts == mid.canary_restarts && w.stats().spawned == mid.spawned,
              "the canary passes after poison a reset clears");
    }

    // Spec 05 section 9: a plugin exception is a HostError, and the worker stays up.
    {
        const auto before = w.stats();
        auto thrower = job;
        thrower.coords[ts::kHazThrow] = 1.0;
        auto r = w.render(thrower);
        check(!r && r.error() == Outcome::HostError && w.last_message().find("threw") != std::string::npos,
              "a plugin exception is a HostError: " + w.last_message());
        check(w.stats().spawned == before.spawned && w.stats().crashes == before.crashes, "an exception costs no process");
        auto next = w.render(job);
        check(next && a && next->samples == a->samples, "render after an exception is clean");
    }

    // Spec 05 section 9: a restart request that renames a parameter stops the run with a clear error.
    {
        auto r = WorkerClient::open(plugin, {}, opt);
        check(r.has_value(), "open for the table change");
        if (r) {
            auto& t = **r;
            check(t.render(job).has_value() && t.table_change().empty(), "no table change before the rename");
            auto rename = job;
            rename.coords[ts::kHazRename] = 1.0;
            auto x = t.render(rename);
            check(!x && x.error() == Outcome::HostError && !t.table_change().empty(), "the rename is a table change: " + t.last_message());
            check(t.table_change().find("Hidden Drive") != std::string::npos, "the change names the parameter: " + t.table_change());
            auto y = t.render(job);
            check(!y && y.error() == Outcome::HostError, "every render after a table change fails");
            check(t.parameters()[ts::kHidden].name == "Hidden Drive", "callers keep the table they indexed into");
        }
    }

    // T7: many crashes, flat descriptor count, no zombies.
    const int fds_before = open_fds();
    const int kCrashes = 300;
    int crashes = 0;
    for (int i = 0; i < kCrashes; ++i) {
        auto r = w.render(i % 2 ? crash : job);
        if (i % 2) crashes += (!r && r.error() == Outcome::Crash);
        else if (!r) check(false, "valid job failed in the crash loop");
    }
    check(crashes == kCrashes / 2, "every planted crash was reported: " + std::to_string(crashes));
    const int fds_after = open_fds();
    check(fds_after <= fds_before + 1, "descriptor count stays flat: " + std::to_string(fds_before) + " -> " + std::to_string(fds_after));
    for (int i = 0; i < 4; ++i) check(!w.render(hang).has_value(), "repeated hang");
    check(w.stats().timeouts == 5, "timeouts counted: " + std::to_string(w.stats().timeouts));

    // Recycling: a fresh process every N renders.
    {
        WorkerOptions o2 = opt;
        o2.recycle_after = 3;
        auto r = WorkerClient::open(plugin, {}, o2);
        check(r.has_value(), "open with recycling");
        if (r) {
            for (int i = 0; i < 10; ++i) check((*r)->render(job).has_value(), "render with recycling");
            check((*r)->stats().recycled >= 2, "worker recycled: " + std::to_string((*r)->stats().recycled));
        }
    }

    // Pool: parallel rendering keeps job order and survives crashes.
    {
        auto pool = WorkerPool::open(plugin, {}, 4, opt);
        check(pool.has_value(), "pool opens");
        if (pool) {
            std::vector<host::Job> jobs;
            for (int i = 0; i < 24; ++i) {
                auto j = job;
                j.coords[ts::kCutoff] = 0.2 + 0.03 * i;
                if (i % 6 == 5) make_crash(j);
                jobs.push_back(j);
            }
            const auto results = (*pool)->render_all(jobs);
            bool order_ok = true;
            for (std::size_t i = 0; i < results.size(); ++i) {
                const bool crashed = i % 6 == 5;
                if (crashed != (!results[i] && results[i].error() == Outcome::Crash)) order_ok = false;
                if (!crashed) {
                    auto ref = (*direct)->render(jobs[i]);
                    if (!results[i] || !ref || results[i]->samples != ref->samples) order_ok = false;
                }
            }
            if (!order_ok) {
                std::string got;
                for (std::size_t i = 0; i < results.size(); ++i)
                    got += std::format(" {}:{}", i, results[i] ? "Ok" : std::string(to_string(results[i].error())));
                std::fprintf(stderr, "pool outcomes:%s\n", got.c_str());
            }
            check(order_ok, "pool results are in job order and equal in-process renders");
            check((*pool)->total_stats().crashes == 4, "pool crash count");
            // Workers respawned after the crashes keep running into the next batch (Linux: the parent-death
            // signal belongs to the spawning thread, so respawns must happen on the pool's long-lived threads).
            std::vector<host::Job> valid(12, job);
            const auto again = (*pool)->render_all(valid);
            bool all_ok = true;
            for (const auto& r : again) all_ok = all_ok && r.has_value();
            check(all_ok && (*pool)->total_stats().crashes == 4, "respawned pool workers survive the batch that respawned them");
            // Concurrent respawns: a worker spawned on one pool thread must not inherit another worker's pipe,
            // or that worker's crash reads as a timeout (no EOF while the copy is open).
            const auto before = (*pool)->total_stats();
            std::vector<host::Job> storm;
            for (int i = 0; i < 64; ++i) {
                auto j = job;
                if (i % 2 == 0) make_crash(j);
                storm.push_back(j);
            }
            long crashed = 0;
            for (int round = 0; round < 4; ++round)
                for (const auto& r : (*pool)->render_all(storm)) crashed += !r && r.error() == Outcome::Crash;
            const auto after_storm = (*pool)->total_stats();
            check(crashed == 128 && after_storm.timeouts == before.timeouts,
                  std::format("crash storm: {} of 128 crashes, {} timeouts", crashed, after_storm.timeouts - before.timeouts));
        }
    }

    client->reset();
    // No zombies: every child has been reaped.
    int status = 0;
    check(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "no zombie or unreaped children");

    if (failures == 0) std::puts("mimicry_worker_test: all checks passed");
    return failures == 0 ? 0 : 1;
#endif
}

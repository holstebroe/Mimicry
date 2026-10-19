# 05 — Resilience to illegal parameter combinations

Some plugins throw, return errors, emit NaN, blow up a filter, hang, or crash outright for parameter
combinations the designer never expected. An optimizer finds those corners on purpose, because they
often score well before failing. The search must treat failure as ordinary information. Everything
here applies to CLAP and VST3 plugins alike; §9 lists the format-specific parts.

## 1. Isolation

All rendering happens in `mimicry_worker` processes. The supervisor:
- sends jobs over a pipe and receives audio through shared memory (no audio on the pipe). Implemented as one JSON object per line on inherited descriptors 3 (to the worker) and 4 (from it), and anonymous shared memory on descriptor 5 (unlinked at creation, so nothing leaks) that also carries state blobs [A];
- applies a **per-job wall-clock timeout**, default `max(5 s, 20 × audio duration)` [D];
- on timeout or process exit, kills the worker (SIGKILL / `TerminateProcess`), reaps it, releases the
  shared memory, respawns, and scores the candidate as `Timeout` or `Crash`;
- keeps handle, memory and zombie counts flat across thousands of crashes (test T7).
- spawns and respawns each pool worker from one long-lived thread per worker: on Linux the worker's
  parent-death signal (`PR_SET_PDEATHSIG`) belongs to the spawning *thread*, so a worker respawned from a
  short-lived thread would be killed when that thread ends and the next candidate scored as `Crash` [A].

`--no-isolation` renders in-process for debugging a plugin under a debugger; `mimicry replay` uses it.

## 2. Outcomes

| Outcome | Cause | Detection |
| --- | --- | --- |
| `Ok` | valid audio | all checks pass |
| `Silent` | plugin muted by the combination | peak below -90 dBFS (when the target is not silent) |
| `NonFinite` | NaN or Inf in the output | per-block scan |
| `Overload` | runaway level | peak above limit, default 50 [S: Acidus rejects renders above 50] |
| `DcRunaway` | unstable filter/feedback | \|mean\| above 0.5 of peak over the last second |
| `HostError` | `process` returned an error status, `activate`/setup failed, or the plugin threw an exception across a call | return codes, caught exception |
| `Timeout` | hang or pathological CPU use | watchdog |
| `Crash` | process died | exit status |

## 3. Search under failure

- **Ranking.** CMA-ES is rank-based. Invalid candidates are ranked after every valid one, ordered by
  outcome severity (Silent, then Overload/DcRunaway/NonFinite, then Timeout/HostError, then Crash)
  and, within a class, by distance to the best valid point, so the strategy still gets a direction away
  from the hazard [D]. Valid candidates keep their true loss.
- **Resample first.** `ask()` rejects a candidate that falls inside a known constraint (§4) and
  redraws, up to 5 times, before accepting it as a probable failure [D].
- **Adapt.** If more than half of a generation is invalid, shrink sigma by 0.7 and recentre on the best
  valid point [D].
- **Safe box.** The probe renders every parameter at its extremes and midpoint with the others at
  default. A range that fails is shortened to the last good value; the search runs inside this box.
- **Never rely on in-plugin guards.** A plugin that clamps internally can still stay in a bad state;
  check outputs, not intentions.

## 4. Learning constraints: failure bisection

When a candidate `x_f` fails and a known good `x_g` exists (best valid, else defaults):

1. **Delta-debug the parameter set.** Start with all differing parameters; partition, test halves
   (cached renders), keep the failing half, repeat until the failing set `S` cannot be reduced
   (ddmin, O(n log n) renders worst case, usually a handful).
2. **Find thresholds.** For each parameter in `S` (usually 1-3), bisect along `x_g → x_f` for the
   boundary value, then record the constraint:
   `{ params: [id…], region: box or half-space, outcome, confidence, count }`.
3. **Store** it in the plugin profile. Later runs and later candidates in this run reject points inside
   the region with a cheap geometric test; confidence grows with repeats, and a constraint contradicted
   by a valid render is widened or dropped.

Example the test plugin plants (T8): crash when `A > 0.8 && B > 0.9`; the bisection must find
`{A, B}` and thresholds within 2 %.

## 5. State poisoning

A filter that blew up can keep NaN state, and the next render of a valid preset then fails.

- After **any** non-`Ok` outcome, the worker resets the instance (`Instance::reset`: the format's
  stop/deactivate/activate/start sequence, 02 §2), then `load_state` of the clean snapshot taken at
  start. If a check still fails on a known-good preset (the *canary*, rendered after every failure), the
  worker restarts.
- Every N renders (default 500 [D]) the instance is recycled to bound leaks and drift.
- Every candidate starts from the full parameter vector, never a delta (02 §5), so earlier candidates
  cannot leak into later ones through parameters the plugin did not reset.

## 6. Smoothing and settling

Parameters are applied at time 0 of block 0, then `settle_ms` of silence is rendered before the notes
so smoothers converge (Acidus sends its CCs 1.2 s before the note for the same reason [S]). A probe
measures the plugin's actual settle time by stepping one parameter and watching convergence; the
profile stores it.

## 7. Failure reports

Each distinct failure writes `crashes/<plugin>-<hash>.json` with the format, plugin uid and version
hash, the full parameter vector (ids and values), note plan, sample rate, block size, outcome, and the
minimal set `S` once known. `mimicry replay` reproduces it, and the file is what you send to a plugin
vendor. The run report lists failure counts per outcome and the constraints learned.

## 8. Never destructive

Mimicry only calls the plugin's own API and writes inside its output directory. It never edits plugin
files, user presets, or the plugin's saved state on disk.

## 9. Format-specific notes

- **VST3** plugins loaded in-process by hosts are the norm, so third-party VST3 plugins are more likely
  than CLAP ones to expect a message loop, a window, or a licence check at start-up. The worker pumps
  a minimal main-thread loop (02 §3) and creates no windows; a plugin that blocks on a dialog is a
  `Timeout`, and its failure report says it never reached the first block.
- **VST3 restart requests** (`restartComponent`: reload component, parameter titles changed, latency
  changed) are not failures: the worker re-enumerates and re-runs setup, and the supervisor invalidates
  the parameter table for that instance. If the table changes between candidates, the run stops with a
  clear error rather than mapping stale ids.
- **Program-change parameters** (VST3 `kIsProgramChange`, CLAP equivalents): pinned, never searched,
  because a program change would silently overwrite every other parameter mid-job.
- **Offline mode**: VST3 is set up in offline processing mode (02 §5); a plugin that behaves differently
  there is flagged by the repeatability probe, which renders in the same mode the fit will use.
- **C++ exceptions** thrown by a plugin across an interface call are caught per call in the worker and
  reported as `HostError`; structured faults (SIGSEGV, SEH) still need the process boundary.

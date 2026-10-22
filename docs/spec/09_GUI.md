# 09 — Command-line control and the Windows front end

Mimicry is a command-line tool first. Everything a person can start in the window an agent can start in a
shell, with the same options, the same files and the same results; the window adds forms, a live monitor and
buttons on top of that, and nothing else. Evidence tags as elsewhere: [S] sourced, [I] inferred, [D] default,
[A] added.

## 1. Rules

1. **One implementation.** The front end runs `mimicry` as a child process and shows what it reports. It loads
   no plugin and includes no CLAP or VST3 header (CLAUDE.md rules 3 and 4); plugins stay in
   `mimicry_worker`. A feature exists when the command line has it; the window then gets a form for it (§5).
2. **Windows only** for the window [A: owner]. The Win32 API and the common controls, no framework, no new
   dependency (CLAUDE.md rule 2). Linux and macOS use the command line; the logic under the window
   (`gui/ChildProcess`, `gui/Forms`, `gui/RunModel`) is portable and tested on all three platforms.
3. **A run can always be stopped and continued** (§3). Stopping keeps everything found so far.

## 2. Machine-readable output (`--events`)

`--events` turns stdout into **one JSON object per line**; nothing else is written there. Text meant for a
person becomes `log` events, and a `done` event always ends the output, also after an error. Plugin chatter on
stderr (workers redirect their stdout to it) is not part of the protocol. Every event has an `"event"` member.

| event | when | members |
| --- | --- | --- |
| `start` | first, once the run directory exists | `command`, `run_dir` (fit, transfer), `plugin`, `target` or `source`, `resumed` |
| `phase` | the run moves on | `name` ("analysing the target", "opening the plugin", "fitting", "harvesting", "writing results", ...); `fitting` also `active`, `categorical`, `time_cap_s`, `workers` |
| `progress` | every `--progress-s` (1 s with `--events`) at a batch boundary | `stage`, `evals`, `best_db`, `seconds` (run time, including earlier sessions), `time_cap_s`, `fraction` (0..1, the larger of time and evaluation budget, below 1 until done), `evals_per_s`; in a transfer also `preset`, `presets`, `preset_name` |
| `stage` | a pipeline stage begins or ends | `name`, `free`, `finished`, `before_db`, `after_db`, `evals`, `seconds` (+ preset members) |
| `preset` | harvest: one preset tried | `method`, `index`, `position` of `of` in the method, `name`, `outcome`, `number`, `message`, `ok` so far |
| `bank` | transfer: counts changed | `presets`, `done` |
| `preset_start`, `preset_done` | transfer: a source preset starts / ends | `preset`, `presets`, `name`; done also `status`, `loss_db`, `evals`, `seconds`, `note` |
| `log` | text for a person | `level` (`info` or `warn`), `text` |
| `done` | last | `status` (`finished`, `cancelled`, `failed`), `run_dir`, `resumable`, `loss_db`, `evals`, `elapsed_s`, `files` (`ab`, `match`, `report`, `spectrogram`), `error` when failed; harvest also `library`, `presets`; transfer `presets_done`, `matched`, `unmatched` |

Keys are stable; new members may be added, so a reader ignores what it does not know. Numbers are doubles.

**Exit codes**: 0 done, 1 failed, 2 not implemented, 3 replay not reproduced, **4 cancelled** (a resumable run).

## 3. Cancel and resume

**Run directory.** `fit` and `transfer` create `runs/<timestamp>-<name>/` at the start (before anything can
fail) with `run.json`: `kind`, `status` (`running`, `cancelled`, `finished`, `failed`), the saved options
(`args`), and the latest `loss_db`, `evals`, `elapsed_s`, `time_cap_s`. It is rewritten atomically at every
change, so a killed process leaves a readable state; `running` with no process is an **interrupted** run and is
resumed like a cancelled one. `mimicry runs [--json]` lists them.

**Asking for a cancel** (any of these; the run finishes the batch in flight, then stops cleanly):
Ctrl-C / SIGTERM / SIGBREAK (a second one ends the process at once), the line `cancel` on stdin (with
`--events`), the end of stdin with `--cancel-on-eof` (a front end that died), or `mimicry cancel <run dir>`
(creates `cancel` in the run directory; polled every 250 ms and removed by the next resume). Workers ignore
SIGINT, so a Ctrl-C in a terminal reaches only the supervisor.

**What a cancelled run keeps.** Everything a finished run writes (spec 07 §1): `best.params.json`,
`best.state`, `match.wav`, `ab.wav`, `diff.ppm`, `report.md`, `result.json` (`"status": "cancelled"`), plus
`checkpoint.json`. The best point so far is the one answer; the top-K set needs re-renders and is selected only
by a run that finishes.

**Checkpoint** (`opt/Checkpoint`, spec 04 §10). Written atomically at the end of every finished stage and every
`--checkpoint-s` (30 s) of run time, and at a cancel. It holds the finished stages, the best vector, the S1
points and finalists (full vectors, CLAUDE.md rule 5), run time, evaluation count, outcome counts and stage
reports.

**Resume** (`mimicry fit --resume <run dir>`, `mimicry transfer --resume <run dir>`): the saved options are
used, options on the command line override them, `--more-minutes M` adds M minutes to the time cap (default:
what is left; a run whose cap is used up says so). Stages that finished are skipped; the stage that was cut
short starts again from the best point (the CMA-ES state is not stored [D]); the best point and the stored
points are re-scored first (a few hundred renders at most) and the seed changes with every session. Time
cap, patience and evaluation budget count the whole run.

**Transfer.** Each source preset has its own folder in the run (`NNN-<name>/` with `source.state`,
`source.wav` and the fit's files); `bank.json` and `bank-report.csv` list status, loss, evaluations and time
(`pending`, `running`, `done`, `cancelled`, `failed`, `source-failed`). A resume skips finished presets and
resumes the cancelled one inside its own checkpoint.

**Harvest** has no run directory: `mimicry harvest --continue` reads the library's `index.json`, takes over the
presets already tried (ok ones keep their numbers and are re-rendered once for the duplicate check) and tries
only the rest. A cancelled harvest writes the library with what it has (`"cancelled": true` in `index.json`);
`presets/<plugin>/cancel` is the cancel file.

## 4. Front end architecture

```
 mimicry_gui.exe                                   mimicry.exe  ── workers (plugins)
 ┌───────────────────────────┐   args, "cancel"   ┌─────────────┐
 │ Forms ──> build_args ─────┼───────────────────>│ fit / ...   │
 │ RunModel <── events ──────┼<── stdout lines ───│ --events    │
 └───────────────────────────┘                    └─────────────┘
```

- `gui/Forms`: the forms as data (a field table per command: key, label, kind, option) and `build_args`,
  `build_resume_args`, `missing_fields`, `split_words`, `scan_plugins`, `Settings`. The window builds its
  controls from the table, so a new option is one table line.
- `gui/ChildProcess`: starts `mimicry` with pipes (Windows `CreateProcess`, POSIX `fork`/`exec`), delivers
  stdout and stderr as lines on reader threads, writes lines to its stdin, ends it on request.
- `gui/RunModel`: applies events to a model (state, fraction, loss history, stage rows, harvest rows, bank
  rows, log, ETA, status line). A line that is not JSON is log text, a process that ends without `done` has
  failed.
- `gui/win/*`: the Win32 code. Reader threads post lines to the UI thread (`WM_APP`); the UI thread owns the
  model and repaints on a 250 ms timer.

**Text encoding on Windows** [A]: the command line receives its arguments in the ANSI code page (the C `main`), and paths and
JSON are written in the same bytes, so the window converts between UTF-16 and the **ANSI code page**, not UTF-8
(`gui/win/WinUtil.hpp`). A folder or plugin name the code page can write survives the round trip (resume, open
folder); names outside it do not, as for every other command (plan OQ16).

The window starts `mimicry` with `--events --cancel-on-eof` in the **work folder** (default
`Documents\Mimicry`), so `runs`, `profiles` and `presets` are made there. `mimicry.exe` is looked for next to
`mimicry_gui.exe`.

## 5. The window

A tab per command (forms in `gui/Forms.cpp`), a **Runs** tab, and below them the monitor, which is the same for
every command.

| Tab | Command | Main fields |
| --- | --- | --- |
| Fit a WAV | `fit` | plugin, target WAV, minutes; more: patience, evaluation budget, mode, note, kind, restarts, alternatives, seed, workers, folders, extra options |
| Transfer presets | `transfer` | source plugin, target plugin, minutes per preset, first N presets; more: preset filter, library, note, gate, velocity, total minutes |
| Harvest presets | `harvest` | plugin, files / folders, "continue an earlier harvest"; more: method, all methods, limit, wait |
| Probe, Fingerprint | `probe`, `fingerprint` | plugin |
| Score a preset | `eval` | plugin, preset (`best.state` or `.vstpreset`), target WAV |
| Runs | `runs` | list of run directories with status; double-click resumes a resumable run, otherwise opens its folder |

- **Plugin fields** are editable lists of the CLAP and VST3 plugins found in the standard folders
  (`%COMMONPROGRAMFILES%\VST3` and `\CLAP`, `%LOCALAPPDATA%\Programs\Common\...`, `CLAP_PATH`); a plugin can be
  typed, browsed to, or **dropped on the window** (as can a WAV or a preset file).
- **Monitor**: status line (what is happening, best loss), progress bar (a bank counts finished presets plus
  the running one), elapsed time, evaluations per second, ETA (the time cap or the evaluation budget, whichever
  comes first; a bank extrapolates from finished presets), the **loss curve** (best loss over run time, log axis
  when the range is wide), a table (pipeline stages for a fit, presets with outcomes for a harvest, the bank
  for a transfer) and the log.
- **Buttons**: *Start*; *Cancel* (sends `cancel`; pressing it again becomes *Force stop*, which ends the process
  and leaves the last checkpoint); *Resume* (the cancelled or interrupted run; "+ minutes" adds time);
  *Open folder*, *Play A/B* (`ab.wav`: target, pause, match), *Report* (`report.md`), *Spectrogram* (`diff.bmp`, the image of `diff.ppm` as a BMP, which Windows opens without a PPM viewer).
- Closing the window during a run asks, then cancels the run and waits for it to save (45 s at most).
- Values are remembered in `%APPDATA%\Mimicry\gui.json`.

## 6. Tests

| ID | Test |
| --- | --- |
| T21 | **Cancel and resume** (`opt`, `cli_*`): a cancelled pipeline keeps its best point and a checkpoint, unfinished stages are not marked done, a resumed run never loses ground, counts evaluations, time and sessions across sessions, skips finished stages and reaches the optimum an uninterrupted run reaches. Through the command line in both formats: `--events` is pure JSON lines starting with `start` and ending with `done`, `mimicry cancel` stops a fit (exit 4, results and checkpoint exist), `--resume` finishes it in the same directory; a transfer is cancelled inside its second preset and resumed to the end (CLAP to CLAP, VST3 to VST3, CLAP to VST3) |
| T22 | **Front end logic** (`gui_core`, `gui_e2e_*`): forms to command lines, event lines to the run model (fit, bank, harvest, a dead process), settings and plugin scan; end to end through `ChildProcess`: progress reaches the model, `cancel` on stdin cancels, the form's resume arguments continue the run, closing stdin cancels (`--cancel-on-eof`) |
| T14 | `.vstpreset`: layout and round trip (`output`), `best.vstpreset` of a VST3 fit renders like `best.state` (`mimicry eval`, `cli_vst3`) |
| T10 | Output: `diff.ppm` is a valid PPM with the three panels, the difference panel is white for equal signals (`output`), `ab.wav`/`report.md` exist after a cancel (`cli_*`) |

The Win32 code (`gui/win`) cannot run in CI: the build checks that it compiles and links on Windows, and a
manual pass on a Windows machine covers it (plan OQ16): start each command from its tab, watch the monitor,
cancel and resume a fit and a transfer, drop a plugin on the window, close during a run, 125 % and 150 % display
scaling.

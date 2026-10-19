# 08 — Diagnosis report (`mimicry doctor`) [A]

`mimicry doctor <plugin>` measures what a plugin does and what each parameter does to it, and writes a
self-contained report. The idea is close to DDMF PluginDoctor (frequency response, harmonics, dynamics
and impulse response of an effect, with live parameter changes) [I: a public product, not ported],
extended to instruments and to the per-parameter view the optimizer already needs. It reuses the
probe (04 §3), the profile (01 §7), the effect probe signals (06 §5) and the parameter roles (04 §2);
the doctor adds measurements and a renderer, not a second analysis path.

## 1. Use

- Understand an unknown plugin before a fit or a transfer: which parameters matter, which are dead,
  which are dangerous, which interact.
- Check the host: latency, tail, repeatability, offline vs realtime difference, render cost, in either
  format, and whether the CLAP and VST3 builds of one plugin agree (T13 on any plugin).
- Compare two plugins side by side (`mimicry doctor A B`), with the parameter map (04 §8) between them:
  the report a user reads before a transfer.

## 2. Measurements

**Every plugin** [A]:

| Section | Content |
| --- | --- |
| Identity | format, uid, vendor, version hash, kind, buses, latency, tail report, state support, preset methods found (06 §6) |
| Health | repeatability floor, offline vs realtime difference (OQ8), render cost per second of audio, failures and hazards met by the probe, safe box |
| Parameters | table: id, name, module, units, value text at 0/25/50/75/100 %, role and confidence (04 §2), class (dead / pitch / brightness / time / level / spectrum / fx), log or linear, safe range, sensitivity |
| Interactions | heat map of the 2×2 interaction probes (04 §3) for the strongest groups |
| Map | with two plugins: the parameter map with confidence (04 §8) |

**Instruments** (rendered through the note probe plan, 06 §2): per parameter, a curve of each probe
feature against the coordinate (centroid, level, decay time, f0 shift, noisiness, tail length) and a
spectrum and envelope overlay at the five probe values; key tracking and velocity response at defaults.

**Effects** (rendered through the effect probe signals, 06 §5), as in PluginDoctor [I]:

| View | Signal | Shows |
| --- | --- | --- |
| Frequency response | impulse and log sweep | magnitude (and group delay, the one phase view kept) |
| Harmonics | sine at 1 kHz (and 100 Hz) | harmonic spectrum, THD, THD+N |
| Dynamics | sine at stepped levels | input to output level curve, compression ratio and knee, attack and release from a level step |
| Impulse response | impulse | waveform, energy decay, RT60 / tail length |
| Modulation | long sustained tone | spectrogram, modulation rate and depth |
| Noise | silence | self-noise, hum, self-oscillation |

Each view is drawn at defaults and overlaid at the five probe values of every active parameter, so the
user sees what a knob does to the response, the static version of PluginDoctor's live view. Stereo
views wait for 7.3.

## 3. Output

`doctor/<format>-<uid>-<hash>/`: `report.html` (one self-contained file, inline SVG plots written by
our own small SVG writer, no scripts from the network, no dependency), `report.json` (every number
behind the plots), and the probe renders as WAV on request (`--keep-audio`). The renders go through the
worker pool like every other render (CLAUDE.md rule 4), failures are shown in the report (rule 6), and
the profile is updated as a side effect.

Defaults [D]: 48 kHz, the probe's five values per parameter, sweep 10 s from 20 Hz to 20 kHz, level
steps of 6 dB from -60 to 0 dBFS. A 100-parameter effect takes about `5·n·signals` renders; `--quick`
measures defaults only.

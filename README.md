# SyncTrack Prep

**English** | [简体中文](README.zh-CN.md)

A real-time **VST3** insert effect for production (sync) audio tracks. It repairs broken channel layouts, tames the noise floor, levels dialogue, and enforces a true-peak ceiling — so location dialogue stays intelligible underneath a music bed.

<p align="center">
  <img src="assets/screenshot.png" alt="SyncTrack Prep plugin interface" width="320">
</p>

---

## What it does

When you import production audio from a camera or field recorder into a video-post session, you usually hit the same problems: the file is a mono source parked on one channel, the noise floor drifts, dialogue level jumps between takes, and peaks clip once you add music. SyncTrack Prep is a single insert that handles that first-pass cleanup so the sync track is usable as a reference or as a rough mix.

### Signal chain

```
In → ChannelRepair → Leveler → DenoiseStage → ToneShaper → OutputGain → UpwardExpander → PeakCompressor → TruePeakLimiter → Out
```

* **DenoiseStage** is selectable via the **Denoise Mode** control: **Off** (no suppression), **Classic** (in-house spectral STFT/OLA, latency 575), **Live (DFN3)** (DeepFilterNet3 / libDF, realtime, ~30 ms), or **HQ (MossFormer2)** (MossFormer2_SE_48K via ONNX Runtime, offline render only, 4 s lookahead). Latency is reported per mode, so hosts compensate correctly.
* The leveler sits *before* the denoiser on purpose: when the spectral stage lifts quiet content first, the chosen denoiser can then shave the floor back down.
* The leveler publishes a per-sample post-gain *scene level*, indexed by both the compressor and the upward expander — so dynamics follow the level the listener actually hears, not the input snapshot.
* **Output is the chain's makeup gain**, placed *ahead* of the always-on safety stages. The −1 dBTP ceiling therefore holds at *any* knob position: turning Output up lifts quiet content until it meets the ceiling, and anything that would exceed it is clamped by the compressor and limiter. At 0 dB the chain behaves as if the knob were not there.

### Concepts

| Term | Meaning |
|---|---|
| **Production track** | Imported video production audio: dialogue mixed with location SFX. |
| **Channel repair** | Corrects a bad channel layout and long-term L/R energy imbalance. |
| **L-only / R-only** | One channel is below the activity threshold while the other has content — treated as mono parked on one side. |
| **Leveler** | Slow automatic gain that pulls active content toward a target loudness zone (−18 dB). Publishes a per-sample post-gain scene level that downstream dynamics (compressor, expander) follow. |
| **Peak control** | Faster downward dynamics that tames peaks and bangs. |
| **Upward expander** | Low-level expansion: lifts the noise floor *downward* in quiet sections so the chain doesn't squash quiet dialogue. Threshold is scene-coupled, not input-coupled. |
| **Tone shaper** | Tilt shelf + 3 kHz presence peak; a single Tone knob rotates between −1 (warmer/duller) and +1 (brighter/forward). Strict bit-transparent at 0. |
| **True-peak ceiling** | Maximum allowed true-peak level (−1 dBTP). |
| **Denoise mode** | Selectable denoise engine: Off / Classic (in-house spectral STFT) / Live (DeepFilterNet3, realtime) / HQ (MossFormer2, offline only). The Amount knob maps to "denoise strength" inside whichever engine is selected. |
| **Dialogue intelligibility** | Whether production speech is understandable under a music bed. This is the product's success criterion — *not* broadcast LUFS compliance. |

---

## Download & install

Grab the archive for your platform from the [Releases](../../releases) page.

### macOS (Universal 2 — Apple Silicon + Intel)

1. Unzip `SyncTrack-Prep-<version>-macOS-Universal.zip`.
2. Move `SyncTrack Prep.vst3` into either:
   * `~/Library/Audio/Plug-Ins/VST3/` — current user only, no admin password, **recommended**
   * `/Library/Audio/Plug-Ins/VST3/` — all users, needs an admin password
3. **Clear the quarantine flag.** The build is not notarized by Apple, so macOS will refuse to load it straight out of a downloaded zip. In Terminal:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/VST3/"SyncTrack Prep.vst3"
   ```

   If your DAW still won't load it, open **System Settings → Privacy & Security**, scroll to the bottom, and click **Open Anyway** next to the blocked item.
4. Fully quit and relaunch your DAW, then rescan VST3 plug-ins.

### Windows (x64)

1. Unzip `SyncTrack-Prep-<version>-Windows-x64.zip`.
2. Move `SyncTrack Prep.vst3` into `C:\Program Files\Common Files\VST3\` (you will need administrator rights).
3. Restart your DAW and rescan VST3 plug-ins.

> The Windows binary is verified by an external grey-tester running Nuendo on Windows; see [Known limitations](#known-limitations) for the residual gaps.

---

## Host support

| DAW | Status |
|---|---|
| **Cubase** | ✅ Verified — runs stably in real time and in offline bounce (tested on macOS) |
| **Nuendo** | ✅ Verified — runs stably in real time and in offline bounce (tested on macOS) |
| Reaper | ⚠️ Not verified |
| FL Studio | ⚠️ Not verified |
| Studio One | ⚠️ Not verified |
| Pro Tools | ❌ Not supported — needs AAX, which is not built yet |
| Logic Pro | ❌ Not supported — needs AU, which is not built yet |

Only the **VST3** format is built today. AAX and AU are on the roadmap.

### If a bounce sounds unprocessed

If offline metrics look right but the exported file is dry, the plug-in is almost certainly not in the render path. Check, in order:

1. The insert is **enabled** (power button lit) on the **same track** you are exporting.
2. A **Soft** export and a **Strong** export are *not* bit-identical.
3. The plug-in's IN/OUT meters move during playback.
4. The insert is on a **stereo** track, not a multi-mono slot.
5. After rebuilding the plug-in: fully quit the DAW, rescan VST3, and re-insert the plug-in.

---

## Using it

There are six controls, and that is deliberate — the presets carry the tuning.

| Control | Range | Notes |
|---|---|---|
| **Preset** | Soft / Strong / Clean | Default **Strong**. Switching preset also sets the denoise-mode default (Soft=Off, Strong=Live, Clean=Classic); your manual mode choice is kept until you switch preset again. |
| **Denoise Mode** | Off / Classic / Live (DFN3) / HQ (MossFormer2) | Classic = in-house spectral. Live = realtime NN. HQ = offline NN (during playback it silently degrades to Live and shows a hint). |
| **Amount** | 0–100 % | Unified "denoise strength": maps to over-subtraction in Classic, attenuation limit in Live, wet/dry mix in HQ. |
| **Tone** | −1 … +1 | Tilt + presence at 3 kHz; bit-transparent at 0. |
| **Output** | −inf … +24 dB | Makeup gain ahead of the safety stages. Skewed range with 0 dB at the centre; reads "-inf" below −60 dB. The −1 dBTP ceiling always holds. |
| **Bypass** | on / off | True bypass — the input is passed through untouched. |

**Which preset?**

* **Soft** — light touch. Use when the source is already fairly clean and you only want layout repair plus gentle levelling.
* **Strong** *(default)* — the workhorse. Heavier levelling (up to +18 dB of gain, 3:1 peak control) and realtime NN denoise. For takes with wide level swings on a noisy location.
* **Clean** — the leveler backs off and the denoiser does the work (Classic mode by default). Use when a steady noise floor is the main problem.

All three presets share the same targets: leveler target −18 dB, high-pass at 70 Hz inside the denoiser, true-peak ceiling −1 dBTP, peak compressor always on.

Only stereo in / stereo out is supported.

> **State compatibility note:** legacy sessions saved without `denoiseMode` (the old Denoise on/off bool) load on **Live** when the bool was on, and **Off** when it was off — an upgrade mapping documented in the changelog. The Output range also changed, so existing Output values load shifted; see [Known limitations](#known-limitations).

---

## Known limitations

Being upfront about these will save you an issue report.

* **HQ (MossFormer2) requires offline rendering.** It runs only when the host signals non-realtime (Nuendo Direct Offline Processing and the bounce export path). During realtime playback the chain silently degrades to **Live (DFN3)** and the editor shows a hint. Plan ~4s of extra reported latency for HQ, which the host's delay compensation will absorb.
* **Classical spectral denoise on real material can be undramatic** when no NN tier is active. The in-house Classic denoiser is great on synth steady noise but quiet on real room tone; pick **Live** (DFN3) or **HQ** (MossFormer2) when the noise is the primary problem.
* **The noise estimator in Classic needs about a second to settle.** It learns the noise spectrum mid-stream, so if a clip opens on speech the first few STFT frames can briefly learn dialogue instead of noise. It self-corrects in roughly one second.
* **Saved sessions from v0.1.x load shifted.** The new **Output** range is −inf…+24 dB (skewed, 0 dB at the centre) — the old −24…+12 range stored 0 dB at a different normalised position, so existing Output values load different. Legacy projects that used the Denoise on/off checkbox now open with denoise on ⇒ **Live (DFN3)**. Both are documented upgrade mappings, not bugs.
* **Stereo only.** No surround or multi-channel support.
* **Not a delivery tool.** SyncTrack Prep does not do broadcast loudness delivery (EBU R128 masters), AI dialogue/music separation, or offline spectral repair and de-clipping. It is a first-pass cleanup insert for sync tracks.

---

## Build from source

Requirements: a C++17 compiler, **CMake ≥ 3.21**, and network access on the first configure (JUCE 8.0.8 and Catch2 3.4.0 are fetched automatically).

```bash
git clone <this repo>
cd "SyncTrack Prep"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
ctest --test-dir build --output-on-failure
```

On macOS this produces a Universal 2 (arm64 + x86_64) binary. `COPY_PLUGIN_AFTER_BUILD` is on, so the VST3 is copied into your user plug-in folder as part of the build.

Already have JUCE or Catch2 checked out? Point CMake at them to skip the download:

```bash
cmake -B build -DSTP_JUCE_PATH=/path/to/JUCE -DSTP_CATCH2_PATH=/path/to/Catch2
```

Build targets:

| Target | What it is |
|---|---|
| `SyncTrackPrep` | The VST3 plug-in (plus a Standalone app for quick auditioning) |
| `SyncTrackPrepOffline` | CLI renderer — process a WAV without a DAW |
| `SyncTrackPrepTests` | Catch2 DSP unit tests |
| `SyncTrackPrepStateTests` | Catch2 processor/APVTS state tests |

Pass `-DSTP_BUILD_TESTS=OFF` to skip the test targets.

---

## Offline rendering and A/B analysis

The offline renderer runs the exact same DSP chain as the plug-in, so you can prove out the processing — and compare presets — with no DAW involved.

```bash
cmake --build build -j 8 --target SyncTrackPrepOffline
./build/SyncTrackPrepOffline input.wav output-strong.wav strong 1
```

Arguments: `<in.wav> <out.wav> [soft|strong|clean] [off|classic|live|hq] [amount 0-100|-1] [tone -1..1]` plus `--tap denoise|final`, `--expander 0|1`, `--flush <seconds>`, `--dfn3 <model.tar.gz>`, `--moss <model.onnx>`.

Three analysis helpers live in `scripts/` (Python 3; `analyze_ab.py` and `diagnose_stereo_noise.py` need `ffmpeg` on your `PATH`):

```bash
# Full A/B report: loudness gap, steady-noise level, true peak
python3 scripts/analyze_ab.py --gold input.wav --proc output-strong.wav --run-id my-run

# Why did the leveler's noise gate open or close here?
python3 scripts/diagnose_gate.py input.wav --noise-window 30:40 --dialogue-window 0:14

# Is the noise actually stereo, or is it one channel leaking?
python3 scripts/diagnose_stereo_noise.py --gold input.wav
```

`analyze_ab.py` writes a per-second CSV, a one-row aggregate CSV with pass/fail flags, and a readable Markdown report.

---

## Roadmap

* Stream-ify MossFormer2 — causal conversion + INT8 re-attempt — so HQ can become a realtime Live tier
* AAX (Pro Tools) and AU (Logic Pro) builds
* Validate against Reaper, FL Studio and Studio One
* More Linux coverage (LV2 / CLAP)

---

## License

SyncTrack Prep is released under the **GNU Affero General Public License v3.0 or later** (AGPL-3.0-or-later). See [LICENSE](LICENSE) for the full text.

Why AGPL: this plug-in links the [JUCE](https://juce.com) framework, whose modules are dual-licensed under AGPLv3 or a commercial JUCE licence. Distributing it under AGPLv3 is the licence-compatible way to ship it as free software. If you fork it and distribute a binary, you must publish your source under the same terms.

Third-party components bundled with the plug-in (`NOTICE` for the full table):

* **JUCE 8.0.8** — AGPLv3 or commercial JUCE licence. Fetched at configure time; not vendored here.
* **Steinberg VST3 SDK** — bundled inside JUCE, dual-licensed proprietary or GPLv3.
* **Catch2 3.4.0** — Boost Software License 1.0. Test-only dependency, fetched at configure time.
* **DeepFilterNet3** (`libdf`) + **DeepFilterNet3 model** — MIT / Apache-2.0. Live denoise engine. Bundled inside the VST3.
* **ONNX Runtime** — MIT. MossFormer2 inference engine. Bundled inside the VST3 (`Contents/Frameworks/libonnxruntime.dylib`).
* **MossFormer2_SE_48K** weights — Apache-2.0. HQ denoise model. Bundled inside the VST3 (`Contents/Resources/mossformer2/`).

VST is a trademark of Steinberg Media Technologies GmbH, registered in Europe and other countries.

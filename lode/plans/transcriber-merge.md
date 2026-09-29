# Plan: Unify `Transcriber` into `libaudio`

> **Status: ✅ Complete & verified** (this document is both the executable plan and
> the record of what was done).

## 1. Problem

Two `Transcriber` classes existed in the project, which was confusing and violated
DRY:

| Class | Ownership | Role (before) |
|---|---|---|
| `libaudio::Transcriber` | libaudio | **Abstract port** — the `Analyzer` seam (header-only base). |
| `midicapture::Transcriber` | midicapture | **Tier-1** concrete engine (aubio YINfft + spectral-flux onsets + defrag). |
| `midicapture::AubioTranscriber` | midicapture | Thin **adapter** exposing the aubio engine *through* the `libaudio::Transcriber` port. |
| `libaudio::BasicPitch` | libaudio | **Tier-2** concrete engine (ONNX/Core ML), already `: public libaudio::Transcriber`. |

So "Transcriber" meant both *the port* and *an engine*, in two different namespaces.

## 2. Goal

A **single `libaudio::Transcriber`** owned by **libaudio**. `midicapture` consumes
`libaudio::Transcriber` (not a global `::Transcriber` of its own) and no longer
ships its own transcription engine or adapter. Regression results must be
**byte-identical** to baseline.

## 3. Key decisions (all explicitly approved by the user)

- **A. Rename the port to `Analyzer`.** Frees the `Transcriber` name for the one
  concrete engine. The abstract port is now `libaudio::Analyzer`; the aubio engine
  is now the concrete `libaudio::Transcriber : public Analyzer`.
- **B. Reuse-pattern = Option 1.** The corpus harness builds **one** analyzer and
  calls `transcribe()` per file (reusing it), matching the `BasicPitch` reuse
  contract. `midicapture` therefore relies on a single engine instance being safe
  to reuse across a whole corpus.
- **C. Fix C2 (chosen by the agent; minimal & correct).** `Transcriber::Impl`
  extracts the detector construction into a shared `rebuildDetectors()`, called
  from **both** the constructor and `resetForNewFile()`. This makes the "safe to
  reuse" contract actually true and byte-equivalent to a fresh engine per file.

## 4. Architecture (after)

```
libaudio::Analyzer                 (abstract port: virtual Score transcribe(path) const = 0; name() = 0)
   ├── libaudio::Transcriber       (concrete Tier-1: aubio YINfft + specflux onsets + hysteresis + defrag)
   └── libaudio::BasicPitch        (concrete Tier-2: ONNX / Core ML)

midicapture/   (thin CLI front-end — owns no engine)
   ├── main.cpp                     → single-file transcription + `--run-corpus`
   └── corpusHarness.{h,cpp}        → analyzer-agnostic 14-file corpus (one reused Analyzer)
```

Deleted from `midicapture`: `include/midicapture/transcriber.h`, `src/transcriber.cpp`,
`include/midicapture/aubioTranscriber.h`, `src/aubioTranscriber.cpp`.
New in `libaudio`: `include/libaudio/analyzer.h` (port), `src/transcriber.cpp` (+ the existing
`include/libaudio/transcriber.h`, now the concrete engine). `basicPitch.h` re-bases to
`: public Analyzer`. `src/tier2/audioDecode.cpp` moved to `src/audioDecode.cpp` (Tier-1)
because the Tier-1 engine's ffmpeg fallback needs `AudioSource`.

## 5. Implementation (Steps 1–2, done by the previous agent; build clean, 84/84)

1. Create `libaudio/include/libaudio/analyzer.h` — the abstract `Analyzer` port
   (the old `libaudio::Transcriber` base, renamed).
2. Re-base `basicPitch.h` to `: public Analyzer`.
3. Move the aubio engine into libaudio as the concrete `libaudio::Transcriber :
   public Analyzer` (`src/transcriber.cpp`); delete `midicapture`'s `transcriber.*`
   and `aubioTranscriber.*`.
4. Point `midicapture::main.cpp` at `libaudio::Transcriber`; make `corpusHarness`
   use the `libaudio::Analyzer` port (`aubio → libaudio::Transcriber`,
   `basic-pitch → libaudio::BasicPitch`), reusing one analyzer across the corpus.
5. Move `audioDecode` to Tier-1; update both `CMakeLists.txt`, both `README.md`.

## 6. The bug found (and fixed) in the reused-engine path

Regression first **failed** on the corpus `aubio` analyzer: detected counts drifted
(e.g. velocity-soft-loud 2→6, low-octave 7→9; summary precision 4.4%→4.8%, onset
7.5→4.8 ms) because the corpus **reuses one engine** and the onset state leaked
across files.

**Root cause.** `OnsetDetector` holds cross-frame state — aubio's internal
phase-vocoder buffer plus the latch edge-detector (`primed`, `prevOnsetSample`,
`lastOnsetTime`, `lastConfidence`). `OnsetDetector::setSampleRate()` is a **no-op
when the rate is unchanged**, and every corpus file is 48 kHz == the 48000
placeholder, so `transcribe()`'s `setSampleRate(48000)` never rebuilt the detector
and the phase-vocoder buffer leaked file → file. The old corpus avoided this by
building a **fresh** engine per file (which is why the `aubio` and `python`
baselines are equal). `PitchDetector` (YINfft) is a pure per-window function — no
result-affecting cross-call state.

**Fix (C2).** In `libaudio/src/transcriber.cpp`, `Transcriber::Impl` extracts the
detector construction into `rebuildDetectors()` (re-creates both detectors from the
stored `bufSize`/`hopSize`/`pitchMethod`); it is called from the constructor **and**
from `resetForNewFile()`. Detector (re)construction is **deterministic**, so a
rebuilt detector is byte-identical to a freshly-constructed one. Result: a reused
engine handed a fresh detector per file == a fresh engine per file, and the fresh
path is unaffected (rebuild@48000 == construct@48000).

## 7. Regression (all must be byte-identical) — ✅ all pass

Render into the baseline path; compare per-harness (the two harnesses print different
preambles). Baselines in `lode/tmp/transcriber-merge-baseline/`; the `aubio`+`python`
baselines are the fresh-engine reference (equal to each other).

| # | Command (engine) | Compare against | Result |
|---|---|---|---|
| 1 | `python3 midicapture/render_test_suite.py --out …/baseline --midicapture …/midicapture/build/bin/midicapture` (Tier-1, fresh/file) | `baseline-python.txt` (full) + 14 `.mid.detected` vs `lode/tmp/…/detected/` + 14 re-rendered `.mp3` | ✅ byte-identical text; 14/14 detected; 14/14 mp3 |
| 2 | `…/midicapture/build-tier2/bin/midicapture --run-corpus …/baseline --analyzer aubio --ffmpeg /opt/homebrew/bin/ffmpeg` (Tier-2, **reused**) | `baseline-corpus-aubio.txt` (strip leading onnx lines) | ✅ byte-identical — **the previously-failing leak is fixed** |
| 3 | same, `--analyzer basic-pitch` (Tier-2, reused) | `baseline-corpus-basicpitch.txt` (strip leading onnx lines) | ✅ byte-identical |

Also green: `test_libaudio` **84/84** (Tier-1 and Tier-2 builds) and
`test_libaudio_tier2` smoke **PASSED**. All four trees
(`midicapture/build`, `midicapture/build-tier2`, `libaudio/build`,
`libaudio/build-tier2`) compile clean with `-Werror`.

## 8. Verification notes / gotchas

- `render_test_suite.py` finds ffmpeg via `shutil.which`; the baseline ran with
  `/opt/homebrew/bin/ffmpeg`, so that must be on `PATH` for a byte-matching run.
- The corpus `aubio` path loads no ONNX model, so its output has **no** onnx warning
  lines and matches the baseline byte-for-byte. The `basic-pitch` path emits 3
  onnxruntime CoreML warning lines carrying wall-clock timestamps / PIDs that never
  byte-match; strip everything before the first `midicapture test-suite` line.
- Detected files are deterministic SMF (note data + tempo; no wall-clock
  timestamps) → byte comparison is valid.

## 9. Docs updated

`lode/midicapture/summary.md` (purpose + file tree + "behind the Analyzer port"),
`lode/summary.md` (Audio→MIDI row + transcription note), `lode/audio-to-midi.md`
(§7.1 ports & §7.3 reuse), `lode/libaudio/summary.md` (§9 current-state note + Phase-1
roadmap bullet), `lode/libaudio/decisions.md` (type-list forward note). The two
`README.md` files were already updated by the previous agent. (`writer.md` / `hir.md`
are unaffected — they cover the MIDI writer and HIR types, not the engine.)

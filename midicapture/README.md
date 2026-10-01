# midicapture

Audio-to-MIDI transcription utility — converts audio recordings (AIFF, WAV, FLAC, MP3, …) to Standard MIDI Files (SMF).

## Overview

`midicapture` is a command-line utility that analyzes an audio recording and generates a **Type 1 MIDI file** (480 ticks per quarter note) compatible with Apple Logic Pro. It is a thin CLI front-end on top of **libaudio**: it selects a transcription **engine** (`libaudio::Analyzer`), runs it on the file, and writes the resulting HIR `Score` to MIDI.

Two engines are selectable with `--model`:

| Model | Engine | Character |
|-------|--------|-----------|
| `basic` *(default)*, `basic-pitch` *(synonym)* | `libaudio::BasicPitch` | **Polyphonic** neural engine (ONNX Runtime + Core ML). Models the harmonic series, so it resolves the octave the monophonic YIN path cannot. |
| `aubio` | `libaudio::Transcriber` | **Monophonic** DSP engine (YINfft pitch + spectral-flux onsets). |

`basic` is the **default** in a Tier-2 build (see [Building](#building)). `--analyzer` is a **deprecated alias** for `--model` (it still works, with a warning).

### Architecture

```
Audio/
├── libaudio/             # DSP library (pitch, onsets, transcription, MIDI writing)
│   ├── include/libaudio/
│   │   ├── analyzer.h    # Analyzer — abstract audio → Score port (the seam)
│   │   ├── transcriber.h # Transcriber — Tier-1 monophonic engine (aubio)
│   │   ├── basicPitch.h  # BasicPitch  — Tier-2 polyphonic engine (ONNX/Core ML)
│   │   ├── audioFile.h   # AudioFileReader (libsndfile)  / audioSource (ffmpeg fallback)
│   │   ├── midiFileWriter.h  # MidiFileWriter (HIR → SMF)
│   │   └── hir.h         # Note, ControlEvent, Score (HIR)
│   └── src/             # Implementation (transcriber.cpp, onnx/, basicPitch/, audioDecode.cpp)
├── midicapture/          # Phase 2: Audio → MIDI (THIS MODULE — thin CLI front-end)
│   ├── CMakeLists.txt   # Build config (links libaudio, Boost)
│   ├── include/midicapture/
│   │   └── corpusHarness.h       # Tier-2: analyzer-agnostic 14-file corpus evaluator + makeAnalyzer
│   └── src/
│       ├── main.cpp              # CLI entry point (Boost program_options)
│       └── corpusHarness.cpp     # Tier-2: corpus evaluator/renderer (--run-corpus)
└── lode/midicapture/    # Module documentation
```

The monophonic `libaudio::Transcriber` and the polyphonic `libaudio::BasicPitch` are both
concrete `libaudio::Analyzer`s; `midicapture` consumes them rather than defining its own
pipeline. The shared `makeAnalyzer(name, params)` factory in `corpusHarness.h` is the single
source of truth for the model-name → engine mapping, used by both the direct path and the
corpus evaluator.

## Requirements

- **macOS** (for Homebrew package management; Core ML for Tier-2)
- **CMake 3.20+**
- **aubio** (`brew install aubio`) — DSP (Tier-1 engine + shared onsets)
- **libsndfile** (`brew install libsndfile`) — audio file I/O
- **Boost** (`brew install boost`) — program_options for CLI parsing
- **Tier-2 build additionally needs**:
  - **onnxruntime** (`brew install onnxruntime`) + a working **Core ML** EP
    (Apple GPU/ANE; falls back to CPU if absent)
  - **ffmpeg** (`brew install ffmpeg`) — basic-pitch's front-end resamples +
    downmixes the input to 22050 Hz mono (and is the aubio container-decode fallback)

## Building

midicapture builds **libaudio** as a sub-project, so the tier is chosen by the
`LIBAUDIO_ENABLE_TIER2` option (default **OFF**). Two build flavors exist:

```bash
# Tier-1 build (default): aubio monophonic engine only. No --model flag,
# no --run-corpus; ONNX/Core ML/ffmpeg are not required.
cd midicapture
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
#   -> build/bin/midicapture

# Tier-2 build: adds the polyphonic basic-pitch engine (the default model),
# --model {basic|basic-pitch|aubio}, and the --run-corpus evaluator.
cd midicapture
mkdir build-tier2 && cd build-tier2
cmake .. -DCMAKE_BUILD_TYPE=Release -DLIBAUDIO_ENABLE_TIER2=ON
cmake --build . --config Release
#   -> build-tier2/bin/midicapture
```

## Usage

```bash
# Basic usage — the default model is "basic" (the polyphonic basic-pitch engine):
./build-tier2/bin/midicapture input.aiff output.mid

# Force the monophonic aubio engine (and tune it):
./build-tier2/bin/midicapture --model aubio --window-size 1024 --silence -35 \
    input.aiff output.mid

# "basic-pitch" is a synonym of "basic"; --analyzer is a deprecated alias for --model:
./build-tier2/bin/midicapture --model basic-pitch input.aiff output.mid
./build-tier2/bin/midicapture --analyzer aubio   input.aiff output.mid   # warns

# Evaluate the 14-file corpus (Tier-2 only) with the chosen model:
./build-tier2/bin/midicapture --run-corpus test-midi --model basic

# Show help:
./build-tier2/bin/midicapture --help
```

### Command-Line Options

| Option | Type | Default | Applies to | Description |
|--------|------|---------|------------|-------------|
| `--help, -h` | flag | — | all | Print usage information. |
| `<input.aiff>` *(positional)* | string | (required) | all | Input audio file (AIFF, WAV, FLAC, MP3, …). |
| `[output.mid]` *(positional)* | string | derived from input | all | Output MIDI file (.mid). |
| `--input, -i` | string | — | all | Input audio file. ⚠️ Prefer the positional form: a trailing argument after `--input` is captured as the *output* and the intended input is lost (see [Known Limitations](#known-limitations)). |
| `--model` | string | `basic` | Tier-2 | Transcription model: `basic`/`basic-pitch` (synonyms, the polyphonic basic-pitch engine) or `aubio` (the monophonic engine). **Absent in a Tier-1 build** (only `aubio` exists there). |
| `--analyzer` | string | — | Tier-2 | **Deprecated** alias for `--model` (warns, still works). |
| `--window-size` | int | 2048 | *aubio only* | FFT window size (power of 2). |
| `--hop-size` | int | 512 | *aubio only* | Hop size between frames. |
| `--silence` | float | -40 | *aubio only* | Silence threshold in dB (note on/off hysteresis). |
| `--method` | string | "yinfft" | *aubio only* | Pitch detection method. |
| `--tempo` | float | 120 | all | Tempo in BPM (a playback-rate control; note times stay in seconds). |
| `--ffmpeg` | string | `/opt/homebrew/bin/ffmpeg` | Tier-2 | Path to the ffmpeg executable (basic-pitch resample + downmix). |
| `--no-pitch-bends` | flag | off (bends on) | *basic only* | Skip basic-pitch's pitch-bend extraction. |
| `--multiple-pitch-bends` | flag | off | *basic only* | Route each distinct bent pitch to its own MIDI channel 1..15. |
| `-t, --test` | flag | — | all | Sanity test: write a single middle-C note (C4, vel 100, 1 s) regardless of input; no analysis. |
| `--run-corpus` | string (dir) | — | Tier-2 | Run the 14-file corpus evaluation in DIR with `--model` (no positional input). |
| `--clean` | flag | — | Tier-2 | With `--run-corpus`: regenerate the corpus assets (`.mid` + `.mp3`) first. |
| `--output-dir` | string | `.` | — | Directory for `--generate-test-midi-files` output. |
| `--generate-test-midi-files` | flag | — | all | Generate the 14-file monophonic-scale corpus; all other options ignored. |

\*The `*aubio only*` / `*basic only*` knobs are inert on the other engine (basic-pitch's window and frame rate are fixed by the model; the aubio engine never bends). If you pass an aubio-only knob with the `basic` model, the tool prints a note that it was ignored.

### aubio Pitch Detection Methods (`--method`, aubio engine only)

| Method | Accuracy | Speed | Notes |
|--------|----------|-------|-------|
| `yinfft` | ★★★★☆ | Fast | FFT-optimized YIN, **default** |
| `yinfast` | ★★★☆☆ | Very fast | Approximation, less accurate |
| `fcomb` | ★★★☆☆ | Fast | Harmonic comb filter |
| `schmitt` | ★★☆☆☆ | Very fast | Schmitt trigger (simple, noisy) |

The `basic` model ignores `--method` (its CQT + note/onset/contour maps are produced
inside the neural network).

## Output

The output is a **Type 1 MIDI file** with 480 ticks per quarter note, compatible with:

- Apple Logic Pro (imports as a Software Instrument track)
- Any DAW (Ableton Live, FL Studio, Cubase, etc.)
- MIDI players (VLC, QuickTime, etc.)
- Music notation software (MuseScore, Sibelius, etc.)

### MIDI File Specifications

- **Type**: 1 (multi-track, one track per channel)
- **Division**: 480 ticks per quarter note (Logic Pro default)
- **Instrument**: Acoustic Grand Piano (GM patch 0, channel 0)
- **Sustain pedal**: ON at start, OFF at end (CC#64)
- **Tempo**: from `--tempo` (default 120 BPM)
- **Pitch bends**: the `basic` engine can attach 14-bit pitch-bend curves to notes (CC#0x00, `0xE0`); `--multiple-pitch-bends` spreads distinct bent pitches across channels 1..15.

## Design Notes

### Two engines, one seam

Both engines are `libaudio::Analyzer`s and emit the same HIR `Score`; the shared
`MidiFileWriter` turns either into a Type 1 MIDI file. The default `basic` engine is
**polyphonic** and models the harmonic series, so it resolves the octave that a
monophonic fundamental tracker cannot — this is the engine you get out of the box in a
Tier-2 build.

- **`basic` / `basic-pitch`** (`BasicPitch`, Tier-2): a small neural network that reads the
  whole file and outputs per-pitch **note / onset / contour** maps; the C++ port
  resamples the input to 22050 Hz mono (via ffmpeg), windows it, and post-processes the
  maps into notes (velocity from amplitude; optional pitch-bends from the contour map).
  Because it models the harmonic series, it is the robust choice for **chords** and for
  **weak-fundamental** sources (where YIN locks below the true fundamental — see
  [audio-to-midi](../lode/audio-to-midi.md) §5).
- **`aubio`** (`Transcriber`, Tier-1): a per-hop, **monophonic** pipeline — one note at a
  time. Each hop (512 samples, 10.7 ms) is energy-gated (note *arms* above `--silence`,
  *disarms* after 3 hops below `--silence − 10 dB`); spectral flux marks onsets; the note's
  chroma is a majority vote of YIN estimates with the octave anchored to the loudest hop; a
  defragmentation pass drops <5-hop fragments and merges same-pitch runs. Strong for a single
  melody with a clear fundamental; **cannot resolve chords** and is octave-unreliable on
  weak-fundamental renders.

### Known Limitations

- **Input and output paths are positional; the `--input` flag is awkward.** The input is registered as a *positional-only* name in `desc` (Boost's positional slots cannot carry short flags) and separately re-registered as `--input`/`-i` aliases for the same variable. A value supplied both via the flag and via the positional slot would be a second use of the same option name, so Boost rejects the documented `--input in.aiff out.mid` form with `option '--input' cannot be specified more than once`. Even the flag-only form `midicapture --input in.aiff` mis-parses: the positional slot swallows the trailing argument as the *output*, and the intended input is silently dropped. **Prefer the positional form** (`midicapture in.aiff [out.mid]`); `--output`/`-o` is safe to use, `--input` is not. This is a Boost program_options limitation, documented at the option registration site in `src/main.cpp`.
- **First note of a file is frequently missed.** Onset detection needs a spectral *change*, and a file that begins with audio has none on its first frames. Affects the aubio engine; basic-pitch's inferred-onset logic is more forgiving but can still drop a very first transient.
- **`basic` precision dips on decaying synthetic renders.** On the 14-file `timidity`-render corpus the `basic` engine hits **100% recall** (correct octave + chroma) but **~68% precision**: long/whole notes get fragmented into a few extra same-pitch notes. This is a post-processing artifact of those *synthetic decaying-voice* renders, not a pitch error; on real recorded performances the output is musically sensible.
- **Octave on a *monophonic* engine.** The `aubio` engine resolves the pitch class robustly but the octave only as well as YIN's harmonic model matches the spectrum — unreliable on weak-fundamental renders. If you need robust octaves, use the `basic` model.
- **ffmpeg is required for `basic`.** basic-pitch's front-end resamples/downmixes through ffmpeg (the installed aubio lacks `libsamplerate`). If ffmpeg is missing or `--ffmpeg` points to the wrong path, the `basic` model cannot process the file.

### Future Enhancements

- **Tempo tracking**: estimate the source tempo (rather than taking `--tempo`) and write per-section tempo meta-events.
- **Pedal detection**: infer sustain pedal from spectral-flux / decay patterns.
- **In-memory ffmpeg front-end**: roll the resample/downmix into the binary (static link, in-memory, streaming) so the `basic` model does not shell out to ffmpeg or write a temp file (see `lode/plans/ffmpeg-in-memory.md`).
- **More models**: TF-MAGS (Onsets&Frames) and a Demucs `Separator` for per-stem transcription, all behind the same `Analyzer`/`Separator` seam.
- **Pitch tracking** (continuing): refine basic-pitch's bend extraction and per-pitch channel routing.

### Key Design Decisions

- **`basic` (basic-pitch) is the default model in Tier-2 builds** — the engine that resolves the octave and handles chords. `aubio` is the monophonic fallback / legacy option.
- **`--analyzer` is a deprecated alias** for `--model` (kept for backward compatibility, warns on use).
- **`--model`, `--analyzer`, `--run-corpus`, `--ffmpeg`, and the pitch-bend flags are Tier-2-only.** A Tier-1 build has only the aubio engine and omits them entirely.
- **Engine tuning is per-engine.** `--window-size`/`--hop-size`/`--silence`/`--method` tune only aubio; `--no-pitch-bends`/`--multiple-pitch-bends` tune only basic. Passing a knob to the wrong engine is a no-op with a warning.
- **YINfft** is the default aubio pitch method; **spectral flux** is the default aubio onset method.
- **480 ticks per quarter note**, **Type 1 MIDI** (Logic Pro compatibility).
- **Pimpl pattern** throughout libaudio; **HIR** is the single source of truth for MIDI and future LilyPond output.

## Testing

To test with a recorded AIFF file:

```bash
# Record audio using aiffcapture:
./aiffcapture/build/bin/aiffcapture --duration 60 test.aiff

# Transcribe to MIDI (default model is basic):
./midicapture/build-tier2/bin/midicapture test.aiff test.mid

# Verify the MIDI file (ffprobe is not reliable for small MIDI files — use midicsv + timidity):
midicsv test.mid
timidity -Ow test.wav test.mid
```

To run the 14-file corpus regression (Tier-2):

```bash
./midicapture/build-tier2/bin/midicapture --run-corpus test-midi --model basic
```

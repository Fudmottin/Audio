# libaudio

A C++ DSP library for audio-to-MIDI transcription, wrapping **aubio** and
**libsndfile** (Tier-1: classic DSP) plus **ONNX Runtime** (Tier-2: neural
transcription) in a modern C++ interface with Pimpl encapsulation and RAII
resource management.

## Overview

`libaudio` provides the signal processing pipeline that converts raw PCM
audio samples into analysis results, assembled into a High-level
Instrumentation Representation (HIR) `Score`, and exported as a Standard MIDI
File (SMF).

The library is **tiered**:
- **Tier-1** (always built) is classic, local, dependency-light DSP — pitch,
  onsets, beats, and notes via aubio. This is the monophonic path.
- **Tier-2** (optional, behind `LIBAUDIO_ENABLE_TIER2`) adds a **neural**
  transcriber — Spotify's [basic-pitch](https://github.com/spotify/basic-pitch)
  hosted on ONNX Runtime (+ the Core ML EP) — for polyphonic, pianistic input
  that the monophonic Tier-1 path cannot resolve. Both are `Analyzer`s feeding
  the same HIR `Score`.

### Modules

| Module | Purpose | Dependency |
|--------|---------|------------|
| `AudioFileReader` | Read AIFF/WAV files | libsndfile |
| `AudioSource` | Resolve any audio container to a readable path (libsndfile, else ffmpeg) | libsndfile + ffmpeg |
| `FFT` | Spectral analysis (forward/inverse) | aubio |
| `PitchDetector` | Pitch detection (YIN variants) | aubio |
| `OnsetDetector` | Note onset detection | aubio |
| `BeatTracker` | Beat tracking and tempo estimation | aubio |
| `NoteDetector` | Note detection (onset + pitch + velocity) | aubio |
| `SpectralAnalyzer` | Spectral features (MFCC, chroma, etc.) | aubio |
| `TemporalProcessor` | Resampling, filtering | aubio |
| `Analyzer` | Abstract audio → `Score` port (the analyzer-agnostic seam) | none |
| `Transcriber` | Tier-1 monophonic audio → `Score` engine (YINfft + onset + hysteresis) | aubio + libsndfile |
| `BasicPitch` | Tier-2 polyphonic audio → `Score` engine (the basic-pitch neural model) | ONNX Runtime |
| `OnnxSession` | Tier-2 ONNX Runtime session (Core ML / CPU); the only module that sees ONNX types | ONNX Runtime |
| `HIR` (Note, ControlEvent, Score) | Intermediate representation | none |
| `MidiFileWriter` | Export HIR to SMF (Type 1, 480 ticks/qn) | none |
| `ControlEventExtractor` | Extract MIDI control events (pedals) | none |
| `ScoreBuilder` | Assemble notes + controls into Score | none |
| `VelocityEstimator` | Estimate velocity from audio amplitude | none |
| `NoteTrimmer` | Trim notes to musical boundaries | none |

## Building

### Requirements

- **macOS** (tested on Apple Silicon)
- **CMake 3.20+**
- **aubio** (`brew install aubio`) — Tier-1
- **libsndfile** (`brew install libsndfile`) — Tier-1
- **ONNX Runtime** (`brew install onnxruntime`) — **Tier-2 only** (the Core ML
  EP is included on Apple; a CPU fallback is used if it is unavailable)
- **ffmpeg** — **Tier-2 only, runtime** (basic-pitch resample + downmix
  front-end; an executable path, not a build dependency)

### Build Steps

```bash
cd libaudio
mkdir build && cd build

# Tier-1 (default): classic DSP only.
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Tier-2: add the basic-pitch neural engine (ONNX Runtime + Core ML).
cmake .. -DCMAKE_BUILD_TYPE=Release -DLIBAUDIO_ENABLE_TIER2=ON
cmake --build . --config Release
```

The library will be at `build/libaudio.a`.

> Tier-2 is **OFF by default** so the Tier-1 aubio path stays byte-for-byte
> unaffected and dependency-light. Enabling it embeds the basic-pitch `nmp.onnx`
> model into the library (SHA-256 verified at build time), so a running
> Tier-2 build needs no model file on disk — only the onnxruntime dylib and an
> ffmpeg path at runtime.

## Usage

```cpp
#include <libaudio/libaudio.h>

// Read an audio file.
AudioFileReader reader("recording.aiff");

// Create a pitch detector (YINfft algorithm, piano-focused).
const uint32_t bufSize = 2048;
const uint32_t hopSize = bufSize / 4;  // 75% overlap
PitchDetector pitch(bufSize);

// Process audio frame by frame.
std::vector<float> buffer(hopSize);
while (true) {
   uint32_t framesRead = reader.readMono(buffer.data(), hopSize);
   if (framesRead == 0) break;  // EOF

   auto [pitchMidi, confidence] = pitch.detect(buffer.data(), framesRead);
   if (confidence > 0.5f) {
      // Process detected pitch.
   }
}

// Build the HIR.
Score score;
score.tempo = 120.0;

// Export to MIDI.
MidiFileWriter writer("output.mid");
writer.write(score);
```

## Design Principles

- **C++**: Explicit types, RAII resource management.
- **Pimpl pattern**: All public classes use `std::unique_ptr<Impl>` for
  encapsulation of C library internals.
- **HIR as single source of truth**: `Score` containing `Note` and
  `ControlEvent` objects is the intermediate representation for both
  MIDI and LilyPond output.
- **Piano-first**: Default pitch detection tuned for piano (range 21–108, 88 keys).
- **480 ticks per quarter note**: Logic Pro standard for MIDI output.
- **Sustain pedal (CC#64)**: Fully supported in MIDI output.

## Cross-References

- **lode/libaudio/summary.md** — Module overview and API design
- **lode/libaudio/decisions.md** — Library choices and default parameters
- **lode/libaudio/tier2.md** — Tier-2 ONNX transcription: basic-pitch I/O contract, the 14-file corpus, phasing
- **lode/libaudio/hir.md** — HIR data structures
- **lode/MIDI.md** — MIDI file format (SMF)
- **lode/LilyPond.md** — LilyPond notation

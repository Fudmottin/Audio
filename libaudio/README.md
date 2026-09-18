# libaudio

A C++20 DSP library for audio-to-MIDI transcription, wrapping **aubio**,
**libsndfile**, and **rubberband** in a modern C++ interface with Pimpl
encapsulation and RAII resource management.

## Overview

`libaudio` provides the signal processing pipeline that converts raw PCM
audio samples into analysis results (pitch, onsets, beats, note events),
assembled into a High-level Instrumentation Representation (HIR) `Score`,
and exported as a Standard MIDI File (SMF).

### Modules

| Module | Purpose | Dependency |
|--------|---------|------------|
| `AudioFileReader` | Read AIFF/WAV files | libsndfile |
| `FFT` | Spectral analysis (forward/inverse) | aubio |
| `PitchDetector` | Pitch detection (YIN variants) | aubio |
| `OnsetDetector` | Note onset detection | aubio |
| `BeatTracker` | Beat tracking and tempo estimation | aubio |
| `NoteDetector` | Note detection (onset + pitch + velocity) | aubio |
| `SpectralAnalyzer` | Spectral features (MFCC, chroma, etc.) | aubio |
| `TemporalProcessor` | Resampling, filtering | aubio |
| `RubberbandProcessor` | Time-stretching, pitch-shifting | rubberband (optional) |
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
- **aubio** (`brew install aubio`)
- **libsndfile** (`brew install libsndfile`)
- **rubberband** (`brew install rubberband`, optional)

### Build Steps

```bash
cd libaudio
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The library will be at `build/libaudio.a`.

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

- **Core Guidelines compliant**: C++20, explicit types, RAII.
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
- **lode/libaudio/hir.md** — HIR data structures
- **lode/MIDI.md** — MIDI file format (SMF)
- **lode/LilyPond.md** — LilyPond notation

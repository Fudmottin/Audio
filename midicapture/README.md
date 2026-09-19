# midicapture

Audio-to-MIDI transcription utility — converts audio recordings (AIFF, WAV, FLAC) to Standard MIDI Files (SMF).

## Overview

`midicapture` is a command-line utility that analyzes audio recordings and generates MIDI files. It uses **aubio** for pitch detection (YINfft) and onset detection (spectral flux), and writes **Type 1 MIDI files** (480 ticks per quarter note) compatible with Apple Logic Pro.

### Architecture

```
Audio/
├── libaudio/             # DSP library (pitch, onsets, MIDI writing)
│   ├── include/libaudio/ # Public headers (PitchDetector, OnsetDetector,
│   │                       #  AudioFileReader, MidiFileWriter, Score, Note)
│   └── src/             # Implementation (aubio wrappers, MIDI writer)
├── midicapture/          # Phase 2: Audio → MIDI (THIS MODULE)
│   ├── CMakeLists.txt   # Build configuration
│   ├── README.md        # This file
│   ├── include/midicapture/
│   │   ├── transcriber.h         # High-level transcription API
│   │   └── audioFile.h           # Audio file reading (libsndfile)
│   └── src/
│       ├── main.cpp              # CLI entry point (Boost program_options)
│       ├── transcriber.cpp       # Transcription pipeline (pitch + onset)
│       └── audioFile.cpp         # Audio file I/O (libsndfile)
└── lode/midicapture/    # Module documentation
```

## Requirements

- **macOS** (for Homebrew package management)
- **CMake 3.20+**
- **aubio** (`brew install aubio`) — DSP library (pitch, onsets, beats, notes)
- **libsndfile** (`brew install libsndfile`) — Audio file I/O
- **Boost** (`brew install boost`) — program_options for CLI parsing

## Building

```bash
cd midicapture
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The binary will be at `build/bin/midicapture`.

## Usage

```bash
# Basic usage (monophonic prototype):
./build/bin/midicapture input.aiff output.mid

# With custom parameters:
./build/bin/midicapture --window-size 1024 --confidence 0.7 \
    input.aiff output.mid

# With custom method and tempo:
./build/bin/midicapture --method yinfast --tempo 144 \
    input.aiff output.mid

# Show help:
./build/bin/midicapture --help
```

### Command-Line Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `--help, -h` | flag | — | Print usage information. |
| `--input` | string | (required) | Input audio file (AIFF, WAV, FLAC, etc.). |
| `--output, -o` | string | (required) | Output MIDI file (.mid). |
| `--window-size` | int | 2048 | FFT window size (power of 2). |
| `--hop-size` | int | 512 | Hop size between frames. |
| `--confidence` | float | 0.5 | Pitch detection confidence threshold (0.0–1.0). |
| `--silence` | float | -40 | Silence threshold in dB. |
| `--tempo` | float | 120 | Tempo in BPM. |
| `--method` | string | "yinfft" | Pitch detection method. |

### Pitch Detection Methods

| Method | Accuracy | Speed | Notes |
|--------|----------|-------|-------|
| `yinfft` | ★★★★☆ | Fast | FFT-optimized YIN, **default** |
| `yinfast` | ★★★☆☆ | Very fast | Approximation, less accurate |
| `fcomb` | ★★★☆☆ | Fast | Harmonic comb filter |
| `schmitt` | ★★☆☆☆ | Very fast | Schmitt trigger (simple, noisy) |

## Output

The output is a **Type 1 MIDI file** with 480 ticks per quarter note, compatible with:

- Apple Logic Pro (imports as a Software Instrument track)
- Any DAW (Ableton Live, FL Studio, Cubase, etc.)
- MIDI players (VLC, QuickTime, etc.)
- Music notation software (MuseScore, Sibelius, etc.)

### MIDI File Specifications

- **Type**: 1 (multi-track, one track for piano)
- **Division**: 480 ticks per quarter note (Logic Pro default)
- **Instrument**: Acoustic Grand Piano (GM patch 0, channel 0)
- **Sustain pedal**: ON at start, OFF at end (CC#64)
- **Tempo**: 120 BPM (configurable via `--tempo`)

## Design Notes

### Monophonic Prototype

The current implementation is a **monophonic prototype** — it assumes only one note at a time:

1. **Pitch detection**: YINfft runs on each audio frame, returning a MIDI note number (float) and confidence (0.0–1.0).
2. **Onset detection**: Spectral flux detects note onsets (transients).
3. **State machine**: Two states — IDLE (waiting for onset) and PLAYING (note active).
   - **IDLE → PLAYING**: When pitch confidence exceeds threshold AND onset is detected.
   - **PLAYING → IDLE**: When pitch confidence drops below threshold (note-off).
4. **Note duration**: From onset timestamp to confidence-drop timestamp.
5. **Note velocity**: Estimated from RMS energy of the note segment (scaled to 0–127).

### Future Enhancements

- **Polyphony**: Spectral peak tracking + multiple pitch detection for chords.
- **Pedal detection**: Infer sustain pedal from audio analysis (characteristic string damping sound).
- **Velocity refinement**: RMS energy of the full note segment (not just the onset frame).
- **Note trimming**: Remove spurious short notes (< 50ms).

### Key Design Decisions

- **YINfft** is the default pitch detection method (best accuracy/speed tradeoff for piano).
- **Spectral flux** is the default onset detection method (most reliable for piano).
- **480 ticks per quarter note** is the default (Logic Pro compatibility).
- **Type 1 MIDI files** are generated (multi-track, Logic Pro compatible).
- **Pimpl pattern** is used throughout (RAII, encapsulation, swappability).
- **HIR (High-level Instrumentation Representation)** is the single source of truth for both MIDI and future LilyPond output.

## Testing

To test with a recorded AIFF file:

```bash
# Record audio using aiffcapture:
./aiffcapture/build/bin/aiffcapture --duration 60 test.aiff

# Transcribe to MIDI:
./midicapture/build/bin/midicapture test.aiff test.mid

# Verify the MIDI file:
file test.mid
ffprobe test.mid
```

## Cross-References

- [`lode/midicapture/summary.md`](../lode/midicapture/summary.md) — Module overview
- [`lode/libaudio/hir.md`](../lode/libaudio/hir.md) — HIR specification
- [`lode/libaudio/decisions.md`](../lode/libaudio/decisions.md) — Default parameters
- [`lode/MIDI.md`](../MIDI.md) — MIDI file format
- [`aiffcapture/README.md`](../aiffcapture/README.md) — Audio capture utility

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
│   │   └── transcriber.h         # High-level transcription API
│   └── src/
│       ├── main.cpp              # CLI entry point (Boost program_options)
│       └── transcriber.cpp       # Transcription pipeline (pitch + onset)
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

# With custom parameters (silence level drives note gating):
./build/bin/midicapture --window-size 1024 --silence -35 \
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
| `<input.aiff>` *(positional)* | string | (required) | Input audio file (AIFF, WAV, FLAC, etc.). |
| `[output.mid]` *(positional)* | string | derived from input | Output MIDI file (.mid). |
| `--input, -i` | string | — | Input audio file.  ⚠️ Prefer the positional form: a trailing argument after `--input` is captured as the *output* and the intended input is lost (see [Known Limitations](#known-limitations)). |
| `--window-size` | int | 2048 | FFT window size (power of 2). |
| `--hop-size` | int | 512 | Hop size between frames. |
| `--silence` | float | -40 | Silence threshold in dB (note on/off hysteresis). |
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

### Monophonic Pipeline (current)

The current implementation is a **monophonic prototype** — it tracks one note at a time. The pipeline runs a single sequential read of the file, and per hop (512 samples, 10.7 ms):

1. **Energy gate (hysteresis)**: A hop is *tonal* when its RMS is above `--silence`. A note *arms* at the on threshold and *disarms* only after the energy has stayed below `--silence − 10 dB` for 3 consecutive hops. The pitch detector's confidence is **not** used to gate boundaries (YINfft reports a usable fundamental even for noise; on rendered piano it reads ~0).
2. **Onsets**: Spectral flux marks note starts (aubio CLI parity: 0.3 peak, 12 ms min IoI).
3. **Pitch**: The note's *chroma* is a majority vote over all its valid YIN estimates (stable across a note's lifetime); the *octave* is anchored to the loudest hop. Note *changes* require 5 consecutive hops to agree on a new pitch class (pitch hysteresis).
4. **Defragmentation**: A *decaying* note is closed and re-opened hop to hop by its own wobble, so a single physical note arrives as a run of short same-pitch fragments. Two stages undo that: notes shorter than 5 hops are dropped, and consecutive same-pitch fragments closer than a quarter-note gap are merged into one note.
5. **Velocity**: The loudest hop's RMS over the note's lifetime (a single attack-and-decay, not a per-hop tremolo).
6. **MIDI**: The notes are written to a Type 1 file; the tempo comes from `--tempo`.

### Known Limitations

- **Input and output paths are positional; the `--input` flag is awkward.** The input is registered as a *positional-only* name in `desc` (Boost's positional slots cannot carry short flags) and separately re-registered as `--input`/`-i` aliases for the same variable.  A value supplied both via the flag and via the positional slot would be a second use of the same option name, so Boost would reject the documented `--input in.aiff out.mid` form with `option '--input' cannot be specified more than once`.  Even the flag-only form `midicapture --input in.aiff` mis-parses: the positional slot swallows the trailing argument as the *output*, and the intended input is silently dropped.  **Prefer the positional form** (`midicapture in.aiff [out.mid]`, as in the examples above); `--output`/`-o` is safe to use, `--input` is not.  This is a Boost program_options limitation, documented in a comment at the option registration site in `src/main.cpp`.
- **Octave ambiguity on recordings with a weak fundamental.** YIN is a *harmonic* estimator. On the small `timidity` scale renders the piano fundamental is spectrally weak, and YIN locks to a low partial (measured ~91 Hz for a 440 Hz note) far below the true fundamental, so the transcribed octave is unreliable there. On a *recorded* performance with a strong fundamental (the project's main target — `aiffcapture/final-fantasy.aiff`) the defragmented output is musically sensible (~100 notes over 30 s, F#/E/G# content). Robust octave resolution for weak-fundamental sources is a future task (spectral-peak / harmonic-series anchor, or a neural analyzer — see `lode/audio-to-midi.md`).
- **First note of a file** is frequently missed (aubio's first-frame artifact: onset detection needs a spectral *change*, and a file that begins with audio has none on its first frames).
- **Monophonic**: chords are not resolved (YIN tracks one fundamental). Polyphony is a future phase.

### Future Enhancements

- **Polyphony**: per-stem transcription after source separation, or a polyphonic analyzer (basic-pitch / Onsets&Frames).
- **Tempo tracking**: estimate the source tempo (`aubio_tempo`) rather than taking `--tempo`.
- **Pedal detection**: infer sustain pedal from spectral flux / decay patterns.

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
midicsv test.mid
```

# Audio Project — Summary

## Purpose

Build a suite of local-first audio processing utilities targeting macOS (later POSIX), with the overarching goal of creating an **audio file → MIDI file** transcription pipeline. The system runs entirely on the user's machine with no cloud dependencies.

## Scope

| Phase | Status | Description |
|-------|--------|-------------|
| **Audio → AIFF** (capture) | Complete | Capture audio from BlackHole 2ch to 16-bit signed integer AIFF files. |
| **DSP Library** (libaudio) | Designed | DSP library wrapping aubio, libsndfile, rubberband. HIR defined. Not yet implemented. |
| **Audio → MIDI** (transcription) | **In Progress** | Monophonic prototype using aubio (YINfft pitch, spectral flux onsets) → Type 1 MIDI. |
| **Audio → Waterfall** (frequency analysis) | **In Progress** | SONAR-style spectral display: FFT per row, 16-bit quantized, text output. |
| **MIDI → Sheet Music** | Planned | Generate readable sheet music from MIDI data. |
| **Sheet Music → MIDI** | Planned | Generate playable audio from sheet music representations. |

## Architecture

```
Audio/
├── aiffcapture/          # Phase 1: BlackHole → AIFF capture utility
├── libaudio/             # Phase 0: DSP library (designed, not implemented)
├── midicapture/          # Phase 2: Audio → MIDI (in progress)
├── waterfall/            # Phase 3: Audio → frequency waterfall (in progress)
├── lode/                 # Lode coding documentation (project knowledge)
│   ├── summary.md        # This file
│   ├── terminology.md    # Shared glossary
│   ├── practices.md      # Coding style, constraints
│   ├── lode-map.md       # Index of all lode files
│   ├── MIDI.md           # MIDI protocol, General MIDI, SMF format
│   ├── LilyPond.md       # LilyPond notation, MIDI mapping, Logic Pro integration
│   └── libaudio/         # Phase 0: DSP library (designed)
│       ├── summary.md    # Module overview, API design
│       ├── decisions.md  # Library choices, wrapper pattern, defaults
│       └── hir.md        # High-level Instrumentation Representation
│   ├── midicapture/      # Phase 2: transcription module
│   │   └── summary.md    # Module overview, architecture, pipeline
│   └── waterfall/        # Phase 3: frequency analysis module
│       ├── summary.md    # Module overview, architecture, pipeline, CLI
│       └── decisions.md  # Design decisions (single file, auto-scale, hop=window)
├── README.md             # Project overview
├── LICENSE
└── .clang-format         # Shared coding style
└── literate-programming.md # Knuth's philosophy, toolchain, source code for humans
```

## Current State (midicapture)

- **MIDI writer — validated.** The writer emits well-formed Type 1 SMF files
  that round-trip cleanly through **`midicsv`** (structural parse) and
  **`timidity`** (render to WAV). A `--test` flag writes a fixed single note
  (middle C, vel 100, 1 s) as a stable regression target. No practical
  note-count limit. See [midicapture/writer.md](midicapture/writer.md).
  *`ffprobe` is not a reliable validator for small MIDI files — use the above.*
- **Segfault on stereo input — resolved.** The transcription buffer overflow
  (2048-float buffer receiving 4096 interleaved stereo floats per frame)
  is fixed. The program now runs to completion on stereo WAV/AIFF files.
  See [midicapture/summary.md](midicapture/summary.md) §8.
- **Transcription — still WIP (open).** The monophonic pipeline detects only
  ~2 notes from a ~30 s recording. Threshold tuning and state-machine stability
  remain open; this is independent of (now-solved) writer validity.

## Key Decisions

- **Language**: C++20
- **Build system**: CMake (canonical directory structure)
- **Platform**: macOS first (Core Audio), POSIX later
- **Audio format**: AIFF output (16-bit signed integer PCM, 32-bit integer sample rate)
- **Capture method**: BlackHole 2ch virtual audio device (Phase 1)
- **DSP library**: aubio (C++ wrapper), libsndfile (file I/O), rubberband (optional time-stretching)
- **HIR**: High-level Instrumentation Representation — single source of truth for MIDI and LilyPond output
- **DRM handling**: Separate utility to strip DRM from Apple Music content
- **Style**: 3-space indent, Attach braces, 80-column limit, std::cout/cerr, no void* in our code
- **Lode coding**: Structured documentation folder (`lode/`) for cross-session knowledge preservation

## Known Issues

- **BlackHole 2ch applies ~3 dB fixed attenuation.** Increase the BlackHole 2ch volume
  slider in System Settings → Sound → Output. The capture code is verified correct.

## Future Modules (Planned)

- `midisheet/` — MIDI → sheet music generation
- `sheetmidi/` — Sheet music → MIDI file generation
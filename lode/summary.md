# Audio Project — Summary

## Purpose

Build a suite of local-first audio processing utilities targeting macOS (later POSIX), with the overarching goal of creating an **audio file → MIDI file** transcription pipeline. The system runs entirely on the user's machine with no cloud dependencies.

## Scope

| Phase | Status | Description |
|-------|--------|-------------|
| **Audio → AIFF** (capture) | Complete | Capture audio from BlackHole 2ch to 16-bit signed integer AIFF files. |
| **DSP Library** (libaudio) | **Built** | Wraps aubio/libsndfile (Tier-1) + ONNX Runtime/Core ML (Tier-2: basic-pitch). HIR is the analyzer-agnostic seam. |
| **Audio → MIDI** (transcription) | **In Progress** | Analyzer-agnostic behind the `Analyzer` port: Tier-1 `libaudio::Transcriber` (YINfft + defrag) and Tier-2 `libaudio::BasicPitch` (Core ML, resolves the octave — the **default `--model`** in a Tier-2 build; aubio is the fallback). → Type 1 MIDI. |
| **Audio → Waterfall** (frequency analysis) | **Mostly complete** | SONAR-style spectral display (text + MP4 video, PCM and MIDI modes). |
| **MIDI → Sheet Music** | Planned | Generate readable sheet music from MIDI data. |
| **Sheet Music → MIDI** | Planned | Generate playable audio from sheet music representations. |

## Architecture

```
Audio/
├── aiffcapture/          # Phase 1: BlackHole → AIFF capture utility
├── libaudio/             # Phase 0: DSP library (built: Tier-1 aubio + Tier-2 ONNX)
├── midicapture/          # Phase 2: Audio → MIDI (in progress)
├── waterfall/            # Phase 3: Audio → frequency waterfall (in progress)
├── lode/                 # Lode coding documentation (project knowledge)
│   ├── summary.md        # This file
│   ├── terminology.md    # Shared glossary
│   ├── practices.md      # Coding style, constraints
│   ├── lode-map.md       # Index of all lode files
│   ├── MIDI.md           # MIDI protocol, General MIDI, SMF format
│   ├── LilyPond.md       # LilyPond notation, MIDI mapping, Logic Pro integration
│   └── libaudio/         # Phase 0: DSP library (built)
│       ├── summary.md    # Module overview, dependencies, architecture, API
│       ├── decisions.md  # Library choices, wrapper pattern, defaults
│       ├── hir.md        # High-level Instrumentation Representation
│       └── tier2.md      # Tier-2 ONNX: basic-pitch, 14-file corpus, phasing
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
- **Transcription — defragmented (Tier-1), analyzer-agnostic.** Decaying-note
  fragmentation (a dense 30 s passage → 1447 one-hop fragments) is fixed:
  minimum-lifetime + same-pitch merge reduce `final-fantasy.aiff` to ~101
  musically-sensible notes, and the chromatic scale's ascending sequence
  survives. The path is analyzer-agnostic behind the `Analyzer` port.
  **Tier-1 (YINfft):** octave stays open on weak-fundamental recordings (YIN
  locks below the fundamental; see [audio-to-midi.md](audio-to-midi.md) §5) —
  the scale round-trip validates note count / defrag, not absolute pitch.
  **Tier-2 (basic-pitch, Core ML):** resolves the octave — 100% recall, correct
  octave + chroma on the 14-file corpus ([audio-to-midi.md](audio-to-midi.md) §7).
  Writer validity is solved.
- **Default engine (Tier-2):** the `basic` (basic-pitch) engine is the default
  `--model` in a Tier-2 build — it resolves the octave YIN cannot; `--model aubio`
  is the monophonic fallback, and `--analyzer` is a deprecated alias for `--model`.
  A Tier-1 build has only aubio and no `--model` flag. See
  [midicapture/summary.md](midicapture/summary.md) §6.

## Key Decisions

- **Language**: C++20
- **Build system**: CMake (canonical directory structure)
- **Platform**: macOS first (Core Audio), POSIX later
- **Audio format**: AIFF output (16-bit signed integer PCM, 32-bit integer sample rate)
- **Capture method**: BlackHole 2ch virtual audio device (Phase 1)
- **DSP library**: aubio (C++ wrapper), libsndfile (file I/O)
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
# Audio Project — Summary

## Purpose

Build a suite of local-first audio processing utilities targeting macOS (later POSIX), with the overarching goal of creating an **audio file → MIDI file** transcription pipeline. The system runs entirely on the user's machine with no cloud dependencies.

## Scope

| Phase | Status | Description |
|-------|--------|-------------|
| **Audio → AIFF** (capture) | Complete | Capture audio from BlackHole 2ch to 16-bit signed integer AIFF files. |
| **DSP Library** (libaudio) | Designed | DSP library wrapping aubio, libsndfile, rubberband. HIR defined. Not yet implemented. |
| **Audio → MIDI** (transcription) | Planned | DSP + AI inference to convert audio recordings to MIDI. Starts with piano-only content. |
| **MIDI → Sheet Music** | Planned | Generate readable sheet music from MIDI data. |
| **Sheet Music → MIDI** | Planned | Generate playable audio from sheet music representations. |

## Architecture

```
Audio/
├── aiffcapture/          # Phase 1: BlackHole → AIFF capture utility
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
├── README.md             # Project overview
├── LICENSE
└── .clang-format         # Shared coding style (Core Guidelines compliant)
```

## Key Decisions

- **Language**: C++20, Core Guidelines compliant
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

- macOS tools (`afinfo`, `ffprobe`) always try to parse 80-bit extended float for sample
  rate, regardless of COMM chunk size. They will reject valid AIFF files that use 32-bit
  integer sample rate encoding (our format). Use `aiff2wav.sh` to convert for playback.
  The script handles both our format and standard AIFF (Audacity, etc.) automatically.
- QuickTime Player cannot open AIFF files with 32-bit integer sample rate encoding
  (same root cause as above — it always tries to parse 80-bit extended float). Use
  `aiff2wav.sh` to convert to WAV for playback in QuickTime or any other player.
- **BlackHole 2ch applies ~3 dB fixed attenuation.** Increase the BlackHole 2ch volume
  slider in System Settings → Sound → Output. The capture code is verified correct.

## Future Modules (Planned)

- `midicapture/` — Audio → MIDI transcription using DSP + AI inference
- `midisheet/` — MIDI → sheet music generation
- `sheetmidi/` — Sheet music → MIDI file generation

## References

- [Lode Coding](https://fjzeit.github.io/lode) — Structured documentation approach for AI-assisted development
- [Core Guidelines for C++](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- [AIFF Specification](https://www.mpg123.de/api/aiff_8c.html)
- [Core Audio API](https://developer.apple.com/library/archive/documentation/MusicAudio/Reference/CoreAudioAPIRef/)
- [aubio](https://aubio.org) — Audio analysis library (pitch, onsets, beats, notes)
- [libsndfile](http://www.mega-nerd.com/libsndfile/) — Audio file I/O library
- [rubberband](http://breakfastquay.com/rubberband/) — Time-stretching and pitch-shifting library

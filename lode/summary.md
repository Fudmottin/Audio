# Audio Project — Summary

## Purpose

Build a suite of local-first audio processing utilities targeting macOS (later POSIX), with the overarching goal of creating an **audio file → MIDI file** transcription pipeline. The system runs entirely on the user's machine with no cloud dependencies.

## Scope

| Phase | Status | Description |
|-------|--------|-------------|
| **Audio → AIFF** (DRM-free capture) | In progress | Capture audio from BlackHole virtual device to AIFF files. Phase 1: single utility (`aiffcapture`). |
| **Audio → MIDI** (transcription) | Planned | DSP + AI inference to convert audio recordings to MIDI. Starts with piano-only content. |
| **MIDI → Sheet Music** | Planned | Generate readable sheet music from MIDI data. |
| **Sheet Music → MIDI** | Planned | Generate playable audio from sheet music representations. |

## Architecture

```
Audio/
├── aiffcapture/          # Phase 1: BlackHole → AIFF capture utility
├── lode/                 # Lode coding documentation (project knowledge)
├── README.md             # Project overview
├── LICENSE
└── .clang-format         # Shared coding style (Core Guidelines compliant)
```

## Key Decisions

- **Language**: C++20, Core Guidelines compliant
- **Build system**: CMake (canonical directory structure)
- **Platform**: macOS first (Core Audio), POSIX later
- **Audio format**: AIFF output (uncompressed PCM stereo)
- **Capture method**: BlackHole 2ch virtual audio device (Phase 1)
- **DRM handling**: Separate utility to strip DRM from Apple Music content
- **Style**: 3-space indent, Attach braces, 80-column limit, std::cout/cerr, no void* in our code
- **Lode coding**: Structured documentation folder (`lode/`) for cross-session knowledge preservation

## Known Issues

- macOS tools (`afinfo`, `ffprobe`) misread the 80-bit extended float sample rate in AIFF files as a 32-bit integer, reporting garbage values (e.g., 30464 Hz instead of 48000 Hz). The files are valid per the AIFF spec. QuickTime and VLC should play them correctly.

## Future Modules (Planned)

- `midicapture/` — Audio → MIDI transcription using DSP + AI inference
- `midisheet/` — MIDI → sheet music generation
- `sheetmidi/` — Sheet music → MIDI file generation
- `libaudio/` — Shared audio processing library (portable C++20)

## References

- [Lode Coding](https://fjzeit.github.io/lode) — Structured documentation approach for AI-assisted development
- [Core Guidelines for C++](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines)
- [AIFF Specification](https://www.mpg123.de/api/aiff_8c.html)
- [Core Audio API](https://developer.apple.com/library/archive/documentation/MusicAudio/Reference/CoreAudioAPIRef/)

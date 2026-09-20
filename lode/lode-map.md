# Lode Map

## Top-Level Files

| File | Purpose |
|------|---------|
| [summary.md](summary.md) | Project overview, scope, architecture, key decisions |
| [terminology.md](terminology.md) | Shared glossary: audio formats, Core Audio API, AIFF format, Lode coding terms |
| [practices.md](practices.md) | Coding style, project structure, Core Audio patterns, documentation standards |
| [literate-programming.md](../literate-programming.md) | Knuth's philosophy, Web language, toolchain history, source code for humans |
| [MIDI.md](MIDI.md) | MIDI protocol, General MIDI, SMF file format, piano-specific considerations |
| [LilyPond.md](LilyPond.md) | LilyPond notation, MIDI-to-LilyPond mapping, Logic Pro integration |

## Subsystem Files

| Module | Path | Status |
|--------|------|--------|
| aiffcapture | [lode/aiffcapture/](aiffcapture/) | Phase 1 — Complete |
| libaudio | [lode/libaudio/](libaudio/) | Phase 0 — Designed |
| **midicapture** | **[lode/midicapture/](midicapture/)** | **Phase 2 — In Progress** |

## Subsystem: aiffcapture

| Document | Purpose |
|----------|---------|
| [summary.md](aiffcapture/summary.md) | Module overview, CLI interface, known issues |
| [decisions.md](aiffcapture/decisions.md) | Key design decisions and rationale |
| [handoffs.md](aiffcapture/handoffs.md) | Session handoff documents for continuity |

## Subsystem: libaudio

| Document | Purpose |
|----------|---------|
| [summary.md](libaudio/summary.md) | Module overview, dependencies, architecture, module-by-module API design |
| [decisions.md](libaudio/decisions.md) | Library choices (aubio, libsndfile, rubberband), wrapper pattern, default parameters |
| [hir.md](libaudio/hir.md) | High-level Instrumentation Representation (Note, ControlEvent, Score) |

## Subsystem: midicapture

| Document | Purpose |
|----------|---------|
| [summary.md](midicapture/summary.md) | Module overview, architecture, transcription pipeline, CLI, validation, open transcription issue |
| [writer.md](midicapture/writer.md) | SMF writer: byte layout, invariants, `--test` flag, midicsv/timidity validation |
| [tmp/session-handoff-midicapture-diagnosis.md](tmp/session-handoff-midicapture-diagnosis.md) | Session diagnosis: secondsToTicks bug, transcription quality issues |

## Future Modules (Planned)

| Module | Description |
|--------|-------------|
| midisheet | MIDI → sheet music generation |
| sheetmidi | Sheet music → MIDI file generation |

## Cross-Reference

- `.clang-format` — Shared coding style (repository root)
- `README.md` — Project-level documentation (repository root)
- `aiffcapture/` — Phase 1 implementation (repository root)
- `midicapture/` — Phase 2 implementation (in progress)
- `lode/MIDI.md` — MIDI protocol, General MIDI, SMF file format
- `lode/LilyPond.md` — LilyPond notation, MIDI-to-LilyPond mapping
- `lode/libaudio/` — Phase 0 DSP library (designed, not yet implemented)
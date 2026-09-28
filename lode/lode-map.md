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
| [audio-to-midi.md](audio-to-midi.md) | Cross-module: audio vs MIDI, what's recoverable, state of the art (tiers), the octave problem, evaluation, **the decided Tier-2 path (ONNX in libaudio)** |

## Subsystem Files

| Module | Path | Status |
|--------|------|--------|
| aiffcapture | [lode/aiffcapture/](aiffcapture/) | Phase 1 — Complete |
| libaudio | [lode/libaudio/](libaudio/) | Tier-1 aubio + Tier-2 ONNX (basic-pitch, Core ML) built + verified (see audio-to-midi.md §7) |
| **midicapture** | **[lode/midicapture/](midicapture/)** | **Phase 2 — In Progress** |
| **waterfall** | **[lode/waterfall/](waterfall/)** | **Phase 3 — Mostly complete** |
| **audio-to-midi** | **[audio-to-midi.md](audio-to-midi.md)** | **Cross-module reference** |

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
| [decisions.md](libaudio/decisions.md) | Library choices (aubio, libsndfile), wrapper pattern, default parameters |
| [hir.md](libaudio/hir.md) | High-level Instrumentation Representation (Note, ControlEvent, Score) |

## Subsystem: waterfall

| Document | Purpose |
|----------|---------|
| [summary.md](waterfall/summary.md) | Module overview, architecture, column grid, pipeline, quantization, CLI, verification |
| [decisions.md](waterfall/decisions.md) | Design decisions (single file, libaudio I/O, auto-scale, hop=window, README reconciliation) |

## Subsystem: midicapture

| Document | Purpose |
|----------|---------|
| [summary.md](midicapture/summary.md) | Module overview, architecture, transcription pipeline, CLI, validation, defragmentation, open octave limitation |
| [writer.md](midicapture/writer.md) | SMF writer: byte layout, invariants, `--test` flag, midicsv/timidity validation |
| [testmidi.md](midicapture/testmidi.md) | `--generate-test-midi-files`: monophonic scale ground-truth files, `--output-dir` |
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
- `lode/libaudio/` — DSP library: Tier-1 aubio + Tier-2 ONNX (basic-pitch, Core ML)

## Maintenance

- **Lode 250-line soft cap — 2 files over; split deferred to a future session.**
  `lode/audio-to-midi.md` (275) and `lode/midicapture/summary.md` (293) exceed the soft
  cap, but their content is correct and current — only over-length. The intended fix: split
  `audio-to-midi.md` §7 (Tier-2: ONNX foundation + basic-pitch + corpus) into a focused
  `lode/tier2.md` and slim the midicapture summary. Deferred by explicit decision; treat as
  a maintenance task, not a correctness bug.
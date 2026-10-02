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
| [audio-to-midi.md](audio-to-midi.md) | Cross-module: audio vs MIDI, what's recoverable, state of the art (tiers), the octave problem, evaluation (Tier-2 details now in [libaudio/tier2.md](libaudio/tier2.md)) |

## Subsystem Files

| Module | Path | Status |
|--------|------|--------|
| aiffcapture | [lode/aiffcapture/](aiffcapture/) | Phase 1 — Complete |
| libaudio | [lode/libaudio/](libaudio/) | Tier-1 aubio + Tier-2 ONNX (basic-pitch, Core ML) built + verified (Tier-2 details in [libaudio/tier2.md](libaudio/tier2.md)) |
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
| [tier2.md](libaudio/tier2.md) | Tier-2 ONNX transcription: basic-pitch I/O contract, 14-file corpus, phasing (the decided neural path) |

## Subsystem: waterfall

| Document | Purpose |
|----------|---------|
| [summary.md](waterfall/summary.md) | Module overview, architecture, column grid, pipeline, quantization, CLI, verification |
| [decisions.md](waterfall/decisions.md) | Design decisions (single file, libaudio I/O, auto-scale, hop=window, README reconciliation) |

## Subsystem: midicapture

| Document | Purpose |
|----------|---------|
| [summary.md](midicapture/summary.md) | Module overview, architecture, transcription pipeline, CLI, validation toolchain (defrag + octave details in [defrag.md](midicapture/defrag.md)) |
| [defrag.md](midicapture/defrag.md) | Tier-1 note modeling: defragmentation, the YIN octave problem, the stereo segfault (measured) |
| [writer.md](midicapture/writer.md) | SMF writer: byte layout, invariants, `--test` flag, midicsv/timidity validation |
| [testmidi.md](midicapture/testmidi.md) | `--generate-test-midi-files`: monophonic scale ground-truth files, `--output-dir` |
| [tmp/session-handoff-midicapture-diagnosis.md](tmp/session-handoff-midicapture-diagnosis.md) | Session diagnosis: secondsToTicks bug, transcription quality issues |

## Plans

| Plan | Purpose |
|------|---------|
| [plans/basic-pitch-tier1.md](plans/basic-pitch-tier1.md) | Promote basic-pitch — midicapture-side **done** (default `--model` in Tier-2); libaudio tier-flattening **open** |
| [plans/ffmpeg-link.md](plans/ffmpeg-link.md) | In-process decode+resample via **linked FFmpeg libraries** (replaces the basic-pitch ffmpeg shell-out) — ✅ **implemented** |
| [plans/ffmpeg-in-memory.md](plans/ffmpeg-in-memory.md) | In-process (miniaua) decode+resample — **superseded** by [ffmpeg-link.md](plans/ffmpeg-link.md) |
| [plans/transcriber-merge.md](plans/transcriber-merge.md) | The prior Transcriber→Analyzer unification (done) |
| [plans/postproc-tuning.md](plans/postproc-tuning.md) | Tunable basic-pitch (NMP) post-processing: knob promotion, clamps, Boost flags, raw-map dump, MAESTRO GT — **in progress** (Phases 1–2 done: raw-map dump; knob promotion + clamps + Boost flags) |

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

- **Lode 250-line soft cap — 4 files over.** `lode/MIDI.md` (525), `lode/LilyPond.md`
  (894), `lode/libaudio/summary.md` (1114), and `lode/libaudio/decisions.md` (327) exceed
  the cap. The Tier-2 split brought `audio-to-midi.md` and `midicapture/summary.md`
  under 250 (their over-length content moved to `libaudio/tier2.md` and
  `midicapture/defrag.md`); the ffmpeg-link session's CLI container/flag notes then
  nudged `midicapture/summary.md` slightly back over (259 — low-priority split
  candidate, its §6 CLI reference is the natural split point).
  **Tracked follow-up (deferred):** split the 1114-line `libaudio/summary.md` (module
  overview + per-module API design) into focused sub-files. The other over-cap files are
  large reference docs; treat them as lower-priority split candidates.
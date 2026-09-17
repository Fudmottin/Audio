# Lode Map

## Top-Level Files

| File | Purpose |
|------|---------|
| [summary.md](summary.md) | Project overview, scope, architecture, key decisions |
| [terminology.md](terminology.md) | Shared glossary: audio formats, Core Audio API, AIFF format, Lode coding terms |
| [practices.md](practices.md) | Coding style, project structure, Core Audio patterns, documentation standards |

## Subsystem Files

| Module | Path | Status |
|--------|------|--------|
| aiffcapture | [lode/aiffcapture/](aiffcapture/) | Phase 1 — In progress |

## Subsystem: aiffcapture

| Document | Purpose |
|----------|---------|
| [summary.md](aiffcapture/summary.md) | Module overview, CLI interface, known issues |
| [decisions.md](aiffcapture/decisions.md) | Key design decisions and rationale |
| [handoffs.md](aiffcapture/handoffs.md) | Session handoff documents for continuity |

## Future Modules (Planned)

| Module | Description |
|--------|-------------|
| midicapture | Audio → MIDI transcription (DSP + AI inference) |
| midisheet | MIDI → sheet music generation |
| sheetmidi | Sheet music → MIDI file generation |
| libaudio | Shared portable audio processing library |

## Cross-Reference

- `.clang-format` — Shared coding style (repository root)
- `README.md` — Project-level documentation (repository root)
- `aiffcapture/` — Phase 1 implementation (repository root)

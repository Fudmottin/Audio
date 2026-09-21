# midicapture — `--generate-test-midi-files`

> A generator-mode that writes a small suite of simple, monophonic piano
> MIDI files into the current working directory (or `--output-dir`). Used to
> produce audio (via timidity/timidity-compatible rendering) that exercises the
> audio→MIDI transcription pipeline end-to-end. Loded alongside
> [summary.md](summary.md); the writer it relies on is documented in
> [writer.md](writer.md); format background in [../MIDI.md](../MIDI.md).

---

## 1. Role & Responsibility

`midicapture --generate-test-midi-files` is a **generator** that **ignores all
other options** (input, output, window-size, hop-size, confidence, silence,
tempo, method). It builds a fixed set of `Score`s (HIR) and writes each to a
`.mid` file. No audio is opened, no analysis is run. It takes precedence over
`--test` if both are present.

Purpose of the generated files:

- They are the **input** for producing reference audio that `midicapture`
  (the real transcription path) can try to recreate as MIDI.
- They are intentionally **simple** — monophonic, one note at a time, uniform
  velocity — so the transcription result is easy to analyze and diff against
  the known ground truth.
- They double as **writer regression targets** like `--test` do, but with
  *more* than one note, proving the writer handles multi-note scores (not just
  a single note).

Perfect reproduction from audio→MIDI is **not** expected at this stage; the
value is a clean, known ground truth to measure progress against.

---

## 2. The Set of Files

Six files, one per pattern. All **monophonic** (one note at a time), Acoustic
Grand (program 0, channel 0), uniform **velocity 100** (no dynamics), sustain
off. The set mixes **three note durations** (whole / half / quarter) and
**three tempos** (60 / 90 / 120 BPM), so the rendered audio exercises a spread
of timing shapes the transcription must resolve.

Each performance is "around five seconds" — a rough guideline, not a hard
target. Rendered durations (incl. timidity's natural note-decay tail) land in
the 4–8 s neighborhood.

| File | Pattern (MIDI pitches) | Per-note duration | Beat length | Tempo | Notes |
|------|------------------------|-------------------|-------------|-------|-------|
| `scale-major-ascending-whole-notes-60bpm.mid` | C4 E4 G4 C5 (60 64 67 72) | whole | 2 | 60 | 4 |
| `scale-major-ascending-half-notes-90bpm.mid`  | C4 E4 G4 C5 (60 64 67 72) | half | 1 | 90 | 4 |
| `scale-major-descending-whole-notes-60bpm.mid`| C5 G4 E4 C4 (72 67 64 60) | whole | 2 | 60 | 4 |
| `scale-major-descending-half-notes-90bpm.mid` | C5 G4 E4 C4 (72 67 64 60) | half | 1 | 90 | 4 |
| `scale-chromatic-ascending-quarter-notes-120bpm.mid` | C4→B4 (60…71, 12) | quarter | ½ | 120 | 12 |
| `scale-minor-ascending-whole-notes-60bpm.mid` | A4 C5 E5 A5 (69 72 76 81) | whole | 2 | 60 | 4 |

Notes use **MIDI numbers**: C4=60, C5=72, E4=64, E5=76, G4=67, A4=69. A
single beat of rest follows the final note.

---

## 3. How Durations Map to Seconds

MIDI timing is tempo-relative. The writer converts **seconds → ticks** using
`480 ticks/qn × (bpm/60)` (see [writer.md](writer.md) §3), and the *duration*
of a note in beats is set by the gap between its `startTime` and `endTime` in
**seconds**:

```
beatSeconds = 60.0 / tempoBpm          // one quarter-note in real seconds
noteDurationSec = noteBeats * beatSeconds
```

So a "whole note" (2 beats) at 60 BPM is `2 × 1.0 = 2.0 s`; at 120 BPM it is
`2 × 0.5 = 1.0 s`. The generator fills in absolute seconds computed from each
file's tempo, so the *same* pattern renders faster/slower purely by tempo —
which is exactly the timing variety the transcription needs to see.

Notes are **non-overlapping**: each note's `startTime` equals the previous
note's `endTime` (pure monophony), so the whole pattern length is
`N × noteBeats` beats. (The writer emits each note's Note Off at its `endTime`
with a delta of 0 before the next Note On — see [writer.md](writer.md) §3, the
`lastTick` invariant.)

---

## 4. CLI Contract

```
midicapture --generate-test-midi-files [--output-dir <dir>]
```

| Option | Default | Effect |
|--------|---------|--------|
| `--output-dir <dir>` | `.` (CWD) | Folder the `.mid` files are written to. Created (including parents) if missing. |

- All other options are **parsed but ignored** in this mode (no error).
- If `--output-dir` does not exist it is created (mkdir -p semantics).
- On success the program prints one line per file written, plus a summary, and
  exits 0.
- On failure to write any file it reports which and exits 1.

`--help` lists the new option in both the option list and the POSIX-style
help block.

---

## 5. Output Layout

```
<output-dir>/
├── scale-major-ascending-whole-notes-60bpm.mid
├── scale-major-ascending-half-notes-90bpm.mid
├── scale-major-descending-whole-notes-60bpm.mid
├── scale-major-descending-half-notes-90bpm.mid
├── scale-chromatic-ascending-quarter-notes-120bpm.mid
└── scale-minor-ascending-whole-notes-60bpm.mid
```

---

## 6. Relationship to `--test`

| | `--test` | `--generate-test-midi-files` |
|---|----------|------------------------------|
| Notes | 1 (middle C) | 4–12 (scales/arpeggios) |
| Purpose | Minimal writer round-trip target | Transcription ground truth + multi-note writer check |
| Input audio | ignored | ignored |
| Output | single file | several files |
| Analysis run | no | no |

Both share the same "renderer-mode" idea: build a `Score`, call
`MidiFileWriter::write`. `--test` is the *minimal* probe; the generator is the
*practical* one.

---

## 7. Validation

Each generated file should parse cleanly with the same toolchain used for
`--test` (see [writer.md](writer.md) §7):

```bash
midicsv <file>.mid                 # structural parse
timidity -Ow <file>.mid <file>.wav # render; expect "Notes lost totally: 0"
```

---

## 8. Cross-References

- [summary.md](summary.md) — module overview, CLI, pipeline
- [writer.md](writer.md) — the SMF renderer this mode drives
- [../MIDI.md](../MIDI.md) — SMF format, varlen, note/tick encoding
- [../libaudio/hir.md](../libaudio/hir.md) — `Score` / `Note` / `ControlEvent`

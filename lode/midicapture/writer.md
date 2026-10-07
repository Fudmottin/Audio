# midicapture — MIDI File Writer

> The SMF renderer in `libaudio` that turns a HIR `Score` into a binary
> Standard MIDI File. Loded alongside [summary.md](summary.md); general format
> background lives in [../MIDI.md](../MIDI.md).

---

## 1. Role & Responsibility

`MidiFileWriter` (`libaudio/include/libaudio/midiFileWriter.h`,
`libaudio/src/midiFileWriter.cpp`) is a **renderer only**. It takes a `Score`
and emits a well-formed **Type 1 SMF** (480 ticks/qn). It does not analyze
audio. Its contract:

> For **any** `Score` — even zero notes — `write()` produces a file that
> `midicsv` and `timidity` can parse and that Logic Pro can import. Validity
> never depends on transcription quality. There is no practical note-count
> limit.

---

## 2. What the Writer Emits (byte layout)

Header chunk (14 bytes):

```
MThd 00 00 00 06 | 00 01 | 00 01 | 01 E0
        ^len = 6     ^fmt 1   ^1 trk   ^480 tpq   (all big-endian)
```

> The 480 ticks-per-quarter field is the 2-byte big-endian `01 E0`. Its **low
> byte (`E0`) matches the pitch-bend status byte** but is *data* inside the
> header (never parsed as a track). Any byte-scan for bends must treat `E0` as
> a bend only in a *status* position — a raw `E0` byte is not by itself a bend.

Track chunk = `MTrk` + 4-byte big-endian length + event bytes. The declared
length must **exactly equal** the number of event bytes that follow.

Event order for a `Score` at tempo *T* (see [../MIDI.md](../MIDI.md) §5 for
varlen/delta encoding):

```mermaid
sequenceDiagram
    participant W as Writer (buildTrack)
    W->>W: SetTempo (FF 51) @ tick 0
    loop each distinct channel used (sorted)
        W->>W: Program 0 = Acoustic Grand (C-ch) @ tick 0   (unconditional)
        W->>W: Sustain ON (B-ch 40 7F) @ tick 0   (only if the score sustains)
    end
    loop each channel event, stable-sorted by absolute tick
        W->>W: NoteOn  (9-ch nn vv) @ startTick   (per note, in note order)
        W->>W: PitchBend (E-ch nn vv) @ gridTick  (bends, linspace across the note)
        W->>W: NoteOff (8-ch nn vv) @ endTick     (>= startTick + 1)
        W->>W: ControlChange @ tick               (Score controls, channel 0)
    end
    loop each distinct channel used (sorted)
        W->>W: Sustain OFF (B-ch 40 00) @ tick 0   (only if the score sustains)
    end
    W->>W: EndOfTrack (FF 2F 00) @ tick 0
```

> The note events and the `Score` control events are **merged into one stream**
> and stably sorted by absolute tick, then emitted in order. A control at time
> *T* therefore lands at its true position among the notes — a t=0 sustain pedal
> engages at t=0 and sustains the notes that follow — instead of being clamped
> to the end of the track (the historical writer appended every control after
> the last note, where an early one sustained nothing). At equal ticks the
> stable sort keeps the historical per-note order, so a control-less `Score`
> (the monophonic corpus cases, the `--test` sanity note) stays
> byte-identical to the old writer.

### Per-channel scaffolding

The writer computes the **set of distinct channels** from the notes (sorted).
At t=0 it emits one `programChange(0, ch)` per channel — **unconditional**
scaffolding — and, only when `emitCoarsePedal` holds (a `Note.sustain` note
is present *and* no explicit CC#64 control is), one `controlChange(64, 127,
ch)` per channel; symmetrically it emits one `controlChange(64, 0, ch)`
before the EOT per channel, again only when `emitCoarsePedal`. A score that
neither sustains nor carries a CC#64 control writes **no pedal at all**, so
the rendered audio reflects the real performance. **Byte-identity invariant:**
when only channel 0 is used (the aubio Tier-1 path, the test corpus, and the
`--test` sanity note) and no note sustains, the output is exactly the
single-channel writer — one program, no pedal. Additional channels appear
only when the basic-pitch channel policy routes bent notes to them (feature
3).

Every event is preceded by a **variable-length delta-time** that is always
**≥ 0**; the absolute timeline is **monotonic** — a non-increasing tick yields
a delta of 0, never negative.

### Pitch bends (0xE0)

A `Note` with a **non-empty** `pitchBends` vector (filled by the Tier-2
basic-pitch `contour` decode; always empty for the aubio path) emits each value
as a pitch-bend message on a `linspace(start, end, n)` grid — the reference's
`np.linspace` — placed between its NoteOn and NoteOff. The payload is
`{E0|ch, s&0x7F, (s>>7)&0x7F}` with `s = value + 8192` (low byte first; value 0
→ `E0 00 40`; value −8192 → `E0 00 00`). A note with an **empty** vector emits
none.

The running timeline (`lastTick`) advances after the NoteOn *and* after each
bend (not just the NoteOff); each subsequent event's delta is against that
running tick, so the absolute timeline stays monotonic even with bends.

**Round-trip.** The matching `MidiFileReader`
(`libaudio/src/midiFileReader.cpp`) keys open notes by `(channel<<8)|pitch`,
decodes each `0xE0` to signed 14-bit (`((d2&0x7F)<<7 | (d1&0x7F)) − 8192`), and
attaches it to the most-recently-opened note on that channel, so a written
`Note::pitchBends` comes back intact.

**A standards-compliant reader.** The reader is a *general* SMF parser, not one
sized to this writer: it honours running status (a data byte reuses the last
*channel* message, exactly as the MIDI 1.0 spec and the `midicsv` reference
define it — a system message does not clear a channel running status), runs a
per-segment clock that honours tempo changes (`FF 51` sets the tempo for later
deltas), and **fails loudly** on malformed input — `ok()` is false and
`error()` names the reason (a non-SMF, a short / oversized header, an
unsupported ticks-per-division, an `MTrk` length past end-of-file, an
unterminated varlen, event data past the track end, a data byte with no prior
channel status, or a reserved / realtime status). Ground-truth callers abort on
`!ok()` rather than score a half-parsed file. This replaced a parser whose
`status = raw[j++]` unconditionally consumed a byte, so a running data byte
desynced the stream (a tick explosion + note loss — the reader bug behind the
earlier MAESTRO sweep's F ≈ 0.2).

---

## 3. Invariants the Writer Enforces

| # | Invariant | How it's enforced |
|---|-----------|-------------------|
| 1 | One `NoteOn` + one `NoteOff` per note | `buildTrack` emits exactly one on, then one off, per `Note`. |
| 2 | Non-negative varlen delta-time before every event | `emit()` calls `appendVarLen(delta)`; deltas clamped to `≥ 0`. |
| 3 | Big-endian multi-byte fields | `writeUint16` / `writeUint32` emit MSB first. |
| 4 | `MTrk` length == actual event bytes | Length is written from `track.size()` *after* building the track. |
| 5 | `FF 2F 00` always present, always last | `buildTrack` appends `endOfTrack()` at the end. |
| 6 | Canonical running status | A channel-voice event matching the last channel status omits its status byte; a system event always carries its full status and does not clear the channel running status. (A `Score` with no two consecutive same-status channel events — e.g. any bend-less file — is byte-identical to a non-running writer.) |
| 7 | Velocities in [1,127]; pitches in [0,127] | `clamp7` (pitch) + `min(127, max(1, v))` (velocity floor/cap). |
| 8 | Note duration ≥ 1 tick | If `offTick <= onTick`, off is bumped to `onTick + 1`. |
| 9 | Bends emit 0xE0 only when the vector is non-empty | `buildTrack` skips the bend block for an empty `Note::pitchBends`; a constant-pitch note (and the entire aubio Tier-1 path) stays byte-identical to the pre-bend writer. |
| 10 | Bend 0xE0 values land between the note's on and off, on a linspace grid | Each bend's tick is `secondsToTicks(start + (end−start)·i/(n−1))`; `lastTick` advances after each bend, keeping the timeline monotonic. |
| 11 | Scaffolding: program unconditional, sustain pedal conditional | A `programChange(0, ch)` is emitted per used channel unconditionally; a sustain-pedal on (t=0) and off (pre-EOT) are emitted per channel **only when** `emitCoarsePedal` (a `Note.sustain` note present, no explicit CC#64). A non-sustained, bend-less, single-channel Score is therefore byte-identical to the pre-bend writer. Additional channels appear only when `Note::channel > 0`. |
| 12 | Notes and controls are emitted in non-decreasing absolute-time order | `buildTrack` merges the per-note events (in note order) and the `Score` controls into one stream and stable-sorts by tick; equal ticks keep insertion order (notes before controls), so a control at time *T* lands at its true position among the notes rather than after the last note. |

### Seconds → ticks

```cpp
uint32_t secondsToTicks(double s, double bpm) {
    return s <= 0 ? 0
                   : static_cast<uint32_t>(
                       std::llround(s * 480 * (bpm / 60.0)));
}
```

With 480 tpq @ 120 BPM → `s × 960`. **Do not divide by 60 twice.**

---

## 4. Concrete Example (the `--test` sanity note)

`midicapture --test <in>` writes a fixed, input-independent note:
**middle C (C4, MIDI 60), velocity 100, 0→1 s** at 120 BPM. Track stream:

```
00 FF 51 03 07 A1 20   SetTempo 500000 µs/qn = 120 BPM
00 C0 00               Program 0 (Acoustic Grand)
00 90 3C 64            C4 NoteOn vel 100   (delta 0)
87 C0 80 3C 64         C4 NoteOff          (delta 960 ticks = 1 s)
00 FF 2F 00            EndOfTrack
```

= 23 event bytes → 14 (header) + 8 (`MTrk` + len) + 23 = **45 bytes**.
The sanity note is non-sustained, so the writer emits **no sustain pedal** —
a forced pedal would add a reverb-like tail the note never contained.

`midicsv` of the file parses cleanly:

```
Header 1,1,480  Tempo 500000  Program_c 0
Note_on 0,60,100  (tick 960) Note_off 0,60,100  End_track
```

`timidity -Ow out.wav <file>` renders a ~1 s note with `Notes lost totally: 0`
(audible, correctly timed, nothing dropped).

---

## 5. The `--test` Flag

`midicapture --test <in>` (also `-t`) ignores the input audio, runs no
analysis, and writes the sanity note above. Output defaults to `<input>.mid`
(or `midicapture-test.mid` if no input is given). It is the stable round-trip
target for validating the writer + `midicsv`/`timidity` **independently of the
still-WIP transcription pipeline**.

```cpp
if (testMode) {                      // in midicapture/src/main.cpp
    Score score = makeSanityScore(tempoBpm);   // C4 / vel 100 / 1 s
    MidiFileWriter w(outputPath);
    if (w.write(score)) { /* print bytes + validation hint */ return 0; }
    return 1;
}
```

---

## 6. File I/O & Failure Handling (RAII)

`Impl` owns a `FILE*`. `write()` opens the file, writes header + track, then
**`fclose` on success** (flushes the stdio buffer to disk) and **nulls the
handle**; the destructor closes it only on failure paths. This
`fclose`-then-null pattern is load-bearing — without it the buffer never
flushed and unit tests saw truncated files.

```cpp
if (fclose(f) != 0) return false;
impl_->file = nullptr;   // destructor must not double-close
```

---

## 7. Validation Toolchain (what "correct" means)

Project decision: **do not gate on `ffprobe`** — it is unreliable for small
MIDI files (reports "Invalid data" on valid ones). Validate with:

- **`midicsv <file>.mid`** — parses to CSV; a clean parse = structurally valid.
- **`timidity -Ow out.wav <file>.mid`** — renders audio; `Notes lost totally: 0`
  plus a correctly-timed note = semantically valid.

The unit test `test_midiWriter` (`libaudio/tests/test_main.cpp`) asserts the
raw bytes: header magic / length / format / tracks / division, `MTrk` length ==
remaining bytes, **first event is `00 FF 51`** (regression guard against the
historical garbage-prefix bug), note bytes present, EOT trailer, and
empty-score structural validity. `test_runningStatus` proves the writer emits
canonical running status for a bent note (one full `0xE0`, the rest data-only)
and the reader recovers a hand-crafted running-status *chord* the writer never
emits; `test_malformedAbort` proves the reader fails loudly on a non-SMF, an
over-long `MTrk`, and a bare data byte with no prior channel status.
`test_midiSustainPedal` pins the pedal-conditioning contract with relative
byte-count + presence assertions: a sustained note adds a whole-performance
on/off bracket (8 bytes), an explicit CC#64 control is written as-is with the
redundant bracket suppressed (4 bytes smaller than the sustained case), and a
non-sustained score emits no pedal at all.

---

## 8. Cross-References

- [summary.md](summary.md) — module overview, pipeline, CLI, open transcription issue
- [../MIDI.md](../MIDI.md) — SMF format, varlen, events, piano specifics
- [../libaudio/hir.md](../libaudio/hir.md) — `Score` / `Note` / `ControlEvent` (writer input)
- [../libaudio/summary.md](../libaudio/summary.md) — the library module that hosts the writer
- [../tmp/session-handoff-midicapture-diagnosis.md](../tmp/session-handoff-midicapture-diagnosis.md) — historical diagnosis (superseded)

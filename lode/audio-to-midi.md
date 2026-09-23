# Turning Audio into MIDI — State of the Art

> A cross-module reference (midicapture / waterfall / libaudio) on the audio→MIDI
> problem: why audio and MIDI are different things, what we can and cannot
> recover per note, where the field stands, and what it means for our
> architecture. Written 2026-09-23 after the midicapture defragmentation work.

---

## 1. Audio and MIDI are different things

- **Audio** is a *physical signal*: a stream of pressure samples (here 48,000/s)
  describing the whole sound — every harmonic, the attack, the room, the
  player's touch. One instrument, one render.
- **MIDI** is a *control score*: a list of events (~10 numbers per note —
  pitch, onset time, offset/velocity, channel, and a few control changes).
  It describes *what to play*, not *what was heard*. It is instrument- and
  render-time dependent: the same MIDI file produces different audio on a
  piano than on a synth, and the same audio can be transcribed into many
  valid MIDI files (different voicings, octaves, velocity interpretations).

The mapping is **many-to-many in both directions**:

```
   same MIDI  ──(different synth/render)──▶  many different audios
   same audio ──(many valid transcriptions)──▶  many different MIDIs
```

So "a MIDI file that sounds like the input" is a **render-time property**, not a
transcription-fidelity property. A transcription is judged by how well its
*control events* (note set, timing, dynamics) describe the performance — and
whether, once re-synthesized, the result *reads as the same performance* to an
ear. For a piano piece whose timbre is supplied by the synth (Logic, timidity),
"indistinguishable to casual listening" is a real, attainable success criterion.

This is the ground the parallel `waterfall` work shifted: waterfall treats audio
as *spectral data to display/verify*; midicapture treats it as *a performance to
recover as control events*. They are two different reads of the same signal.

---

## 2. What we can and cannot recover per note

| Recoverable (from audio) | Not recoverable |
|---|---|
| Onset time (transient / spectral-flux edge) | Exact instrument / sample library |
| Offset / decay envelope | Voicing (which octave a note was played in — see §1) |
| Fundamental pitch, ±cents (for monophonic) | The player's exact touch / articulation |
| Harmonic spectrum / "brilliance" (timbre) | Room / reverb (unless separated from dry) |
| Dynamics (RMS envelope → velocity) | Other instruments (without separation) |
| Pedal (spectral-flux / decay-pattern inference) | |

The **one irreducible ambiguity for monophonic pitch is the octave**: a
frequency of 880 Hz and 440 Hz are the same *pitch class* (A) and, after one
octave of harmonics, almost the same spectrum. Monophonic pitch estimators
resolve the pitch class robustly but the octave only as well as their
*harmonic* model matches the actual spectrum — and that fails on sources whose
fundamental is weak relative to its harmonics (a major theme in §5 below).

---

## 3. State of the art, in tiers

**Tier 1 — monophonic DSP (classic, local, no training).**
A pitch estimator (YIN / YINfft / McDF / F0) plus an onset detector (spectral
flux / energy), then a state machine to segment notes. This is **where
midicapture is**. It is fast, local, dependency-light, and strong for a single
melody or a piano line with a clear fundamental. It **fails on chords** (a single
fundamental is tracked — usually the lowest or the loudest note) and on
**weak-fundamental** sources (YIN can lock to a sub-octave partial; see §5).

**Tier 2 — separate, then transcribe per source.**
Source separation (**Spleeter / Demucs**) splits a mix into per-instrument
stems (vocals, drums, bass, other — or, for a piano-only recording, the *whole*
mix *is* the piano stem). Transcribe each stem independently. This is the
practical route for most *song* recordings where a melody overlaps an
accompaniment: the separation isolates the line midicapture's monophonic machine
can handle. It also fixes the "loudest vs. lowest" ambiguity — each stem is
transcribed on its own. Cost: a trained model (or a C++ port) and a heavier
dependency.

  - **basic-pitch** (Spotify, 2022): a small neural network for *polyphonic*
    piano, directly MIDI-out. The practical modern baseline and the most likely
    "swap the analyzer" upgrade for midicapture when a local runtime is
    acceptable.
  - **NNoteS** (2021): neural, per-note; relevant for monophonic robustness.

**Tier 3 — large / foundation models.**
Whole-piece, polyphonic, any-instrument transcription (Onsets&Frames / TF-MAGS
academic standard; AudioLDM / MusicLM lineage; ACE-Transcriber). Highest
quality, but a **cloud/GPU class of tool** — at odds with the project's
local-first, dependency-light constraint. Not on the near-term path.

---

## 4. What this means for our architecture

The **HIR (`libaudio::Score`) is the right seam**: it is the instrument- and
renderer-independent middle layer that every analyzer can feed and every output
(MIDI, LilyPond) can read. The path to "more interesting MIDI":

1. **Fix monophonic** — *done* (defragmentation, §5). Single-line repertoire.
2. **Stem separation + per-stem monophonic** — covers most song recordings
   (melody + accompaniment), reusing the fixed Tier-1 machine per stem.
3. **Swap the analyzer for basic-pitch** (Tier 2) where a local runtime fits —
   for genuine polyphony / chords that no amount of monophonic tuning resolves.

**Waterfall's role is verification, not boundary detection.** With peak
normalization every row shows every note of a chord at ≥25% and a fundamental's
*harmonic* can outrank the fundamental, so row-energy cannot segment notes (a
chord has no spectral *edge*; only attacks do). Boundary/attack detection must
come from a **time-domain or spectral-*change*** signal (the onset detector);
waterfall is for *display and human verification* (draw detected notes as
columns and check they light the right bands).

---

## 5. The weak-fundamental / octave problem (measured)

The single most important finding from the 2026-09-23 round-trip work:

- On the small `timidity` scale renders, a decaying note's YIN estimates sit
  **1–2 octaves *below* the true fundamental** — e.g. **~91 Hz for an A4 (440
  Hz)** note, and the *loudest* hop is also ~90 Hz. Every estimate is below the
  sub-octave (220 Hz); **zero** estimates are near 440.
- **The 2048-sample window is *not* the cause.** A direct spectral read of the
  same note shows the 440 Hz fundamental is the **dominant** partial (~11× the
  220 Hz sub-octave, ~5× the 880 Hz harmonic). 440 Hz needs only ~4 cycles to
  resolve, and the window holds ~11. The fundamental is *present and strong*;
  YIN simply does not report it.
- The same low-estimate is returned by **all** aubio methods (yin, yinfast,
  mcdf, f0) and is **unchanged by window size** (2048 → 16384).

**Interpretation:** YIN is a *harmonic* estimator — it finds the best single
period that explains the spectrum. On a source whose spectrum peaks at a
*harmonic* rather than the fundamental, it reports that harmonic (or a
lower component) and, when the true fundamental is the actual *lowest* strong
partial, can still land on a sub-harmonic. It is **not** the "sub-octave error"
that a bigger window fixes; it is a model mismatch with the actual spectrum.

**Consequence for midicapture:** on these renders the *chroma* is correct and
defragmentation now gives a plausible note count, but the *octave* is not
trustworthy, so the small scale *corpus is not a clean round-trip target*. On a
**recorded** performance with a strong fundamental (the project's real target,
`aiffcapture/final-fantasy.aiff`) the defragmented output is musically sensible.

**Robust octave fix (future):** resolve the octave from the *dominant spectral
peak* and/or a *harmonic-series* fit (pick the f₀ whose k·f₀ multiples best match
the spectrum's peaks), using YIN's estimate only as a prior. Or move to a Tier-2
analyzer (basic-pitch) that models harmonics explicitly. This is the next
real "more interesting MIDI" step after monophonic is settled.

---

## 6. Evaluation methodology

- **Ground-truth round trip** is the core metric: `MIDI → timidity → audio →
  midicapture → compare note sets` (pitch recall/precision, onset offset vs. the
  score). The 6-scale corpus (`--generate-test-midi-files`) is the fixture.
  *Caveat (from §5):* the round trip is only *clean* for sources with a strong
  fundamental; weak-fundamental renders confound the octave, so they validate
  **defragmentation and note count**, not absolute pitch.
- **Perceptual A/B**: render the transcribed MIDI and the original side by side;
  "indistinguishable to casual listening" is the bar for piano.
- **Structure check**: does the *sequence* of notes survive? (The chromatic
  scale's ascending pitch sequence is preserved after defrag even when the
  absolute octave is off — a useful intermediate signal.)

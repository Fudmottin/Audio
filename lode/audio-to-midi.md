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
    acceptable. → **This is the decided path:** host it (and later TF-MAGS /
    Demucs) inside libaudio on ONNX Runtime. See §7.
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
   The *how* — hosting these models in libaudio on ONNX Runtime — is decided;
   see §7.

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

---

## 7. The decided Tier-2 path: ONNX Runtime in libaudio

> *Added as the decided direction for the next real step (after the §5 octave
> problem). Records the decision, the architecture, and the basic-pitch I/O
> contract. Status: **Phase 1a (ONNX foundation) is implemented and verified** —
> the real `nmp.onnx` loads, the Core ML EP is active, and a 440 Hz (A4) sine
> comes back as MIDI 69. The basic-pitch adapter + 14-file corpus run is the
> next increment (Phase 1b). See the §7.4 phasing.*

**Decision.** Move to **Tier 2** by hosting *neural* transcribers inside
**libaudio** on **ONNX Runtime** (with the **Core ML** execution provider for
Apple GPU/ANE). We **consume** pretrained models; we do not train. The same
runtime hosts basic-pitch (now) and — after ONNX export — TF-MAGS and Demucs
(later). This departs from the pure-aubio Tier-1 path for exactly the reason in
§5: a model that *explicitly models the harmonic series* resolves the octave
that YIN cannot.

**Why ONNX (not raw TensorFlow/PyTorch).** ONNX is the *common-denominator*
export format for the Python tools we want: basic-pitch, TF-MAGS, and Demucs
can all be published as `.onnx`. One C++ runtime (ONNX Runtime) then hosts them
all behind one seam, so libaudio gets local Tier-2 with no Python process in the
loop. basic-pitch ships its model *already* as `nmp.onnx` — no conversion step,
which is why it leads.

### 7.1 Architecture (ports & adapters)

- **Ports** (abstract, in libaudio): `Transcriber` (audio → `Score`) and, later,
  `Separator` (mix → per-source audio). The existing aubio Tier-1 pipeline is one
  `Transcriber` adapter; basic-pitch is another.
- **`onnx_session`** — *one* Pimpl class, the **single** translation unit that
  includes the ONNX Runtime C++ headers. It loads a `.onnx`, runs it, and returns
  raw output tensors. It hides `Ort*` types exactly as the aubio `Impl`s do.
- **Per-model adapters** (e.g. `BasicPitch`) — know *one* model's I/O contract
  and `output_semantics`; emit libaudio HIR `Note`s.
- **`ModelDescriptor`** — in-code declaration of a model: id/version/opset; input
  contract (sample-rate, channels, front-end type, frame-rate, note range);
  output contract (names/shapes + an `output_semantics` enum that *selects* the
  post-processor); post-proc knobs.
- **`manifest`** — per-model provenance: submodule commit, weight sha256, license,
  converting tool + the runtime it was validated on.
- **Front-end** — resample to the model's rate + window. For basic-pitch this is
  all it is (the CQT lives *inside* the model); mel/CQT math is added only when a
  model needs it (TF-MAGS).
- **Post-proc** — small files selected by `output_semantics`; emit the existing
  HIR, then reuse `ScoreBuilder` → `MidiFileWriter`.
- **`external/`** holds git submodules (basic-pitch first, then tf-mags, demucs).
  Each model's weights are referenced **in place** from its submodule (no copy),
  with provenance (commit pin, weight sha256, license, validating runtime) in
  `manifests/` — one source of truth.
- **`LIBAUDIO_ENABLE_TIER2`** CMake flag gates all of the above so the Tier-1
  aubio path is byte-for-byte unaffected when off.
- **Test harness** — the analyzer-agnostic C++ port of
  `midicapture/render_test_suite.py` (14-file corpus + the recall/precision/Δ
  metrics).

### 7.2 basic-pitch I/O contract (verified from the real model)

| | |
|---|---|
| **Input** | raw **mono @ 22050 Hz**, float32; window = **43844 samples** (a 2 s window: `22050·2 − 256`). The **CQT is computed inside the model**. C++ front-end = resample→22050 mono + cut into 43844-sample chunks. No mel/CQT math in C++. |
| **Outputs** | `note (T,88)`, `onset (T,88)`, `contour (T,264)` per window. **172 frames**/window @ **86 fps** (hop ≈ 11.6 ms). 88 bins = **MIDI 21..108**; 264 = 3/semitone (fine pitch / pitch-bend). |
| **Windowing** | hop by 256 samples over overlapping 2 s windows; per-window outputs are **stitched** (~30-frame overlap). The C++ port reproduces window + overlap-stitch. |
| **Post-proc** | port of `note_creation.py`: onset/frame-threshold note detector + inferred onsets + min/max-freq gate → notes; velocity = `round(127·amplitude)`. **Pitch-bends (`contour`) skipped initially** (a later enhancement). |
| **Model file** | `basic_pitch/saved_models/icassp_2022/nmp.onnx` (228 KB, made by tf2onnx 1.15.1) — **committed in the repo, no conversion step.** Code + weights **Apache-2.0**. |

Tunable post-proc constants (exposed in `ModelDescriptor` + the harness): onset
threshold **0.5**, frame threshold **0.3**, min note length **11 frames**,
velocity scale **127**.

### 7.3 Reuse (the C++ stays small)

basic-pitch's adapter emits the **existing** HIR `Note`s and reuses
`VelocityEstimator` / `NoteTrimmer` / `ScoreBuilder` / `MidiFileWriter` and the
existing aubio Tier-1 path, all behind the `Transcriber` port. The genuinely new
code is `onnx_session` + the basic-pitch adapter + a small piano-roll post-proc —
not a from-scratch transcription engine.

### 7.4 Phasing

| Phase | Scope |
|---|---|
| **1a** | ~~ONNX foundation~~ **Done + verified:** `onnx_session` (Core ML EP) + `ModelDescriptor` + fail-fast I/O validation + a real-model smoke test. The real `nmp.onnx` loads, the Core ML EP is active, and an A4 sine is detected as MIDI 69. | **1b** | **basic-pitch** adapter (resample→window→overlap-stitch) + `piano_roll` post-proc → HIR, run on the 14-file corpus, compared against ground truth. *(next increment)* |
| **2** | **TF-MAGS** (Onsets&Frames) via ONNX export + a mel front-end — a second model exercising the same seam. |
| **3** | **Demucs** `Separator` + per-stem transcription (melody / accompaniment / vocals) — the "arbitrary instruments / bands / vocals" end goal. |

**Decided at build time (Phase 1a):** CMake finds onnxruntime via
`find_package(onnxruntime CONFIG)` against the Homebrew prefix (option **b**);
`LIBAUDIO_ENABLE_TIER2` defaults **OFF** so the aubio Tier-1 path is unaffected;
the Core ML EP is requested but its failure is non-fatal (CPU fallback). See
`libaudio/CMakeLists.txt` and `manifests/basic-pitch.txt`.

# libaudio — Tier-2 Neural Transcription (ONNX Runtime in libaudio)

> The decided Tier-2 path: host *neural* transcribers inside **libaudio** on
> **ONNX Runtime** (+ Core ML EP), consuming pretrained models we never train.
> This is the content that lived in [../audio-to-midi.md](../audio-to-midi.md)
> §7 (the cross-module file keeps a short pointer). It is libaudio-specific, so
> it lives here. Status: **Phase 1a (ONNX foundation) + 1b (basic-pitch adapter +
> 18-case corpus, timidity voice) implemented and verified.** Loded alongside
> [summary.md](summary.md).

---

## 1. Decision & rationale

Move to **Tier 2** by hosting *neural* transcribers inside **libaudio** on
**ONNX Runtime** (with the **Core ML** execution provider for Apple GPU/ANE).
We **consume** pretrained models; we do not train. The same runtime hosts
basic-pitch (now) and — after ONNX export — TF-MAGS and Demucs (later).

This departs from the pure-aubio Tier-1 path for exactly the reason in
[../audio-to-midi.md](../audio-to-midi.md) §5: a model that *explicitly models the
harmonic series* resolves the octave that YIN cannot.

**Why ONNX (not raw TF/PyTorch).** ONNX is the common-denominator export format:
basic-pitch, TF-MAGS, and Demucs can all be published as `.onnx`. One C++ runtime
hosts them all behind one seam, so libaudio gets local Tier-2 with no Python
process in the loop. basic-pitch ships its model *already* as `nmp.onnx` — no
conversion step, which is why it leads.

## 2. Architecture (ports & adapters)

- **Ports** (abstract, in libaudio): `Analyzer` (audio → `Score`) and, later,
  `Separator` (mix → per-source audio). The aubio Tier-1 pipeline is the concrete
  `libaudio::Transcriber`; basic-pitch is `libaudio::BasicPitch` — both are
  `Analyzer`s, so the corpus harness and the CLI stay analyzer-agnostic.
- **`onnx_session`** — *one* Pimpl class, the **single** translation unit that
  includes the ONNX Runtime C++ headers. It loads a model, runs it, and returns
  raw output tensors. Two loaders share the same Core ML-request / CPU-fallback
  semantics: `load(path)` (a `.onnx` on disk) and `loadFromMemory(data, len)`
  (a buffer — how a model embedded into the binary is loaded). It hides `Ort*`
  types exactly as the aubio `Impl`s do. The Tier-2 sources live in `src/onnx/`
  (runtime layer) and `src/basicPitch/` (the model adapter); headers stay flat in
  `include/`.
- **Per-model adapters** (e.g. `BasicPitch`) — know *one* model's I/O contract and
  `output_semantics`; emit libaudio HIR `Note`s. `BasicPitch::getRawPredictions`
  runs the model **once** and returns the stitched note/onset/contour maps
  (`RawPredictions`, `rawMap.{h,cpp}`) — the pre-decode state a post-proc knob-sweep
  reads **without re-running the model**. The resampled audio is *not* cached (it is
  cheap + regenerable from the source); the maps (the expensive model run) are the
  artifact. A small dependency-free **binary** file format (`writeRawPredictions` /
  `readRawPredictions`) round-trips it byte-for-byte. Exposed by `midicapture
  --dump-raw-map` (raw-map dump: [../plans/postproc-tuning.md](../plans/postproc-tuning.md) §9).
- **`ModelDescriptor`** — in-code declaration of a model: id/version/opset; input
  contract (sample-rate, channels, front-end type, frame-rate, note range);
  output contract (names/shapes + an `output_semantics` enum that *selects* the
  post-processor). A **pure I/O contract** — the tunable note-creation knobs live
  on `BasicPitchOptions` (§3), not here. Fail-fast `validateInput` /
  `validateOutputs` let the test harness validate a model's I/O instead of
  silently mis-reading it.
- **`manifest`** — per-model provenance: submodule commit, weight sha256, license,
  converting tool + the runtime it was validated on.
- **Front-end** — resample to the model's rate + window. For basic-pitch this is
  all it is (the CQT lives *inside* the model); mel/CQT math is added only when a
  model needs it (TF-MAGS). basic-pitch decodes + resamples **in-process** via
  `detail::decodeToMonoFloat` (`src/ffmpegDecode.cpp`): the FFmpeg shared
  libraries (libavformat/libavcodec/libswresample) in one streaming pass →
  mono float32 @ 22050 Hz — **no subprocess, no temp file**. (The installed
  aubio lacks `libsamplerate`, so `TemporalProcessor::resample()` silently
  returns empty — a 48 kHz source would yield 0 notes — and is not used.)
  Inputs libsndfile reads natively at the model rate skip the decode and read
  directly; a different rate or a container libsndfile cannot open (e.g. mp4)
  goes through the in-process decode. The ffmpeg *binary* remains a dep only
  for the Tier-1 aubio container fallback (`AudioSource::open`).
- **Post-proc** — small files selected by `output_semantics`; emit the existing
  HIR, then reuse `ScoreBuilder` → `MidiFileWriter`.
- **`external/`** holds git submodules (basic-pitch first, then tf-mags, demucs).
  Each model's weights are referenced **in place** from its submodule (no second
  checked-in copy), with provenance in `manifests/`. For basic-pitch the weights
  are additionally **embedded into the binary at build time**: a generator turns
  `nmp.onnx` into a byte-array header (SHA-256-verified against the manifest; the
  build fails if the model drifted), so the shipping binary is self-contained and
  needs no model file on disk at runtime.
- **`LIBAUDIO_ENABLE_TIER2`** CMake flag gates all of the above (default **OFF**)
  so the Tier-1 aubio path is byte-for-byte unaffected when off.
- **Test harness** — the analyzer-agnostic C++ port of
  `midicapture/render_test_suite.py` (18-case corpus, timidity-rendered +
  recall/precision/Δ metrics).

## 3. basic-pitch I/O contract (verified from the real model)

| | |
|---|---|
| **Input** | raw **mono @ 22050 Hz**, float32; window = **43844 samples** (a 2 s window: `22050·2 − 256`). The **CQT is computed inside the model**. C++ front-end = resample→22050 mono + cut into 43844-sample chunks. No mel/CQT math in C++. |
| **Outputs** | `note (T,88)`, `onset (T,88)`, `contour (T,264)` per window. **172 frames**/window @ **86 fps** (hop ≈ 11.6 ms). 88 bins = **MIDI 21..108**; 264 = 3/semitone (fine pitch / pitch-bend). |
| **Windowing** | hop by 256 samples over overlapping 2 s windows; per-window outputs are **stitched** (~30-frame overlap). The C++ port reproduces window + overlap-stitch. |
| **Post-proc** | port of `note_creation.py`: onset/frame-threshold note detector + inferred onsets + min/max-freq gate → notes; velocity = `round(127·amplitude)`; **pitch-bends** (`get_pitch_bends`) decode the 264-bin `contour` map into `Note.pitchBends` (14-bit MIDI ticks via a 51-bin Gaussian argmax, then a 1-bin deadband + overlap drop); those bends are emitted as `0xE0` by the writer (canonical running status) and round-trip through the now-standards-compliant reader; **per-pitch channels** route each distinct bent pitch to its own channel 1..15 (gated by `multiplePitchBends`; default off = reference parity). On these decaying `timidity` renders it fragments long/whole notes into extra same-pitch notes (correct pitch; precision ~40–80%). |
| **Model file** | `basic_pitch/saved_models/icassp_2022/nmp.onnx` (230,444 bytes, tf2onnx 1.15.1) — lives in the `external/basic-pitch` submodule (pinned) and is **embedded into the binary at build time** (generated header, SHA-256-verified). Code + weights **Apache-2.0**. |

Tunable post-proc knobs (in `BasicPitchOptions`; the descriptor is a pure I/O
contract): onset threshold **0.5**, frame threshold **0.3**, min note length
**127.7 ms** (11 frames), velocity scale **127**, min/max frequency **27.5 / 4186
Hz** (A0..C8; the band is kept *inclusive*), the `inferOnsets` / `melodiaTrick`
gates (both on), and `midiTempo` **120**. All default to the reference values so a
no-flag run is byte-identical (18/18 corpus parity, metrics unchanged); each is
clamped per [../plans/postproc-tuning.md](../plans/postproc-tuning.md) §6.

## 4. Reuse (the C++ stays small)

basic-pitch's adapter emits the **existing** HIR `Note`s and reuses
`VelocityEstimator` / `NoteTrimmer` / `ScoreBuilder` / `MidiFileWriter` and the
existing aubio Tier-1 path, all behind the `Analyzer` port. The genuinely new
code is `onnx_session` + the basic-pitch adapter + a small piano-roll post-proc —
not a from-scratch transcription engine.

## 5. Phasing

| Phase | Scope |
|---|---|
| **1a** | ~~ONNX foundation~~ **Done + verified:** `onnx_session` (Core ML EP) + `ModelDescriptor` + fail-fast I/O validation + a real-model smoke test. `nmp.onnx` loads, Core ML active, A4 → MIDI 69. |
| **1b** | **basic-pitch** adapter (in-process FFmpeg decode→window→overlap-stitch) + `piano_roll` post-proc → HIR; the corpus → **100% recall, correct octave+chroma** (Core ML, ~63/246 nodes, ~2 s; precision is voice-dependent — see the §5 *Measured* note on the timidity re-baseline). |
| **1c** | **Pitch-bends** — (a) ~~extract~~ **done**: the `contour` map decodes into `Note.pitchBends` (14-bit ticks); (b) ~~emit 0xE0~~ **done**: the writer emits bends + the reader round-trips them (a constant-pitch note stays byte-identical); (c) ~~per-pitch channels~~ **done**: each distinct bent pitch gets its own channel 1..15 (ascending pitch, capped); the writer scaffolds per-channel program/sustain (byte-identical when only channel 0). `--no-pitch-bends` gates (a); `--multiple-pitch-bends` gates (c). See [plans/basic-pitch-tier1.md](../plans/basic-pitch-tier1.md). |
| **2** | **TF-MAGS** (Onsets&Frames) via ONNX export + a mel front-end — a second model exercising the same seam. |
| **3** | **Demucs** `Separator` + per-stem transcription (melody / accompaniment / vocals). |

**Decided at build time (Phase 1a):** CMake finds onnxruntime via
`find_package(onnxruntime CONFIG)` against the Homebrew prefix; the Core ML EP is
requested but its failure is non-fatal (CPU fallback).

**Measured on the 18-case corpus (timidity voice — the current regression
anchor):** basic-pitch on Core ML (63/246 nodes; ~2 s for all 18) → **100%
recall (0 missed), avg precision 45.4%** (per-file 14–100%), correct octave
(median Δoct 0.1) and chroma (median 0.0); median onset 3.9 ms. It resolves the
§5 octave problem monophonic YIN cannot (aubio on the same corpus: 6% recall,
octave-unreliable). The voice switch from the old in-process C++ synth to
**timidity re-baselined** this corpus (the C++ synth is now gone): recall and
octave/chroma are stable across the re-voice, but **precision dropped** (from the
historical C++-synth 68% to timidity 45.4%) because timidity's longer, pedalled
decays fragment long/whole notes into extra same-pitch notes — the same defrag
precision sink, not a pitch error. `sustained-run-whole-notes` is the stand-out
outlier (Δoct 2.0, precision 25%) — the in-corpus canary for the
[post-proc defrag](../plans/postproc-tuning.md) work. Velocity is the most
drift-prone metric (float-derived; the Core ML cross-session ULP note below);
recall/precision/onset/duration are the stable gate. It is now midicapture's
**default `--model`** in a Tier-2 build (`aubio` is the monophonic fallback):
`midicapture --run-corpus test-midi --model {basic|aubio}` (`--analyzer` is a
deprecated alias for `--model`).

**Reproducibility note (Core ML):** the model is **deterministic within a session**
(a no-flag `--run-corpus` is byte-identical across repeated runs, and the metrics
above are the stable regression gate). **Across sessions** the *exact output bytes*
are *not* bit-stable on the Core ML path — the signature is a 1-velocity-tick flip,
a near-threshold false note flickering in/out, or a few duration-boundary ticks,
while the **recall / precision / onset / duration metrics stay identical** (the
discrete note *detections* are bit-stable; only float-derived bytes move). This is
*consistent with* the Core ML / Apple-ANE execution provider re-deriving slightly
different activations across launches, and is **not** a code regression. It does not
affect the post-proc **sweep**, which runs the model *once* and re-decodes the
cached raw-map under each knob with pure C++ (immune to the drift) — see
[../plans/postproc-tuning.md](../plans/postproc-tuning.md) §9.

## 6. Cross-References

- [summary.md](summary.md) — the libaudio module overview (Tier-1 core)
- [../audio-to-midi.md](../audio-to-midi.md) — the cross-module problem framing (§5 octave, §6 eval)
- [hir.md](hir.md) — the `Score` / `Note` the adapter emits
- [../plans/basic-pitch-tier1.md](../plans/basic-pitch-tier1.md) — the remaining basic-pitch promotion work (midicapture default: done; libaudio tier-flattening: open)

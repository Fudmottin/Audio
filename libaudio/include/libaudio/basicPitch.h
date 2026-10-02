/**
 * @file basicPitch.h
 * @brief The basic-pitch (Spotify) polyphonic transcriber, hosted on ONNX.
 *
 * `BasicPitch` is libaudio's Tier-2 `Analyzer`: it knows *one* model's I/O
 * contract (the `basicPitchDescriptor()`) and turns an audio file path into a
 * HIR `Score` of notes. It is the Tier-2 upgrade path for midicapture
 * (audio-to- midi.md: a model that explicitly models the harmonic series
 * resolves the octave the monophonic YIN path cannot).
 *
 * The adapter is **ORT-free**: it talks only to `OnnxSession` / `Tensor` /
 * `ModelDescriptor`. The ONNX Runtime headers are confined to
 * `onnxSession.cpp`.
 *
 * @section basic-pitch-pipeline Pipeline
 *
 * `transcribe(path)`:
 *   1. Get mono float32 at the model's rate: probe the file with libsndfile
 *      and read directly when its rate matches (or is unknown); otherwise — a
 *      different rate, or a container libsndfile cannot open (e.g. mp4) —
 *      decode + resample in-process via the FFmpeg libraries (no subprocess,
 *      no temp file).
 *   2. Prepend the front pad, cut into overlapping 43844-sample windows
 *      (hop 36164), and run each window through the model.
 *   3. Overlap-stitch the per-window note/onset maps (trim the 30-frame overlap
 *      per window; apply the global start/end trims) into one global map.
 *   4. Decode the global map with `PianoRoll` into `Note`s; assemble the
 * `Score`.
 *
 * The exact windowing / trim math mirrors the reference `inference.py`
 * (`get_audio_input` + `unwrap_output`). When `BasicPitchOptions` has
 * `includePitchBends` set (the default), the model's fine-pitch *contour* map
 * is decoded into `Note::pitchBends` (a port of `get_pitch_bends`).
 *
 * @section basic-pitch-tier2 Tier-2 grouping
 *
 * Part of libaudio's Tier-2 layer; gated behind `LIBAUDIO_HAS_TIER2`.
 */

#ifndef LIBAUDIO_BASICPITCH_H
#define LIBAUDIO_BASICPITCH_H

#include <libaudio/analyzer.h>
#include <libaudio/hir.h>
#include <libaudio/modelDescriptor.h>
#include <libaudio/onnxSession.h>
#include <libaudio/pianoRoll.h>
#include <libaudio/rawMap.h>
#include <memory>
#include <string>
#include <string_view>

namespace libaudio {

// ============================================================================
// BasicPitchOptions — the user-tunable knobs for the basic-pitch
// post-processor.
//
// These are how a *given* model output is turned into notes: the note-detection
// thresholds, the frequency band, the decode gates, the output tempo, and the
// pitch-bend policy. They are separate from the model's fail-fast I/O contract
// (`ModelDescriptor`): the options change the *post-processing*, not what the
// model expects. The defaults mirror the reference's `predict()` /
// `model_output_to_notes()` so that a run with no tuning is byte-identical to
// the baseline; `clampOptions` bounds the tunable range (declared below).
// ============================================================================
struct BasicPitchOptions {
   // --- Note-detection thresholds (moved off the model contract) ----------
   /**
    * Onset activation threshold (0..1). A candidate onset peak must reach this
    * activation. The reference default is 0.5.
    */
   float onsetThreshold = 0.5f;

   /**
    * Frame (sustain) activation threshold (0..1). A note's sustain run extends
    * while the note energy stays above this. The reference default is 0.3.
    */
   float frameThreshold = 0.3f;

   /**
    * Minimum note length, in milliseconds. Notes shorter than this are
    * discarded. (The reference stores this in *frames*; we keep the
    * time-domain form and convert at decode time with the reference's exact
    * formula, so 127.7 ms → 11 frames at 86.13 fps.) The reference default is
    * 127.7 ms.
    */
   double minNoteLenMs = 127.7;

   /**
    * Velocity scale: `velocity = clamp(round(scale * maxAmplitude), 1, 127)`.
    * The reference default is 127.
    */
   int velocityScale = 127;

   // --- Frequency band -----------------------------------------------------
   /**
    * Lowest frequency (Hz) to consider; activations below it are zeroed.
    * 27.5 Hz = A0, the lowest piano key (MIDI 21). With the defaults the band
    * spans the whole 88-key range, so it is a no-op.
    */
   double minFrequency = 27.5;

   /**
    * Highest frequency (Hz) to consider; activations above it are zeroed.
    * 4186 Hz = C8, the highest piano key (MIDI 108). Together with
    * `minFrequency` this is the inclusive band the decode keeps (a port of the
    * reference `constrain_frequency`); the defaults cover all 88 bins.
    */
   double maxFrequency = 4186.0;

   // --- Decode gates -------------------------------------------------------
   /**
    * Infer onsets the model missed (sharp rises in the note energy). The
    * reference default is on.
    */
   bool inferOnsets = true;

   /**
    * The melodia trick: claim notes that have no onset peak by growing the
    * remaining-energy maxima in both time directions. The reference default is
    * on.
    */
   bool melodiaTrick = true;

   // --- Output timing ------------------------------------------------------
   /**
    * The output MIDI tempo (bpm). The reference default is 120. (`midicapture`
    * additionally honours its `--tempo` flag on the written file; this is the
    * engine's internal value for library / knob-sweep use.)
    */
   double midiTempo = 120.0;

   // --- Pitch bends --------------------------------------------------------
   /**
    * Decode the model's fine-pitch contour map into `Note::pitchBends`.
    * True is the reference (Python) default.
    */
   bool includePitchBends = true;

   /**
    * Allow overlapping notes to each carry a pitch bend. When false (the
    * reference default), a note's bends are dropped if it overlaps any other
    * note — MIDI has one bend wheel per channel, so two bending notes on a
    * channel would fight. When true, each *distinct bent pitch* is routed to
    * its own MIDI channel (1..15, ascending pitch; channel 0 = non-bent
    * notes), so overlapping bent notes each get their own bend wheel.
    */
   bool multiplePitchBends = false;

   /**
    * A note whose pitch-bend never moves more than this many contour bins
    * (1/3 semitones each) is treated as a *flat* note and its bend vector is
    * cleared. The reference has **no** such floor: it stores a per-frame bend
    * even when the contour merely wobbles by a bin or two around the base
    * pitch, which would render as audible tremolo on a note meant to be flat.
    *
    * Default 1.0 (= 1 contour bin = 1/3 semitone). This is an *improvement*
    * over the reference, not a parity requirement: a constant-pitch source
    * (our test corpus) whose model contour tracks flatly reads |bend| ≤ 1 bin
    * per frame and is cleared, so its output stays byte-identical to the
    * no-bend path. Set to 0 for strict reference parity (keep every bend,
    * wobble included).
    */
   double bendDeadbandBins = 1.0;
};

// ============================================================================
// clampOptions — bound the tunable knobs to their sane ranges.
//
// The tuner's guardrail (not a parity requirement): a non-finite float is an
// *error* (fail fast, like the descriptor's validation); an out-of-range
// finite value is clamped to the nearest bound with a one-line note to stderr
// (never a silent accept, never a crash). Every default already lies inside its
// range, so a run with no tuning is byte-identical (all the clamps no-op on the
// defaults). `setOptions` applies it, so whatever a caller passes is stored in
// a safe form.
// ============================================================================
[[nodiscard]] BasicPitchOptions clampOptions(const BasicPitchOptions& options);

// ============================================================================
// BasicPitch — the basic-pitch transcriber adapter (audio → Score).
//
// Domain context: basic-pitch is a small neural net for *polyphonic* piano that
// emits per-pitch note/onset activation maps directly (the CQT is computed
// *inside* the model, so the C++ front-end is only resample + window + stitch).
// This class is the glue between that model and the HIR: it is driven by
// `basicPitchDescriptor()` for all its numeric constants.
//
// A single `BasicPitch` owns one loaded model session and can transcribe many
// files; `transcribe` is const (a session is reusable). Non-copyable, movable.
// ============================================================================
class BasicPitch : public Analyzer {
 public:
   // Prepare for transcription. The basic-pitch model (`nmp.onnx`) is embedded
   // into the binary at build time (see `nmp_onnx_data.h`), so there is no
   // model path to resolve on disk. Audio decode + resample is in-process
   // (FFmpeg shared libraries); no external tool is needed.
   //
   // @throws std::runtime_error if the model cannot be loaded.
   BasicPitch();

   // Destructor. Releases the ONNX session.
   ~BasicPitch();

   BasicPitch(const BasicPitch&) = delete;
   BasicPitch& operator=(const BasicPitch&) = delete;

   BasicPitch(BasicPitch&& other) noexcept;
   BasicPitch& operator=(BasicPitch&& other) noexcept;

   // Transcribe an audio file to a HIR Score (see the pipeline above).
   // @throws std::runtime_error if the audio cannot be read or a run fails.
   [[nodiscard]] Score transcribe(std::string_view path) const override;

   // Run the model once over `path` and return the raw stitched activation
   // maps (note / onset / contour) plus `annotNFrames` — the pre-decode state
   // a post-processor (or a knob-sweep) consumes. This is the model's output
   // before any thresholding / segmentation, and is the expensive artifact to
   // cache: the resampled audio it was built from is deliberately *not* kept
   // (it is cheap and regenerable from the source on demand).
   //
   // Unlike `transcribe` it always accumulates the contour map, so a cached
   // `RawPredictions` can be re-decoded with or without pitch bends without
   // another model run. Pair with `writeRawPredictions` / `readRawPredictions`
   // (rawMap.h) to persist or reload it.
   //
   // @throws std::runtime_error if the audio cannot be read or a run fails.
   [[nodiscard]] RawPredictions getRawPredictions(std::string_view path) const;

   // The analyzer's stable identifier.
   [[nodiscard]] std::string name() const override;

   // Whether the model is actually running on the Core ML execution provider
   // (false = CPU fallback, still correct).
   [[nodiscard]] bool coreMlActive() const;

   /**
    * Configure the post-processing behaviour of this transcriber.
    *
    * The options live on the concrete `BasicPitch` (not on the generic
    * `Analyzer` port, which is `transcribe` + `name` only) because they are
    * specific to basic-pitch's three-way note/onset/contour output. They are
    * clamped to their sane ranges (`clampOptions`) before being stored: a
    * non-finite knob is an error, an out-of-range knob is clamped to its bound
    * with a note to stderr. A no-flag run is unaffected (the defaults are in
    * range), so `transcribe` stays byte-identical.
    *
    * @throws std::invalid_argument if any knob is non-finite (NaN / ±inf).
    */
   void setOptions(const BasicPitchOptions& options);

   // The currently active options, already clamped (the defaults until
   // `setOptions` is called).
   [[nodiscard]] const BasicPitchOptions& options() const;

 private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_BASICPITCH_H

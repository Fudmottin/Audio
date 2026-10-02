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
// BasicPitchOptions — the user-tunable knobs specific to basic-pitch.
//
// These are the post-processing choices the reference exposes (the Python
// `model_output_to_notes` kwargs `include_pitch_bends` /
// `multiple_pitch_bends`) plus one intentional *improvement* over the reference
// (the bend deadband, see below). They are separate from the model's fail-fast
// contract
// (`ModelDescriptor`): the options change how a *given* model output is turned
// into notes, not what the model expects.
// ============================================================================
struct BasicPitchOptions {
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
    * specific to basic-pitch's three-way note/onset/contour output.
    */
   void setOptions(const BasicPitchOptions& options);

   // The currently active options (the defaults until `setOptions` is called).
   [[nodiscard]] const BasicPitchOptions& options() const;

 private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_BASICPITCH_H

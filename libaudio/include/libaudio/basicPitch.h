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
 *   1. Resolve the path via `AudioSource` (libsndfile, else an ffmpeg decode).
 *   2. Read the whole file as mono; resample to 22050 Hz if needed.
 *   3. Prepend the front pad, cut into overlapping 43844-sample windows
 *      (hop 36164), and run each window through the model.
 *   4. Overlap-stitch the per-window note/onset maps (trim the 30-frame overlap
 *      per window; apply the global start/end trims) into one global map.
 *   5. Decode the global map with `PianoRoll` into `Note`s; assemble the
 * `Score`.
 *
 * The exact windowing / trim math mirrors the reference `inference.py`
 * (`get_audio_input` + `unwrap_output`). Pitch-bends (the contour map) are
 * skipped for now.
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
#include <memory>
#include <string>
#include <string_view>

namespace libaudio {

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
   // model path to resolve on disk.
   //
   // @param ffmpegPath  Path to ffmpeg, used only if the input cannot be read
   //                    directly by libsndfile (the `AudioSource` fallback).
   //
   // @throws std::runtime_error if the model cannot be loaded.
   explicit BasicPitch(
      const std::string& ffmpegPath = "/opt/homebrew/bin/ffmpeg");

   // Destructor. Releases the ONNX session.
   ~BasicPitch();

   BasicPitch(const BasicPitch&) = delete;
   BasicPitch& operator=(const BasicPitch&) = delete;

   BasicPitch(BasicPitch&& other) noexcept;
   BasicPitch& operator=(BasicPitch&& other) noexcept;

   // Transcribe an audio file to a HIR Score (see the pipeline above).
   // @throws std::runtime_error if the audio cannot be read or a run fails.
   [[nodiscard]] Score transcribe(std::string_view path) const override;

   // The analyzer's stable identifier.
   [[nodiscard]] std::string name() const override;

   // Whether the model is actually running on the Core ML execution provider
   // (false = CPU fallback, still correct).
   [[nodiscard]] bool coreMlActive() const;

 private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_BASICPITCH_H

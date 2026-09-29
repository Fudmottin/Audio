/**
 * @file transcriber.h
 * @brief The monophonic audio → Score transcription engine.
 *
 * `Transcriber` is libaudio's Tier-1, model-free transcription engine: a
 * YINfft pitch detector + spectral-flux onset detector driving a monophonic
 * energy-hysteresis state machine. It implements the `Analyzer` port (see
 * analyzer.h), so it is the concrete analyzer the corpus harness and
 * midicapture use behind the `Analyzer` seam alongside `BasicPitch`.
 *
 * @section transcriber-pipeline Pipeline
 *
 * `transcribe(path)`:
 *   1. Resolve the input path via `AudioSource` (libsndfile probe, else an
 *      ffmpeg decode) so any container opens, then read the resolved path.
 *   2. For each hop of audio: measure energy, estimate pitch (YINfft), and
 *      detect onsets (spectral flux) in a single sequential read.
 *   3. Gate note boundaries on energy (RMS vs. a silence threshold) with an
 *      on/off hysteresis state machine, and on sustained pitch change.
 *   4. Resolve each note's final pitch (chroma voted over its lifetime;
 *      octave anchored to its loudest hop) and velocity, then merge
 *      same-pitch fragments and assemble a HIR `Score`.
 *
 * @section transcriber-monophonic Monophonic
 *
 * This is the *monophonic* engine: it tracks one active note at a time.
 * Polyphony (chords) is provided by the Tier-2 `BasicPitch` analyzer, not here.
 *
 * A `Transcriber` owns its analysis state and can transcribe many files: each
 * `transcribe()` resets the per-file state, so a single instance is safe to
 * reuse across a corpus (matching the `BasicPitch` reuse contract).
 *
 * @section transcriber-tier1 Tier-1 grouping
 *
 * Part of libaudio's Tier-1 layer (always compiled). It uses only aubio
 * (pitch/onset) + libsndfile (via `AudioFileReader` / `AudioSource`); no ONNX.
 */

#ifndef LIBAUDIO_TRANSCRIBER_H
#define LIBAUDIO_TRANSCRIBER_H

#include <cstdint>
#include <libaudio/analyzer.h>
#include <libaudio/hir.h>
#include <memory>
#include <string>
#include <string_view>

namespace libaudio {

// ============================================================================
// Transcriber — the Tier-1 monophonic transcription engine (audio → Score).
//
// Domain context: the main entry point for the model-free monophonic path. It
// orchestrates the full transcription pipeline: reading audio, detecting pitch
// and onsets, segmenting notes with an energy hysteresis state machine, and
// returning a HIR Score. Note boundaries are gated on *energy*, not on the
// pitch detector's confidence: YINfft reports a usable fundamental even for
// noise (and on rendered piano notes its confidence reads ~0), so confidence is
// not a reliable gate. See the implementation for the full pipeline.
//
// RAII — all analysis resources are freed when the Transcriber is destroyed.
// Non-copyable; implements the `Analyzer` port.
// ============================================================================
class Transcriber : public Analyzer {
 public:
   // Create a Transcriber with the given analysis parameters.
   //
   // @param bufSize    FFT window size (e.g., 2048).
   // @param hopSize    Hop size between frames (e.g., 512).
   // @param silenceDb  Silence threshold in dB (-40.0 = typical default).
   // @param pitchMethod  Pitch detection method ("yinfft", etc.).
   // @param tempoBpm   Tempo in BPM of the output Score (drives the
   //                   fragment-merge gap and the MIDI tick conversion).
   // @param ffmpegPath  Path to ffmpeg, used only if the input cannot be read
   //                    directly by libsndfile (the `AudioSource` fallback).
   Transcriber(uint32_t bufSize, uint32_t hopSize, float silenceDb,
               const std::string& pitchMethod, double tempoBpm = 120.0,
               const std::string& ffmpegPath = "/opt/homebrew/bin/ffmpeg");

   // Destructor. Frees all analysis resources (RAII).
   ~Transcriber();

   // Non-copyable (analysis handles are non-copyable).
   Transcriber(const Transcriber&) = delete;
   Transcriber& operator=(const Transcriber&) = delete;

   // Movable.
   Transcriber(Transcriber&& other) noexcept;
   Transcriber& operator=(Transcriber&& other) noexcept;

   // Transcribe an audio file to a HIR Score (see the pipeline above).
   //
   // Resolves the input to a libsndfile-readable path, then runs the
   // monophonic pipeline and returns the detected notes as a Score.
   //
   // @param path Path to the input audio file (AIFF, WAV, FLAC, MP3, ...).
   // @return A HIR Score with detected notes and control events.
   // @throws std::runtime_error if the file cannot be opened or analyzed.
   [[nodiscard]] Score transcribe(std::string_view path) const override;

   // The analyzer's stable identifier (the corpus selects analyzers by this).
   [[nodiscard]] std::string name() const override;

 private:
   // Private implementation — all analysis state is isolated here. The pointer
   // is mutable so the const `transcribe()` can reset the per-file state.
   struct Impl;
   mutable std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_TRANSCRIBER_H

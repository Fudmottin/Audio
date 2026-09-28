/**
 * @file aubioTranscriber.h
 * @brief Wrap the monophonic aubio engine as an analyzer-agnostic
 * `Transcriber`.
 *
 * `AubioTranscriber` adapts midicapture's existing Tier-1 monophonic engine
 * (the global `Transcriber`, a YINfft + spectral-flux state machine) to the
 * `libaudio::Transcriber` port. It is the *second* analyzer behind the corpus
 * harness: `BasicPitch` (polyphonic neural, in libaudio) and `AubioTranscriber`
 * (monophonic DSP, here) both implement the same `libaudio::Transcriber` seam,
 * so the harness scores either one identically.
 *
 * The one reason this adapter exists at all is the corpus files are **MP3**:
 * the aubio engine opens its input with a plain `AudioFileReader` (libsndfile),
 * which may not read an MP3 in a given build. So before running the engine we
 * resolve the path through `libaudio::AudioSource` (libsndfile-first, ffmpeg
 * fallback) and hand the engine a path libsndfile *can* open. Everything else
 * (pitch, onset, note segmentation, velocity) is the unchanged Tier-1 pipeline.
 *
 * This type lives in the global namespace to match the engine it wraps
 * (`midicapture/transcriber.h`) — the two `Transcriber` types (the global
 * engine and `libaudio::Transcriber`) are deliberately qualified everywhere to
 * keep them distinct.
 *
 * @section aubio-tier2 Tier-2 grouping
 *
 * Gated behind `LIBAUDIO_HAS_TIER2`: it depends on the Tier-2
 * `libaudio::Transcriber` port and `libaudio::AudioSource`, so it only exists
 * in a Tier-2 build. (The engine itself is Tier-1; only this *adapter* is
 * Tier-2 because it implements the Tier-2 port.)
 */

#ifndef MIDICAPTURE_AUBIO_TRANSCRIBER_H
#define MIDICAPTURE_AUBIO_TRANSCRIBER_H

#include <memory>
#include <string>
#include <string_view>

// The `libaudio::Transcriber` port and `libaudio::AudioSource` are Tier-2
// types; they are only declared when the build has Tier-2.
#ifdef LIBAUDIO_HAS_TIER2

#include <libaudio/audioDecode.h>
#include <libaudio/hir.h>
#include <libaudio/transcriber.h>
#include <midicapture/transcriber.h>

// ============================================================================
// AubioTranscriber — the Tier-1 aubio engine behind the `libaudio::Transcriber`
// port, so the corpus harness can run it analyzer-agnostically.
//
// Domain context: this is the "swap the analyzer" half of the corpus design.
// It holds only a path to ffmpeg and, for each `transcribe()`, constructs a
// *fresh* `::Transcriber` engine (fixed Tier-1 parameters) — it never reuses
// one across calls, because the engine's note-tracking state is not reset
// between `transcribe()` calls (see the .cpp for the full rationale).
//
// Movable, non-copyable (a `unique_ptr<Impl>` is moved).
// ============================================================================
class AubioTranscriber : public libaudio::Transcriber {
 public:
   // Build the adapter. The engine uses midicapture's standard Tier-1
   // parameters (2048 FFT window, 512 hop, -40 dB silence, yinfft, 120 BPM).
   //
   // @param ffmpegPath  Path to ffmpeg, used only if the input cannot be read
   //                    directly by libsndfile (the `AudioSource` fallback).
   explicit AubioTranscriber(
      const std::string& ffmpegPath = "/opt/homebrew/bin/ffmpeg");

   // Destructor.
   ~AubioTranscriber();

   AubioTranscriber(const AubioTranscriber&) = delete;
   AubioTranscriber& operator=(const AubioTranscriber&) = delete;

   AubioTranscriber(AubioTranscriber&& other) noexcept;
   AubioTranscriber& operator=(AubioTranscriber&& other) noexcept;

   // Transcribe an audio file to a HIR Score (see the file header).
   // @throws std::runtime_error if the audio cannot be read or analyzed.
   [[nodiscard]] libaudio::Score
   transcribe(std::string_view path) const override;

   // The analyzer's stable identifier (the corpus selects analyzers by this).
   [[nodiscard]] std::string name() const override;

 private:
   // The wrapped Tier-1 engine (global `::Transcriber`) + the ffmpeg path the
   // path-resolution step uses.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_HAS_TIER2

#endif // MIDICAPTURE_AUBIO_TRANSCRIBER_H

/**
 * @file transcriber.h
 * @brief High-level API for audio-to-MIDI transcription.
 *
 * The Transcriber orchestrates the full transcription pipeline:
 * 1. Open the input audio file.
 * 2. For each hop of audio: estimate pitch (YINfft) and onset
 *    (spectral flux) using a single sequential read of the file.
 * 3. Gate note boundaries on energy (RMS vs. silence threshold).
 * 4. Build a HIR Score with the detected notes.
 * 5. Return the Score (HIR) for MIDI writing.
 *
 * This is the main entry point for the monophonic prototype.
 * Polyphony (chords) is a future enhancement.
 *
 * @section design Design
 *
 * The Transcriber uses a simple state machine:
 * - IDLE: No note currently active. Waiting for an onset.
 * - PLAYING: A note is active. Watching for note-off.
 *
 * Transitions:
 * - IDLE → PLAYING: An onset is detected on a tonal hop.
 * - PLAYING → IDLE: Energy hysteresis (RMS falls below the release
 *   threshold and stays there for a few hops), or a stable pitch
 *   change, or an onset that replaces the current note.
 *
 * Note boundaries are gated on *energy*, not on the pitch detector's
 * confidence: YINfft reports a usable fundamental even for noise (and
 * on rendered piano notes its confidence reads ~0), so confidence is
 * not a reliable gate. See the transcriber implementation for the
 * full pipeline description.
 *
 */

#ifndef MIDICAPTURE_TRANSCRIBER_H
#define MIDICAPTURE_TRANSCRIBER_H

#include <cstdint>
#include <libaudio/hir.h>
#include <memory>
#include <string>

// ============================================================================
// Transcriber — High-level API for audio-to-MIDI transcription.
//
// Domain context: The Transcriber is the main entry point for the
// monophonic prototype. It orchestrates the full transcription pipeline:
// reading audio, detecting pitch and onsets, building notes, and
// returning a HIR Score.
//
// Key design decisions:
// - Monophonic state machine (IDLE / PLAYING).
// - Notes start on an onset; notes end on sustained silence
//   (RMS hysteresis) or a stable pitch change.
// - Returns a HIR Score (vector of Note + ControlEvent).
//
// RAII resource management — all resources are automatically
// freed when the Transcriber is destroyed.
// ============================================================================
class Transcriber {
 public:
   // Create a Transcriber with the given analysis parameters.
   //
   // @param bufSize    FFT window size (e.g., 2048).
   // @param hopSize    Hop size between frames (e.g., 512).
   // @param silenceDb  Silence threshold in dB (-40.0 = default).
   // @param pitchMethod  Pitch detection method ("yinfft", etc.).
   Transcriber(uint32_t bufSize, uint32_t hopSize, float silenceDb,
               const std::string& pitchMethod);

   // Destructor. Frees all analysis resources.
   // RAII — resources are released automatically.
   ~Transcriber();

   // Non-copyable (analysis handles are non-copyable).
   Transcriber(const Transcriber&) = delete;
   Transcriber& operator=(const Transcriber&) = delete;

   // Transcribe an audio file to a HIR Score.
   //
   // Opens the input audio file, runs pitch and onset detection
   // hop by hop, builds a Score with detected notes, and returns it.
   //
   // @param inputPath Path to the input audio file (AIFF, WAV, FLAC, etc.).
   // @return A HIR Score with detected notes and control events.
   // @throws std::runtime_error if the file cannot be opened.
   libaudio::Score transcribe(const std::string& inputPath) const;

 private:
   // Private implementation — all analysis state is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // MIDICAPTURE_TRANSCRIBER_H

// /**
//  * @file transcriber.h
//  * @brief High-level API for audio-to-MIDI transcription.
//  *
//  * The Transcriber orchestrates the full transcription pipeline:
//  * 1. Open the input audio file (via AudioFile).
//  * 2. Run pitch detection (YINfft) on each audio frame.
//  * 3. Run onset detection (spectral flux) on each frame.
//  * 4. Build a HIR Score with detected notes.
//  * 5. Return the Score (HIR) for MIDI writing.
//  *
//  * This is the main entry point for the monophonic prototype.
//  * Polyphony (chords) is a future enhancement.
//  *
//  * @section design Design
//  *
//  * The Transcriber uses a simple state machine:
//  * - IDLE: No note currently active. Waiting for onset.
//  * - PLAYING: A note is active. Watching for note-off.
//  *
//  * Transitions:
//  * - IDLE → PLAYING: When pitch confidence exceeds threshold AND
//  *   an onset is detected.
//  * - PLAYING → IDLE: When pitch confidence drops below threshold
//  *   (note-off) or when a new onset is detected (new note).
//  *
//  * @see lode/libaudio/hir.md — HIR specification
//  * @see lode/libaudio/decisions.md — Default parameters
//  */

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
// - Notes start when confidence exceeds threshold AND onset detected.
// - Notes end when confidence drops below threshold.
// - Returns a HIR Score (vector of Note + ControlEvent).
//
// RAII resource management — all resources are automatically
// freed when the Transcriber is destroyed.
// ============================================================================
class Transcriber {
 public:
   // Create a Transcriber with the given analysis parameters.
   //
   // @param bufSize              FFT window size (e.g., 2048).
   // @param hopSize              Hop size between frames (e.g., 512).
   // @param confidenceThreshold  Minimum confidence to detect a pitch
   //                             (0.0 = always, 1.0 = never).
   // @param silenceDb            Silence threshold in dB (-40.0 = default).
   // @param pitchMethod          Pitch detection method ("yinfft", etc.).
   Transcriber(uint32_t bufSize, uint32_t hopSize, float confidenceThreshold,
               float silenceDb, const std::string& pitchMethod);

   // Destructor. Frees all analysis resources.
   // RAII — resources are released automatically.
   ~Transcriber();

   // Non-copyable (analysis handles are non-copyable).
   Transcriber(const Transcriber&) = delete;
   Transcriber& operator=(const Transcriber&) = delete;

   // Transcribe an audio file to a HIR Score.
   //
   // Opens the input audio file, runs pitch and onset detection
   // frame by frame, builds a Score with detected notes, and returns it.
   //
   // @param inputPath Path to the input audio file (AIFF, WAV, FLAC, etc.).
   // @return A HIR Score with detected notes and control events.
   // @throws std::runtime_error if the file cannot be opened.
   Score transcribe(const std::string& inputPath) const;

 private:
   // Private implementation — all analysis state is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif  // MIDICAPTURE_TRANSCRIBER_H

/**
 * @file notes.h
 * @brief Note detection via aubio — combines onset + pitch + velocity.
 *
 * This module wraps aubio's note detection. It combines onset detection,
 * pitch estimation, velocity analysis, and note-off detection into a
 * single interface.
 *
 * @section notes-simplicity Why NoteDetector?
 *
 * This is the simplest path to a working transcription system. aubio's
 * `aubio_notes_t` does onset + pitch + velocity + note-off all in one
 * call. For monophonic piano, this alone could produce a reasonable
 * result.
 *
 *         release drop, min IoI)
 */

#ifndef LIBAUDIO_NOTES_H
#define LIBAUDIO_NOTES_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// ============================================================================
// NoteEvent — A note event detected by the NoteDetector.
//
// Domain context: This is the intermediate result from aubio's note
// detection. It contains the pitch (in MIDI note numbers, as a float
// for sub-note accuracy), velocity (0–127), and note-off MIDI note
// (from aubio's internal analysis).
//
// Aggregate type with no hidden state or side effects.
// ============================================================================
struct NoteEvent {
   /** MIDI note (float), or 0 if no note detected. */
   float pitchMidi = 0.0f;

   /** Velocity (0–127, from aubio's internal analysis). */
   float velocity = 0.0f;

   /** MIDI note to turn off (from aubio), or 0. */
   float noteOffMidi = 0.0f;
};

// ============================================================================
// NoteDetector — Note detection via aubio (combines onset + pitch + velocity).
//
// Domain context: This is the simplest path to a working transcription
// system. aubio's `aubio_notes_t` does onset + pitch + velocity +
// note-off all in one call. For monophonic piano, this alone could
// produce a reasonable result.
//
// Key design decisions:
// - Default method: "default" (aubio's default note detection).
// - Default silence threshold: -40 dB (good for piano).
// - Default release drop: 10 dB (aubio default).
// - Returns std::nullopt when no note is detected.
//
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class NoteDetector {
 public:
   // Create a note detector.
   //
   // @param method     Detection method (e.g., "default", "specflux").
   // @param bufSize    FFT window size (e.g., 1024, 2048, 4096).
   // @param hopSize    Step size between frames (hop).
   // @param sampleRate Sample rate of the input signal.
   NoteDetector(std::string_view method, uint32_t bufSize,
                uint32_t hopSize, uint32_t sampleRate);

   // Destructor. Frees aubio note detection resources.
   // RAII — resources are released automatically.
   ~NoteDetector();

   // Non-copyable (aubio handles are non-copyable).
   NoteDetector(const NoteDetector&) = delete;
   NoteDetector& operator=(const NoteDetector&) = delete;

   // Movable (aubio handles can be moved).
   NoteDetector(NoteDetector&& other) noexcept;
   NoteDetector& operator=(NoteDetector&& other) noexcept;

   // Detect notes in a buffer of audio samples.
   //
   // @param samples Input audio samples (length must equal bufSize).
   // @return std::optional<NoteEvent> with pitch, velocity, and note-off info,
   //         or std::nullopt if no note was detected.
   std::optional<NoteEvent> detect(const float* samples, uint32_t length);

   // Get the current silence threshold (below this, no note is detected).
   //
   // @return Silence threshold in dB. Default: -40.0.
   [[nodiscard]] float silenceThreshold() const;

   // Set the silence threshold.
   //
   // @param threshold Silence threshold in dB. Default: -40.0.
   void setSilenceThreshold(float threshold);

   // Get the minimum time between onsets (in milliseconds).
   //
   // @return Minimum inter-onset interval in milliseconds. Default: 10.0.
   [[nodiscard]] double minIoIMs() const;

   // Set the minimum time between onsets (in milliseconds).
   //
   // @param ms Minimum inter-onset interval in milliseconds. Default: 10.0.
   void setMinIoIMs(double ms);

   // Get the note-off release drop level (in dB).
   //
   // @return Release drop level in dB. Default: 10.0.
   [[nodiscard]] float releaseDropDb() const;

   // Set the note-off release drop level (in dB).
   //
   // When a new note is found, the current level in dB is measured.
   // If the measured level drops under that initial level - release_drop_level,
   // then a note-off will be emitted.
   //
   // @param db Release drop level in dB. Default: 10.0.
   void setReleaseDropDb(float db);

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_NOTES_H

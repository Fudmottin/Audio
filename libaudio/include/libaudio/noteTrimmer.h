/**
 * @file noteTrimmer.h
 * @brief Trim detected notes to musical boundaries.
 *
 * This module refines detected note durations by trimming them to
 * musical boundaries (note off events, silence gaps).
 *
 */

#ifndef LIBAUDIO_NOTETRIMMER_H
#define LIBAUDIO_NOTETRIMMER_H

#include <cstdint>
#include <memory>
#include <vector>

namespace libaudio {

// Forward declarations.
struct Note;
class AudioFileReader;

// ============================================================================
// NoteTrimmer — Trim detected notes to musical boundaries.
//
// Domain context: When note A is held while note B starts, when does A
// end? This module refines note durations by analyzing the audio for
// note-off events, silence gaps, and musical boundaries.
//
// Key design decisions:
// - Uses aubio's note-off detection (release drop level).
// - Configurable silence threshold (default: -40 dB).
// - Configurable minimum note duration (default: 50 ms).
// - Handles legato passages (overlapping notes).
//
// RAII resource management — no external resources.
// ============================================================================
class NoteTrimmer {
 public:
   // Create a NoteTrimmer.
   //
   // @param sampleRate Sample rate of the input signal.
   explicit NoteTrimmer(uint32_t sampleRate);

   // Destructor.
   // RAII — no external resources to release.
   ~NoteTrimmer();

   // Non-copyable (stateful object).
   NoteTrimmer(const NoteTrimmer&) = delete;
   NoteTrimmer& operator=(const NoteTrimmer&) = delete;

   // Movable.
   NoteTrimmer(NoteTrimmer&& other) noexcept;
   NoteTrimmer& operator=(NoteTrimmer&& other) noexcept;

   // Refine note durations by trimming to musical boundaries.
   //
   // Analyzes the audio for note-off events and refines the endTime
   // of each note. Returns a new vector of refined notes.
   //
   // @param notes Input notes (with approximate endTimes).
   // @param reader Audio file reader (already opened).
   // @return Refined notes with accurate endTimes.
   std::vector<Note> refine(const std::vector<Note>& notes,
                            AudioFileReader& reader);

   // Set the silence threshold (in dB) for note-off detection.
   //
   // @param db Silence threshold in dB. Default: -40.0.
   void setSilenceThreshold(float db);

   // Get the current silence threshold (in dB).
   //
   // @return Silence threshold in dB. Default: -40.0.
   [[nodiscard]] float silenceThreshold() const;

   // Set the minimum note duration (in seconds).
   //
   // Notes shorter than this are removed (silence/rest).
   //
   // @param seconds Minimum duration in seconds. Default: 0.05.
   void setMinNoteDuration(double seconds);

   // Get the minimum note duration (in seconds).
   //
   // @return Minimum duration in seconds. Default: 0.05.
   [[nodiscard]] double minNoteDuration() const;

 private:
   // Private implementation — all note trimming logic is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_NOTETRIMMER_H

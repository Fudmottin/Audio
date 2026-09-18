/**
 * @file beat.h
 * @brief Beat tracking via aubio — tempo estimation and beat location.
 *
 * This module wraps aubio's beat tracking. It provides tempo estimation
 * and beat location detection for audio analysis.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#ifndef LIBAUDIO_BEAT_H
#define LIBAUDIO_BEAT_H

#include <cstdint>
#include <memory>
#include <optional>

// ============================================================================
// BeatTracker — Beat tracking via aubio (tempo estimation).
//
// Domain context: Beat tracking estimates the tempo (BPM) and locates
// beat positions in audio. For piano transcription, this is used to
// refine timing and estimate the overall tempo of a piece.
//
// Key design decisions:
// - Default window size: 2048 (consistent with other modules).
// - Returns estimated BPM from the entire analysis so far.
// - Tracks individual beat positions for tempo refinement.
//
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class BeatTracker {
 public:
   // Create a beat tracker.
   //
   // @param bufSize    FFT window size (e.g., 1024, 2048, 4096).
   // @param hopSize    Step size between frames (hop).
   // @param sampleRate Sample rate of the input signal.
   BeatTracker(uint32_t bufSize, uint32_t hopSize, uint32_t sampleRate);

   // Destructor. Frees aubio beat tracking resources.
   // Core Guidelines: RAII — resources are released automatically.
   ~BeatTracker();

   // Core Guidelines: non-copyable (aubio handles are non-copyable).
   BeatTracker(const BeatTracker&) = delete;
   BeatTracker& operator=(const BeatTracker&) = delete;

   // Core Guidelines: movable (aubio handles can be moved).
   BeatTracker(BeatTracker&& other) noexcept;
   BeatTracker& operator=(BeatTracker&& other) noexcept;

   // Analyze a buffer for beat events.
   //
   // @param samples Input audio samples (length must equal bufSize).
   // @return true if a beat was detected at this frame, false otherwise.
   bool detect(const float* samples, uint32_t length);

   // Get the timestamp of the last detected beat (in seconds).
   //
   // @return std::nullopt if no beat was detected.
   [[nodiscard]] std::optional<double> lastBeatTime() const;

   // Get the estimated tempo in BPM.
   //
   // @return Estimated tempo in BPM, or 0 if not enough data.
   [[nodiscard]] double estimatedTempo() const;

   // Get the current tempo estimate (in BPM).
   //
   // @return Current tempo estimate in BPM.
   [[nodiscard]] double currentTempo() const;

   // Get the number of beats detected so far.
   //
   // @return Number of beats detected.
   [[nodiscard]] uint32_t beatCount() const;

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_BEAT_H

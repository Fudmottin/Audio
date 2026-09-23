/**
 * @file rubberband.h
 * @brief Time-stretching and pitch-shifting via rubberband (optional).
 *
 * This module wraps rubberband for time-stretching and pitch-shifting.
 * It is optional: if rubberband is not found, the module is excluded
 * from the build and the RubberbandProcessor class is not available.
 *
 */

#ifndef LIBAUDIO_RUBBERBAND_H
#define LIBAUDIO_RUBBERBAND_H

#include <cstdint>
#include <memory>
#include <vector>

namespace libaudio {

// ============================================================================
// RubberbandProcessor — Time-stretching and pitch-shifting via rubberband.
//
// Domain context: rubberband provides high-quality time-stretching and
// pitch-shifting. Use cases:
// - Normalizing recordings to a consistent sample rate.
// - Pitch-shifting for analysis (e.g., comparing to piano templates).
// - Time-stretching to match a target duration.
//
// Key design decisions:
// - This module is only compiled if rubberband is found.
// - The class is conditionally available via `#ifdef LIBAUDIO_HAS_RUBBERBAND`.
//
// RAII resource management — rubberband resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
#ifdef LIBAUDIO_HAS_RUBBERBAND

class RubberbandProcessor {
 public:
   // Create a rubberband processor.
   //
   // @param sampleRate Sample rate of the input signal.
   // @param channels   Number of channels (1 = mono, 2 = stereo).
   RubberbandProcessor(uint32_t sampleRate, uint32_t channels = 1);

   // Destructor. Frees rubberband resources.
   // RAII — resources are released automatically.
   ~RubberbandProcessor();

   // Non-copyable (rubberband handles are non-copyable).
   RubberbandProcessor(const RubberbandProcessor&) = delete;
   RubberbandProcessor& operator=(const RubberbandProcessor&) = delete;

   // Movable (rubberband handles can be moved).
   RubberbandProcessor(RubberbandProcessor&& other) noexcept;
   RubberbandProcessor& operator=(RubberbandProcessor&& other) noexcept;

   // Process a block of audio samples.
   //
   // @param samples Input samples.
   // @return Processed samples.
   std::vector<float> process(const std::vector<float>& samples);

   // Set the time-stretch factor (1.0 = no change).
   //
   // > 1.0 = slow down, < 1.0 = speed up.
   // @param factor Time-stretch factor.
   void setTimeStretch(float factor);

   // Set the pitch shift (in semitones).
   //
   // Positive = higher, negative = lower.
   // @param semitones Pitch shift in semitones.
   void setPitchShift(float semitones);

   // Set the desired tempo (in BPM) for time-stretching.
   //
   // @param bpm Tempo in BPM.
   void setTempo(float bpm);

   // Flush any remaining samples.
   //
   // @return Remaining processed samples.
   std::vector<float> flush();

   // Check if there are remaining samples to process.
   //
   // @return true if there are remaining samples.
   [[nodiscard]] bool hasRemaining() const;

 private:
   // Private implementation — all rubberband C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_HAS_RUBBERBAND

} // namespace libaudio

#endif // LIBAUDIO_RUBBERBAND_H

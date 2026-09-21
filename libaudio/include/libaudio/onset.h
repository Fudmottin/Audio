/**
 * @file onset.h
 * @brief Note onset detection via aubio — spectral flux, energy, HPS.
 *
 * This module wraps aubio's onset detection algorithms. It provides
 * multiple methods for detecting the start of musical notes (onsets),
 * which is critical for piano transcription.
 *
 * @section onset-methods Available Methods
 *
 * | Method | Description | Piano Suitability |
 * |--------|-------------|-------------------|
 * | specflux | Spectral flux (change in spectral envelope) | ★★★★★ (best for
 * piano) | | energy | Energy-based detection | ★★★☆☆ | | hpsst | Harmonic
 * product spectral flux | ★★★★☆ | | phase | Phase vocoder-based | ★★★★☆ | |
 * combs | Comb filter-based | ★★★☆☆ |
 *
 * Default: "specflux" (spectral flux is most reliable for piano).
 *
 */

#ifndef LIBAUDIO_ONSET_H
#define LIBAUDIO_ONSET_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// ============================================================================
// OnsetDetector — Note onset detection via aubio.
//
// Domain context: Piano notes have a very characteristic attack — a fast
// transient followed by exponential decay. Spectral flux (change in
// spectral envelope between frames) is the most reliable onset signal
// for piano. The default method is "specflux".
//
// Key design decisions:
// - Default method: "specflux" (most reliable for piano).
// - Configurable threshold: higher = fewer false positives.
// - Minimum inter-onset interval (IoI): prevents detecting spurious
//   onsets during the decay phase of a note.
//
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class OnsetDetector {
 public:
   // Create an onset detector.
   //
   // @param method    Detection method: "specflux" (default),
   //                  "energy", "hpsst", "phase", "combs".
   // @param bufSize   FFT window size (e.g., 1024, 2048, 4096).
   // @param hopSize   Step size between frames (hop).
   // @param sampleRate Sample rate of the input signal.
   OnsetDetector(std::string_view method, uint32_t bufSize, uint32_t hopSize,
                 uint32_t sampleRate);

   // Destructor. Frees aubio onset detection resources.
   // RAII — resources are released automatically.
   ~OnsetDetector();

   // Non-copyable (aubio handles are non-copyable).
   OnsetDetector(const OnsetDetector&) = delete;
   OnsetDetector& operator=(const OnsetDetector&) = delete;

   // Movable (aubio handles can be moved).
   OnsetDetector(OnsetDetector&& other) noexcept;
   OnsetDetector& operator=(OnsetDetector&& other) noexcept;

   // Detect an onset in the next hop of audio samples.
   //
   // The internal phase vocoder rotates a bufSize analysis window;
   // each call advances it by one hop. Feed consecutive, non-overlapping
   // chunks of exactly hopSize samples (aubio's own aubioonset CLI uses
   // this contract).
   //
   // @param samples Input audio samples (length must equal hopSize).
   // @return true if an onset was detected in this frame, false otherwise.
   bool detect(const float* samples, uint32_t length);

   // Get the timestamp of the last detected onset (in seconds).
   //
   // @return std::nullopt if no onset was detected.
   [[nodiscard]] std::optional<double> lastOnsetTime() const;

   // Get the confidence of the last onset detection.
   //
   // @return Confidence value (higher = more confident).
   [[nodiscard]] float lastConfidence() const;

   // Set the peak picking threshold (higher = fewer onsets).
   //
   // @param threshold Threshold value. Default: 0.2.
   void setThreshold(float threshold);

   // Get the current threshold.
   //
   // @return Current threshold value.
   [[nodiscard]] float threshold() const;

   // Set the minimum time between onsets (in seconds).
   //
   // @param minIoI Minimum inter-onset interval in seconds. Default: 0.02.
   void setMinIoI(double minIoI);

   // Get the minimum time between onsets.
   //
   // @return Minimum inter-onset interval in seconds.
   [[nodiscard]] double minIoI() const;

   // Set the sample rate of the input signal.
   //
   // Rebuilds the internal onset object (the specflux descriptor and
   // the minioi conversion depend on it). Call after the input file's
   // sample rate is known and before the first detect() call.
   //
   // @param sampleRate Sample rate in Hz.
   void setSampleRate(uint32_t sampleRate);

   // Get the current method name.
   //
   // @return Current detection method name.
   [[nodiscard]] std::string method() const;

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_ONSET_H

/**
 * @file pitch.h
 * @brief Pitch detection via aubio — YIN variants for piano-focused analysis.
 *
 * This module wraps aubio's pitch detection algorithms. It provides
 * multiple algorithms (YIN, YINfft, YINfast, fcomb, mcomb, Schmitt)
 * with configurable confidence scoring and threshold filtering.
 *
 * @section pitch-algorithms Available Algorithms
 *
 * | Algorithm | Accuracy | Speed | Notes |
 * |-----------|----------|-------|-------|
 * | yin | ★★★★☆ | Slow | Full autocorrelation, most accurate |
 * | yinfft | ★★★★☆ | Fast | FFT-optimized YIN, best tradeoff |
 * | yinfast | ★★★☆☆ | Very fast | Approximation, less accurate |
 * | fcomb | ★★★☆☆ | Fast | Harmonic comb filter |
 * | mcomb | ★★★★☆ | Medium | Multiple-comb filter (polyphonic) |
 * | schmitt | ★★☆☆☆ | Very fast | Schmitt trigger (simple, noisy) |
 *
 * Default: YINfft (fast, accurate, good for piano).
 *
 */

#ifndef LIBAUDIO_PITCH_H
#define LIBAUDIO_PITCH_H

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// ============================================================================
// PitchDetector — Pitch detection via aubio (YIN variants, piano-focused).
//
// Domain context: Pitch detection estimates the fundamental frequency
// (pitch) of audio samples. For piano transcription, we need accurate
// pitch detection in the range A0 (MIDI note 21, 27.5 Hz) to C8
// (MIDI note 108, 4186 Hz).
//
// Key design decisions:
// - Default algorithm: YINfft (FFT-optimized YIN, best tradeoff between
//   accuracy and speed for piano).
// - Default window size: 2048 (43 ms at 48 kHz, good balance).
// - Default confidence threshold: 0.5 (moderate sensitivity).
// - Returns MIDI note numbers (float) — callers round to nearest integer.
//
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class PitchDetector {
 public:
   // Create a pitch detector using the YINfft algorithm.
   //
   // @param bufSize   FFT window size (e.g., 1024, 2048, 4096).
   // @param tolerance Tolerance parameter for minima selection [default 0.15].
   //                  Lower = more sensitive, higher = less sensitive.
   PitchDetector(uint32_t bufSize, float tolerance = 0.15f);

   // Destructor. Frees aubio pitch detection resources.
   // RAII — resources are released automatically.
   ~PitchDetector();

   // Non-copyable (aubio handles are non-copyable).
   PitchDetector(const PitchDetector&) = delete;
   PitchDetector& operator=(const PitchDetector&) = delete;

   // Movable (aubio handles can be moved).
   PitchDetector(PitchDetector&& other) noexcept;
   PitchDetector& operator=(PitchDetector&& other) noexcept;

   // Detect pitch from a buffer of audio samples.
   //
   // @param samples  Input audio samples (length must equal bufSize).
   // @return The detected pitch in MIDI note numbers (float),
   //         or 0.0 if no pitch was detected.
   //         Confidence is in [0.0, 1.0].
   std::pair<float, float> detect(const float* samples, uint32_t length);

   // Configure the detection method.
   //
   // Available methods: "yinfft", "yinfast", "fcomb", "schmitt",
   // "default". Note: yin and mcomb require pre-computed FFT and are
   // not supported by this simple wrapper.
   //
   // @param method Algorithm name (case-insensitive).
   void setMethod(std::string_view method);

   // Get the current confidence of the last detection.
   //
   // @return Confidence value in [0.0, 1.0].
   [[nodiscard]] float confidence() const;

   // Get the current confidence threshold.
   //
   // @return Confidence threshold (notes below this return 0.0).
   [[nodiscard]] float confidenceThreshold() const;

   // Set the confidence threshold (notes below this are ignored).
   //
   // @param threshold Threshold value in [0.0, 1.0]. Default: 0.5.
   void setConfidenceThreshold(float threshold);

   // Get the current method name.
   //
   // @return Current algorithm name.
   [[nodiscard]] std::string method() const;

   // Get the window size.
   //
   // @return Window size (must be a power of 2).
   [[nodiscard]] uint32_t bufSize() const;

   // Get the step size (hop size).
   //
   // @return Hop size (default: bufSize / 4).
   [[nodiscard]] uint32_t hopSize() const;

   // Set the step size (hop size).
   //
   // @param hopSize Hop size (default: bufSize / 4).
   void setHopSize(uint32_t hopSize);

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_PITCH_H

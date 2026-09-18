/**
 * @file temporal.h
 * @brief Time-domain processing via aubio — resampling, filtering.
 *
 * This module wraps aubio's temporal processing (resampling, filtering).
 * It provides pre-processing utilities for audio analysis.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#ifndef LIBAUDIO_TEMPORAL_H
#define LIBAUDIO_TEMPORAL_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// TemporalProcessor — Time-domain processing via aubio (resampling, filtering).
//
// Domain context: Time-domain processing handles pre-processing tasks:
// - High-pass filter to remove sub-bass rumble (below A0 = 27.5 Hz).
// - Resample to a consistent sample rate (48 kHz).
// - Mono downmix: average stereo channels for analysis.
//
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class TemporalProcessor {
 public:
   // Create a temporal processor.
   //
   // @param sampleRate Sample rate of the input signal.
   explicit TemporalProcessor(uint32_t sampleRate);

   // Destructor. Frees aubio temporal processing resources.
   // Core Guidelines: RAII — resources are released automatically.
   ~TemporalProcessor();

   // Core Guidelines: non-copyable (aubio handles are non-copyable).
   TemporalProcessor(const TemporalProcessor&) = delete;
   TemporalProcessor& operator=(const TemporalProcessor&) = delete;

   // Core Guidelines: movable (aubio handles can be moved).
   TemporalProcessor(TemporalProcessor&& other) noexcept;
   TemporalProcessor& operator=(TemporalProcessor&& other) noexcept;

   // Resample audio to a different sample rate.
   //
   // @param samples Input samples.
   // @param targetSampleRate Desired sample rate.
   // @return Resampled samples.
   std::vector<float> resample(const std::vector<float>& samples,
                               uint32_t targetSampleRate);

   // Apply a low-pass filter.
   //
   // @param samples Input samples.
   // @param cutoffHz Cutoff frequency in Hz.
   // @return Filtered samples.
   std::vector<float> lowPass(const std::vector<float>& samples,
                              float cutoffHz);

   // Apply a high-pass filter.
   //
   // @param samples Input samples.
   // @param cutoffHz Cutoff frequency in Hz.
   // @return Filtered samples.
   std::vector<float> highPass(const std::vector<float>& samples,
                               float cutoffHz);

   // Apply an A-weighting filter (for perceived loudness).
   //
   // @param samples Input samples.
   // @return Weighted samples.
   std::vector<float> aWeighting(const std::vector<float>& samples);

   // Apply a C-weighting filter.
   //
   // @param samples Input samples.
   // @return Weighted samples.
   std::vector<float> cWeighting(const std::vector<float>& samples);

   // Compute the Biquad filter coefficients.
   //
   // @param filterType Filter type ("lowpass", "highpass", "bandpass").
   // @param cutoffHz Cutoff frequency in Hz.
   // @param q Quality factor.
   // @return Biquad coefficients (3 coefficients: a0, a1, a2).
   std::vector<std::vector<float>> biquadCoefficients(
      std::string_view filterType, float cutoffHz, float q);

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_TEMPORAL_H

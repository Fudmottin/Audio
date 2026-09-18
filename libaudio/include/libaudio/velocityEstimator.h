/**
 * @file velocityEstimator.h
 * @brief Estimate note velocity from audio amplitude.
 *
 * This module estimates note velocity from audio amplitude (RMS energy).
 * It converts audio amplitude to MIDI velocity (0–127).
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#ifndef LIBAUDIO_VELOCITYESTIMATOR_H
#define LIBAUDIO_VELOCITYESTIMATOR_H

#include <cstdint>
#include <memory>
#include <vector>

// Forward declaration (from hir.h).
struct Note;

// ============================================================================
// VelocityEstimator — Estimate note velocity from audio amplitude.
//
// Domain context: Real pianos have multiple velocity layers (often 8–32+)
// with different recordings per layer. A basic MIDI file uses a single
// layer with velocity as a volume cue. This module estimates velocity
// from the RMS energy of the note segment.
//
// Key design decisions:
// - Uses RMS energy of the note segment during analysis.
// - Maps RMS energy to MIDI velocity (0–127).
// - Configurable normalization range (default: -40 dB to 0 dB → 0–127).
//
// Core Guidelines: RAII resource management — no external resources.
// ============================================================================
class VelocityEstimator {
 public:
   // Create a VelocityEstimator.
   //
   // @param sampleRate Sample rate of the input signal.
   explicit VelocityEstimator(uint32_t sampleRate);

   // Destructor.
   // Core Guidelines: RAII — no external resources to release.
   ~VelocityEstimator();

   // Core Guidelines: non-copyable (stateful object).
   VelocityEstimator(const VelocityEstimator&) = delete;
   VelocityEstimator& operator=(const VelocityEstimator&) = delete;

   // Core Guidelines: movable.
   VelocityEstimator(VelocityEstimator&& other) noexcept;
   VelocityEstimator& operator=(VelocityEstimator&& other) noexcept;

   // Estimate velocity from a buffer of audio samples.
   //
   // Computes the RMS energy of the buffer and maps it to MIDI
   // velocity (0–127). Uses the configured normalization range.
   //
   // @param samples Input audio samples.
   // @param length Number of samples.
   // @return Estimated velocity (0–127).
   uint8_t estimate(const float* samples, uint32_t length);

   // Set the normalization range (in dB).
   //
   // @param minDb Minimum RMS energy in dB (maps to velocity 0).
   // @param maxDb Maximum RMS energy in dB (maps to velocity 127).
   void setNormalizationRange(float minDb, float maxDb);

   // Get the current normalization range (in dB).
   //
   // @return std::pair of (minDb, maxDb).
   [[nodiscard]] std::pair<float, float> normalizationRange() const;

 private:
   // Private implementation — all velocity estimation logic is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_VELOCITYESTIMATOR_H

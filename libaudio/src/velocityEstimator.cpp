/**
 * @file velocityEstimator.cpp
 * @brief Implementation of VelocityEstimator — estimate note velocity
 *        from audio amplitude.
 *
 * This module estimates note velocity from audio amplitude (RMS energy).
 * It converts audio amplitude to MIDI velocity (0–127).
 *
 */

#include <algorithm>
#include <cmath>
#include <libaudio/velocityEstimator.h>

// ============================================================================
// VelocityEstimator::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All velocity estimation logic is isolated here.
//
// RAII — no external resources to manage.
// ============================================================================
struct VelocityEstimator::Impl {
   uint32_t sampleRate;
   float minDb = -40.0f;
   float maxDb = 0.0f;

   Impl(uint32_t sampleRate)
      : sampleRate(sampleRate) {}
   ~Impl() = default;

   // Compute RMS energy in dB (relative to max).
   float rmsToDb(float rms) const {
      // Convert RMS energy to dB scale.
      // 0 dB = max RMS (silence = -infinity dB).
      if (rms <= 0.0f) {
         return -100.0f; // Minimum dB (silence).
      }
      return 20.0f * std::log10(rms);
   }

   // Map dB value to MIDI velocity (0–127).
   uint8_t dbToVelocity(float db) const {
      // Map dB to MIDI velocity (0–127).
      // Linear mapping: velocity = (db - minDb) / (maxDb - minDb) * 127.
      float normalized = (db - minDb) / (maxDb - minDb);
      normalized = std::max(0.0f, std::min(1.0f, normalized));
      return static_cast<uint8_t>(std::round(normalized * 127.0f));
   }
};

// ============================================================================
// VelocityEstimator implementation
// RAII resource management — no external resources.
// ============================================================================

VelocityEstimator::VelocityEstimator(uint32_t sampleRate)
   : impl_(std::make_unique<Impl>(sampleRate)) {
   // Constructor.
}

VelocityEstimator::~VelocityEstimator() = default;

VelocityEstimator::VelocityEstimator(VelocityEstimator&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ =
      std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
}

VelocityEstimator&
VelocityEstimator::operator=(VelocityEstimator&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ =
         std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
   }
   return *this;
}

uint8_t VelocityEstimator::estimate(const float* samples, uint32_t length) {
   // Estimate velocity from a buffer of audio samples.

   if (samples == nullptr || length == 0 || impl_ == nullptr) {
      return 0;
   }

   // Compute RMS energy.
   double sumSquares = 0.0;
   for (uint32_t i = 0; i < length; ++i) {
      sumSquares += static_cast<double>(samples[i]) * samples[i];
   }

   float rms = static_cast<float>(std::sqrt(sumSquares / length));

   // Convert RMS to dB and then to MIDI velocity.
   float db = impl_->rmsToDb(rms);
   return impl_->dbToVelocity(db);
}

void VelocityEstimator::setNormalizationRange(float minDb, float maxDb) {
   // Set the normalization range (in dB).

   if (impl_) {
      impl_->minDb = std::min(minDb, maxDb);
      impl_->maxDb = std::max(minDb, maxDb);
   }
}

std::pair<float, float> VelocityEstimator::normalizationRange() const {
   // Return the current normalization range.

   if (impl_) {
      return {impl_->minDb, impl_->maxDb};
   }
   return {-40.0f, 0.0f};
}

/**
 * @file beat.cpp
 * @brief Implementation of BeatTracker — beat tracking via aubio.
 *
 * This module wraps aubio's beat tracking. It provides tempo estimation
 * and beat location detection for audio analysis.
 *
 */

#include <libaudio/beat.h>
// clang-format off
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/tempo/tempo.h>
// clang-format on
#include <stdexcept>

namespace libaudio {

// ============================================================================
// BeatTracker::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct BeatTracker::Impl {
   aubio_tempo_t* tracker = nullptr;
   fvec_t* inputBuffer = nullptr;
   uint32_t bufSize;
   uint32_t hopSize;
   uint32_t sampleRate;
   uint32_t currentFrame = 0;
   uint32_t beatCount_ = 0;
   double lastBeatTime = -1.0;
   double estimatedTempo_ = 0.0;
   double currentTempo_ = 0.0;

   ~Impl() {
      if (tracker) del_aubio_tempo(tracker);
      if (inputBuffer) del_fvec(inputBuffer);
   }
};

// ============================================================================
// BeatTracker implementation
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

BeatTracker::BeatTracker(uint32_t bufSize, uint32_t hopSize,
                         uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   // Create the aubio beat tracker.

   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->sampleRate = sampleRate;

   // Create the tempo (beat) tracker.
   impl_->tracker = new_aubio_tempo("default", bufSize, hopSize, sampleRate);

   // Allocate input buffer.
   impl_->inputBuffer = new_fvec(bufSize);
}

BeatTracker::~BeatTracker() = default;

BeatTracker::BeatTracker(BeatTracker&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

BeatTracker& BeatTracker::operator=(BeatTracker&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

bool BeatTracker::detect(const float* samples, uint32_t length) {
   // Analyze a buffer for beat events.

   if (impl_ == nullptr || impl_->tracker == nullptr) {
      return false;
   }

   if (length != impl_->bufSize) {
      throw std::invalid_argument("Sample length must equal buffer size");
   }

   // Copy samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));

   // Run beat detection.
   // aubio_tempo_do takes 3 args: (tracker, input, output_fvec)
   // and returns void. The output fvec holds the beat decision.
   fvec_t* tempoOutput = new_fvec(1);
   aubio_tempo_do(impl_->tracker, impl_->inputBuffer, tempoOutput);

   // Extract results.
   bool beatDetected = tempoOutput->data[0] != 0.0f;
   del_fvec(tempoOutput);

   if (beatDetected) {
      impl_->beatCount_++;
      impl_->lastBeatTime = static_cast<double>(impl_->currentFrame) *
                            impl_->hopSize / impl_->sampleRate;
      impl_->estimatedTempo_ = aubio_tempo_get_bpm(impl_->tracker);
      impl_->currentTempo_ = impl_->estimatedTempo_;
   }

   impl_->currentFrame += length;
   return beatDetected;
}

std::optional<double> BeatTracker::lastBeatTime() const {
   // Return the timestamp of the last detected beat.

   if (impl_ && impl_->lastBeatTime >= 0.0) {
      return impl_->lastBeatTime;
   }
   return std::nullopt;
}

double BeatTracker::estimatedTempo() const {
   // Simple accessor.
   return impl_ ? impl_->estimatedTempo_ : 0.0;
}

double BeatTracker::currentTempo() const {
   // Simple accessor.
   return impl_ ? impl_->currentTempo_ : 0.0;
}

uint32_t BeatTracker::beatCount() const {
   // Simple accessor.
   return impl_ ? impl_->beatCount_ : 0;
}

} // namespace libaudio

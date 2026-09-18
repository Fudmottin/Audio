/**
 * @file onset.cpp
 * @brief Implementation of OnsetDetector — note onset detection via aubio.
 *
 * This module wraps aubio's onset detection algorithms. It provides
 * multiple methods for detecting the start of musical notes (onsets),
 * which is critical for piano transcription.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 * @see lode/libaudio/decisions.md — Default parameters
 */

#include <libaudio/onset.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/onset/onset.h>
#include <stdexcept>

// ============================================================================
// OnsetDetector::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// Core Guidelines: RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct OnsetDetector::Impl {
   aubio_onset_t* detector = nullptr;
   fvec_t* inputBuffer = nullptr;
   std::string method;
   uint32_t bufSize;
   uint32_t hopSize;
   uint32_t sampleRate;
   float threshold = 0.2f;
   double minIoI = 0.02;  // 20 ms default
   double lastOnsetTime = -1.0;
   float lastConfidence = 0.0f;
   uint32_t currentFrame = 0;

   ~Impl() {
      if (detector) del_aubio_onset(detector);
      if (inputBuffer) del_fvec(inputBuffer);
   }
};

// ============================================================================
// OnsetDetector implementation
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

OnsetDetector::OnsetDetector(std::string_view method, uint32_t bufSize,
                             uint32_t hopSize, uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   // Core Guidelines: create the aubio onset detector.

   impl_->method = std::string(method);
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->sampleRate = sampleRate;

   // Core Guidelines: create the onset detector with the specified method.
   impl_->detector = new_aubio_onset(impl_->method.c_str(), bufSize,
                                     hopSize, sampleRate);

   // Core Guidelines: allocate input buffer.
   impl_->inputBuffer = new_fvec(bufSize);

   // Core Guidelines: set the threshold and minimum IoI.
   // Note: aubio_onset_set_minioi takes uint_t (frame count), not float.
   aubio_onset_set_threshold(impl_->detector, impl_->threshold);
   aubio_onset_set_minioi(impl_->detector,
                          static_cast<uint_t>(impl_->minIoI * sampleRate));
}

OnsetDetector::~OnsetDetector() = default;

OnsetDetector::OnsetDetector(OnsetDetector&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

OnsetDetector& OnsetDetector::operator=(OnsetDetector&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

bool OnsetDetector::detect(const float* samples, uint32_t length) {
   // Core Guidelines: detect onsets in a buffer of audio samples.

   if (impl_ == nullptr || impl_->detector == nullptr) {
      return false;
   }

   if (length != impl_->bufSize) {
      throw std::invalid_argument(
         "Sample length must equal buffer size");
   }

   // Core Guidelines: copy samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));

   // Core Guidelines: run onset detection.
   // aubio_onset_do takes 3 args: (detector, input, output_fvec)
   // and returns void. The output fvec holds the onset decision.
   fvec_t* output = new_fvec(impl_->bufSize);
   aubio_onset_do(impl_->detector, impl_->inputBuffer, output);

   // Core Guidelines: extract the onset decision from the output buffer.
   // aubio_onset_get_silence returns the silence threshold.
   // The output buffer's data[0] is non-zero if an onset was detected.
   bool detected = output->data[0] != 0.0f;
   del_fvec(output);

   if (detected) {
      impl_->lastOnsetTime =
         static_cast<double>(impl_->currentFrame) * impl_->hopSize /
         impl_->sampleRate;
   } else {
      impl_->lastOnsetTime = -1.0;
   }

   impl_->currentFrame += length;
   return detected;
}

std::optional<double> OnsetDetector::lastOnsetTime() const {
   // Core Guidelines: return the timestamp of the last detected onset.

   if (impl_ && impl_->lastOnsetTime >= 0.0) {
      return impl_->lastOnsetTime;
   }
   return std::nullopt;
}

float OnsetDetector::lastConfidence() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->lastConfidence : 0.0f;
}

void OnsetDetector::setThreshold(float threshold) {
   // Core Guidelines: set the peak picking threshold.

   if (impl_ && impl_->detector) {
      impl_->threshold = threshold;
      aubio_onset_set_threshold(impl_->detector, threshold);
   }
}

float OnsetDetector::threshold() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->threshold : 0.2f;
}

void OnsetDetector::setMinIoI(double minIoI) {
   // Core Guidelines: set the minimum time between onsets.
   // Core Guidelines: aubio_onset_set_minioi takes uint_t (frame count),
   // not float — compute the number of frames from the time.

   if (impl_ && impl_->detector) {
      impl_->minIoI = minIoI;
      aubio_onset_set_minioi(impl_->detector,
                             static_cast<uint_t>(impl_->minIoI * impl_->sampleRate));
   }
}

double OnsetDetector::minIoI() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->minIoI : 0.02;
}

std::string OnsetDetector::method() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->method : "specflux";
}

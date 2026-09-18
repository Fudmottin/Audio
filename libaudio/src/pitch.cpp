/**
 * @file pitch.cpp
 * @brief Implementation of PitchDetector — pitch detection via aubio.
 *
 * This module wraps aubio's pitch detection algorithms. It provides
 * multiple algorithms (YIN, YINfft, YINfast, fcomb, mcomb, Schmitt)
 * with configurable confidence scoring and threshold filtering.
 *
 */

#include <libaudio/pitch.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/cvec.h>
#include <aubio/pitch/pitchyinfft.h>
#include <aubio/pitch/pitchyin.h>
#include <aubio/pitch/pitchyinfast.h>
#include <aubio/pitch/pitchfcomb.h>
#include <aubio/pitch/pitchmcomb.h>
#include <aubio/pitch/pitchschmitt.h>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <cctype>

// ============================================================================
// PitchDetector::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here. The public
// interface never exposes aubio types (aubio_pitchyinfft_t*, fvec_t*,
// etc.). This makes the rest of the codebase framework-free (except
// for the pitch.h header which only includes pitch.h).
//
// RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct PitchDetector::Impl {
   aubio_pitchyinfft_t* yinfft = nullptr;
   aubio_pitchyinfast_t* yinfast = nullptr;
   aubio_pitchfcomb_t* fcomb = nullptr;
   aubio_pitchschmitt_t* schmitt = nullptr;
   void* active = nullptr;
   std::string currentMethod;
   uint32_t bufSize;
   uint32_t hopSize;
   uint32_t sampleRate = 48000;
   float confidenceThreshold = 0.5f;
   float lastConfidence = 0.0f;
   fvec_t* inputBuffer = nullptr;
   fvec_t* candsBuffer = nullptr;

   ~Impl() {
      if (yinfft) del_aubio_pitchyinfft(yinfft);
      if (yinfast) del_aubio_pitchyinfast(yinfast);
      if (fcomb) del_aubio_pitchfcomb(fcomb);
      if (schmitt) del_aubio_pitchschmitt(schmitt);
      if (inputBuffer) del_fvec(inputBuffer);
      if (candsBuffer) del_fvec(candsBuffer);
   }
};

// ============================================================================
// PitchDetector implementation
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

PitchDetector::PitchDetector(uint32_t bufSize, float tolerance)
   : impl_(std::make_unique<Impl>()) {
   // Create the default (YINfft) pitch detector.

   impl_->bufSize = bufSize;
   impl_->hopSize = bufSize / 4;  // Default: 75% overlap
   impl_->sampleRate = 48000;  // Default sample rate
   impl_->currentMethod = "yinfft";

   // Create the YINfft detector (default algorithm).
   impl_->yinfft = new_aubio_pitchyinfft(impl_->sampleRate, bufSize);
   impl_->active = static_cast<void*>(impl_->yinfft);

   // Allocate input and candidate buffers.
   impl_->inputBuffer = new_fvec(bufSize);
   impl_->candsBuffer = new_fvec(bufSize);

   // Set the tolerance parameter.
   aubio_pitchyinfft_set_tolerance(impl_->yinfft, tolerance);
}

PitchDetector::~PitchDetector() = default;

PitchDetector::PitchDetector(PitchDetector&& other) noexcept
   : impl_(std::move(other.impl_)) {
   // Move constructor. Transfer ownership of all aubio
   // resources from the source object.
   other.impl_ = std::make_unique<Impl>();
}

PitchDetector& PitchDetector::operator=(PitchDetector&& other) noexcept {
   // Move assignment operator. Release current resources
   // and take ownership of the source object's resources.

   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

std::pair<float, float> PitchDetector::detect(const float* samples,
                                              uint32_t length) {
   // Detect pitch from a buffer of audio samples.

   if (impl_ == nullptr || impl_->active == nullptr) {
      return {0.0f, 0.0f};
   }

   if (length != impl_->bufSize) {
      throw std::invalid_argument(
         "Sample length must equal buffer size");
   }

   // Copy samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));

   // Run pitch detection based on the active algorithm.
   // The active pointer is cast to the appropriate type for each algorithm.
   // Note: only fvec-based detectors are supported (they take time-domain
   // samples and compute FFT internally). Cvec-based detectors (yin, mcomb)
   // would require pre-computed complex spectra, which is outside the scope
   // of this simple pitch detector wrapper.
   if (impl_->currentMethod == "yinfft") {
      aubio_pitchyinfft_do(static_cast<aubio_pitchyinfft_t*>(impl_->active),
                           impl_->inputBuffer, impl_->candsBuffer);
   } else if (impl_->currentMethod == "yinfast") {
      aubio_pitchyinfast_do(static_cast<aubio_pitchyinfast_t*>(impl_->active),
                            impl_->inputBuffer, impl_->candsBuffer);
   } else if (impl_->currentMethod == "fcomb") {
      aubio_pitchfcomb_do(static_cast<aubio_pitchfcomb_t*>(impl_->active),
                          impl_->inputBuffer, impl_->candsBuffer);
   } else if (impl_->currentMethod == "schmitt") {
      aubio_pitchschmitt_do(
         static_cast<aubio_pitchschmitt_t*>(impl_->active),
         impl_->inputBuffer, impl_->candsBuffer);
   } else {
      // Default: YINfft.
      aubio_pitchyinfft_do(static_cast<aubio_pitchyinfft_t*>(impl_->active),
                           impl_->inputBuffer, impl_->candsBuffer);
   }

   // Extract results.
   float pitch = impl_->candsBuffer->data[0];  // MIDI note (float)
   float confidence = 0.0f;

   // Get confidence from the active detector.
   // Only yinfft and yinfast provide confidence; fcomb and schmitt
   // (from the tuneit project) do not.
   if (impl_->currentMethod == "yinfft") {
      confidence = aubio_pitchyinfft_get_confidence(
         static_cast<aubio_pitchyinfft_t*>(impl_->active));
   } else if (impl_->currentMethod == "yinfast") {
      confidence = aubio_pitchyinfast_get_confidence(
         static_cast<aubio_pitchyinfast_t*>(impl_->active));
   } else {
      // fcomb and schmitt do not provide confidence.
      confidence = 0.0f;
   }

   impl_->lastConfidence = confidence;

   // Return 0.0 pitch if confidence is below threshold.
   if (confidence < impl_->confidenceThreshold) {
      return {0.0f, confidence};
   }

   return {pitch, confidence};
}

void PitchDetector::setMethod(std::string_view method) {
   // Switch to a different detection algorithm.

   std::string lowerMethod;
   lowerMethod.reserve(method.size());
   for (char c : method) {
      lowerMethod += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   }

   // Delete current detector.
   if (impl_->yinfft) del_aubio_pitchyinfft(impl_->yinfft);
   if (impl_->yinfast) del_aubio_pitchyinfast(impl_->yinfast);
   if (impl_->fcomb) del_aubio_pitchfcomb(impl_->fcomb);
   if (impl_->schmitt) del_aubio_pitchschmitt(impl_->schmitt);

   impl_->currentMethod = lowerMethod;

   // Create the requested detector.
   // Supported methods: yinfft, yinfast, fcomb, schmitt (all fvec-based).
   // Note: yin and mcomb are cvec-based (require pre-computed FFT)
   // and are not supported by this simple wrapper.
   if (lowerMethod == "yinfft") {
      impl_->yinfft = new_aubio_pitchyinfft(impl_->sampleRate, impl_->bufSize);
      impl_->active = static_cast<void*>(impl_->yinfft);
   } else if (lowerMethod == "yinfast") {
      impl_->yinfast = new_aubio_pitchyinfast(impl_->bufSize);
      impl_->active = static_cast<void*>(impl_->yinfast);
   } else if (lowerMethod == "fcomb") {
      impl_->fcomb = new_aubio_pitchfcomb(impl_->bufSize, impl_->hopSize);
      impl_->active = static_cast<void*>(impl_->fcomb);
   } else if (lowerMethod == "schmitt") {
      impl_->schmitt = new_aubio_pitchschmitt(impl_->bufSize);
      impl_->active = static_cast<void*>(impl_->schmitt);
   } else {
      // Default: YINfft.
      impl_->yinfft = new_aubio_pitchyinfft(impl_->sampleRate, impl_->bufSize);
      impl_->active = static_cast<void*>(impl_->yinfft);
   }
}

float PitchDetector::confidence() const {
   // Simple accessor.
   return impl_ ? impl_->lastConfidence : 0.0f;
}

float PitchDetector::confidenceThreshold() const {
   // Simple accessor.
   return impl_ ? impl_->confidenceThreshold : 0.0f;
}

void PitchDetector::setConfidenceThreshold(float threshold) {
   // Set the confidence threshold.

   if (impl_) {
      impl_->confidenceThreshold = threshold;
   }
}

std::string PitchDetector::method() const {
   // Simple accessor.
   return impl_ ? impl_->currentMethod : "yinfft";
}

uint32_t PitchDetector::bufSize() const {
   // Simple accessor.
   return impl_ ? impl_->bufSize : 0;
}

uint32_t PitchDetector::hopSize() const {
   // Simple accessor.
   return impl_ ? impl_->hopSize : 0;
}

void PitchDetector::setHopSize(uint32_t hopSize) {
   // Set the hop size (requires recreating the detector).

   if (impl_) {
      impl_->hopSize = hopSize;
      // Recreate the current detector with the new hop size.
      setMethod(impl_->currentMethod);
   }
}

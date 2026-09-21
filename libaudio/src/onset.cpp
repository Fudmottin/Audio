/**
 * @file onset.cpp
 * @brief Implementation of OnsetDetector — note onset detection via aubio.
 *
 * This module wraps aubio's onset detection algorithms. It provides
 * multiple methods for detecting the start of musical notes (onsets),
 * which is critical for piano transcription.
 *
 * @section aubio-contract The aubio onset object contract
 *
 * An aubio_onset_t combines a phase vocoder (win_s = bufSize, hop_s =
 * hopSize) with an onset novelty function and a peak picker. Its contract:
 *
 * 1. `aubio_onset_do(o, in, out)` expects `in` to be **hop_size** samples
 *    long (NOT buf_size). The internal phase vocoder rotates the analysis
 *    buffer to keep it filled, so feeding it buf_size samples corrupts its
 *    internal state and makes the novelty function meaningless.
 * 2. `out` must be a one-sample vector: 0.0 when no onset, `1 + a`
 *    (a in [0,1]) when one is found.
 * 3. Detected onsets are then queried with `aubio_onset_get_last_s()`; the
 *    object internally applies the silence threshold and the minimum
 *    inter-onset interval (minioi).
 *
 */

#include <libaudio/onset.h>
// clang-format off
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/onset/onset.h>
// clang-format on
#include <algorithm>
#include <cstring>
#include <stdexcept>

// ============================================================================
// OnsetDetector::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct OnsetDetector::Impl {
   aubio_onset_t* detector = nullptr;
   // Input buffer sized to the HOP, not the window: aubio_onset_do()
   // expects a hop-size frame (see file header).
   fvec_t* inputBuffer = nullptr;
   // One-sample output: 0 when no onset, 1+a when an onset is found.
   fvec_t* outputBuffer = nullptr;
   std::string method;
   uint32_t bufSize;
   uint32_t hopSize;
   uint32_t sampleRate;
   float threshold = 0.2f;
   // Minimum time between two consecutive onsets (in seconds).
   //
   // Domain context: the onset detector's internal peak picker rejects
   // detections closer than this. aubio's own aubioonset CLI defaults
   // to 0.012 s (12 ms) — short enough that the natural decay tail of a
   // struck piano note does not produce a second "onset", but long
   // enough that two genuinely different notes (which are at least one
   // beat apart in our test corpus) are never suppressed.
   double minIoI = 0.012;
   double lastOnsetTime = -1.0;
   float lastConfidence = 0.0f;
   // Latch edge detector: `aubio_onset_get_last()` reports the total
   // sample count of the last *accepted* onset and keeps that value
   // until a newer one arrives. We compare each call's value against
   // the previous call's value to detect the *edge* (a new onset on
   // this hop) rather than the latched state.
   uint_t prevOnsetSample = 0;
   bool primed = false;

   // (Re)create the aubio onset object from the stored parameters.
   // See the function header below for the domain context.
   void buildDetector();

   ~Impl() {
      if (detector) del_aubio_onset(detector);
      if (inputBuffer) del_fvec(inputBuffer);
      if (outputBuffer) del_fvec(outputBuffer);
   }
};

// ============================================================================
// OnsetDetector implementation
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

OnsetDetector::OnsetDetector(std::string_view method, uint32_t bufSize,
                             uint32_t hopSize, uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   impl_->method = std::string(method);
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->sampleRate = sampleRate;

   impl_->buildDetector();

   // Allocate a hop-sized input buffer and a one-sample output buffer
   // (both per the aubio_onset_do contract in the file header).
   impl_->inputBuffer = new_fvec(hopSize);
   impl_->outputBuffer = new_fvec(1);

   // Set the peak-picking threshold and the minimum inter-onset interval.
   aubio_onset_set_threshold(impl_->detector, impl_->threshold);
   aubio_onset_set_minioi_s(impl_->detector, static_cast<smpl_t>(impl_->minIoI));

   // Last-detection state (no detection yet).
   impl_->lastOnsetTime = -1.0;
   impl_->lastConfidence = 0.0f;
   impl_->prevOnsetSample = 0;
}

// ============================================================================
// Impl::buildDetector — (re)create the aubio onset object.
//
// Domain context: aubio_onset_do() expects hop-sized input and derives the
// minioi frame count from the sample rate, so any parameter change
// (method, window, hop, sample rate) requires a fresh object. The caller
// is responsible for re-applying threshold/minIoI (see setThreshold,
// setMinIoI, setSampleRate).
// ============================================================================
void OnsetDetector::Impl::buildDetector() {
   if (detector) del_aubio_onset(detector);
   detector = new_aubio_onset(method.c_str(), bufSize, hopSize, sampleRate);
   if (detector == nullptr) {
      throw std::runtime_error(
         "Could not create onset detector ('" + method + "')");
   }
   // A rebuilt detector has no detections yet; reset the latch.
   prevOnsetSample = 0;
   primed = false;
   lastOnsetTime = -1.0;
   lastConfidence = 0.0f;
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
   // Detect an onset in the next hop of audio samples.
   //
   // The input must be exactly hopSize samples: the internal phase
   // vocoder rotates its analysis buffer to keep it filled with the most
   // recent window of data, and only accepts hop-sized increments.

   if (impl_ == nullptr || impl_->detector == nullptr) {
      return false;
   }

   if (length != impl_->hopSize) {
      throw std::invalid_argument(
         "Sample length must equal hop size (onset detector contract)");
   }

   // Copy samples into aubio's fvec_t and run the detector.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->hopSize * sizeof(float));
   aubio_onset_do(impl_->detector, impl_->inputBuffer, impl_->outputBuffer);

   // Edge detection: get_last() latches the sample count of the last
   // *accepted* onset (it stays > 0 after the first detection). A new
   // onset on this hop is signalled only by a strictly larger value.
   //
   // First-call priming: before any accepted onset, get_last() returns
   // 0 but the internal *candidate* state (min_sample/min_time) can be
   // left holding a stale value by the aubio peak picker, which makes
   // a second 0 return look like a new detection. Prime the latch on
   // the first call: accept the candidate if one exists, otherwise
   // record the current value and wait for a larger one.
   uint_t lastSample = aubio_onset_get_last(impl_->detector);
   if (!impl_->primed) {
      impl_->primed = true;
      impl_->prevOnsetSample = lastSample;
      if (lastSample == 0) return false;
   }
   if (lastSample > impl_->prevOnsetSample) {
      impl_->lastOnsetTime =
         static_cast<double>(lastSample) / static_cast<double>(impl_->sampleRate);
      impl_->lastConfidence = impl_->outputBuffer->data[0];
      impl_->prevOnsetSample = lastSample;
      return true;
   }

   // No onset in this frame.
   impl_->lastOnsetTime = -1.0;
   impl_->lastConfidence = 0.0f;
   return false;
}

std::optional<double> OnsetDetector::lastOnsetTime() const {
   // Return the timestamp of the last detected onset.
   if (impl_ && impl_->lastOnsetTime >= 0.0) {
      return impl_->lastOnsetTime;
   }
   return std::nullopt;
}

float OnsetDetector::lastConfidence() const {
   return impl_ ? impl_->lastConfidence : 0.0f;
}

void OnsetDetector::setThreshold(float threshold) {
   if (impl_ && impl_->detector) {
      impl_->threshold = threshold;
      aubio_onset_set_threshold(impl_->detector, impl_->threshold);
   }
}

float OnsetDetector::threshold() const {
   return impl_ ? impl_->threshold : 0.2f;
}

void OnsetDetector::setMinIoI(double minIoI) {
   // Set the minimum time between onsets (in seconds) via the
   // seconds-based setter (a plain float value, in seconds).
   if (impl_ && impl_->detector) {
      impl_->minIoI = minIoI;
      aubio_onset_set_minioi_s(impl_->detector,
                               static_cast<smpl_t>(minIoI));
   }
}

double OnsetDetector::minIoI() const {
   return impl_ ? impl_->minIoI : 0.012;
}

void OnsetDetector::setSampleRate(uint32_t sampleRate) {
   // Rebuild the internal aubio onset object with the new sample rate.
   //
   // Domain context: new_aubio_onset() derives the specflux descriptor
   // and the minioi frame count from the sample rate, so a rate change
   // requires a fresh object. This lets a wrapper constructed with a
   // placeholder rate be finalized once the input file is known.

   if (!impl_ || sampleRate == impl_->sampleRate) {
      return;
   }
   impl_->sampleRate = sampleRate;
   impl_->buildDetector();
   aubio_onset_set_threshold(impl_->detector, impl_->threshold);
   aubio_onset_set_minioi_s(impl_->detector, static_cast<smpl_t>(impl_->minIoI));
}

std::string OnsetDetector::method() const {
   return impl_ ? impl_->method : "specflux";
}

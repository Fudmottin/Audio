/**
 * @file temporal.cpp
 * @brief Implementation of TemporalProcessor — time-domain processing
 *        via aubio (resampling, filtering).
 *
 * This module wraps aubio's temporal processing (resampling, filtering).
 * It provides pre-processing utilities for audio analysis.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#include <libaudio/temporal.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/lvec.h>
#include <aubio/temporal/resampler.h>
#include <aubio/temporal/filter.h>
#include <aubio/temporal/biquad.h>
#include <aubio/temporal/a_weighting.h>
#include <aubio/temporal/c_weighting.h>
#include <stdexcept>

// Core Guidelines: libsamplerate converter type constants.
// SRC_SINC_best_quality is the highest quality resampler.
#ifndef SRC_SINC_best_quality
#define SRC_SINC_best_quality 0
#endif

// ============================================================================
// TemporalProcessor::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// Core Guidelines: RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct TemporalProcessor::Impl {
   aubio_resampler_t* resampler = nullptr;
   aubio_filter_t* filter = nullptr;
   aubio_filter_t* biquad = nullptr;
   aubio_filter_t* aWeight = nullptr;
   aubio_filter_t* cWeight = nullptr;
   uint32_t sampleRate;
   uint32_t channels;
   fvec_t* inputBuffer = nullptr;

   ~Impl() {
      if (resampler) del_aubio_resampler(resampler);
      if (filter) del_aubio_filter(filter);
      if (biquad) del_aubio_filter(biquad);
      if (aWeight) del_aubio_filter(aWeight);
      if (cWeight) del_aubio_filter(cWeight);
      if (inputBuffer) del_fvec(inputBuffer);
   }
};

// ============================================================================
// TemporalProcessor implementation
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

TemporalProcessor::TemporalProcessor(uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   // Core Guidelines: create aubio temporal processing objects.

   impl_->sampleRate = sampleRate;
   impl_->channels = 1;

   // Core Guidelines: create resampler. aubio_resampler takes (ratio, type).
   // We start with a 1:1 ratio (no resampling) and update dynamically.
   // SRC_SINC_best_quality is the highest quality constant.
   impl_->resampler = new_aubio_resampler(1.0, SRC_SINC_best_quality);

   // Core Guidelines: create biquad filter with default coefficients.
   // aubio_filter_biquad takes 5 coefficients: (b0, b1, b2, a1, a2).
   // Default: unity gain low-pass at Nyquist.
   impl_->biquad = new_aubio_filter_biquad(0.5, 0.0, 0.0, 0.5, 0.0);

   // Core Guidelines: create weighting filters.
   // aubio_filter_a_weighting and aubio_filter_c_weighting take samplerate.
   impl_->aWeight = new_aubio_filter_a_weighting(sampleRate);
   impl_->cWeight = new_aubio_filter_c_weighting(sampleRate);

   // Core Guidelines: create a generic filter (order=2 for biquad).
   impl_->filter = new_aubio_filter(2);

   // Core Guidelines: allocate input buffer.
   impl_->inputBuffer = new_fvec(4096);
}

TemporalProcessor::~TemporalProcessor() = default;

TemporalProcessor::TemporalProcessor(TemporalProcessor&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

TemporalProcessor& TemporalProcessor::operator=(
   TemporalProcessor&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

std::vector<float> TemporalProcessor::resample(const std::vector<float>& samples,
                                               uint32_t targetSampleRate) {
   // Core Guidelines: resample audio to a different sample rate.
   // Core Guidelines: aubio_resampler_do takes 3 args: (resampler, input, output).

   if (samples.empty() || impl_ == nullptr || impl_->resampler == nullptr) {
      return {};
   }

   // Core Guidelines: compute the resampling ratio.
   double ratio = static_cast<double>(targetSampleRate) / impl_->sampleRate;
   del_aubio_resampler(impl_->resampler);
   impl_->resampler = new_aubio_resampler(static_cast<smpl_t>(ratio),
                                         SRC_SINC_best_quality);

   // Core Guidelines: resample the input.
   // aubio_resampler_do takes (resampler, input, output).
   uint32_t numSamples = static_cast<uint32_t>(samples.size());
   fvec_t* input = new_fvec(numSamples);
   std::memcpy(input->data, samples.data(), numSamples * sizeof(float));

   // Core Guidelines: compute output length.
   uint32_t outLength = static_cast<uint32_t>(
      static_cast<double>(numSamples) * targetSampleRate / impl_->sampleRate);

   fvec_t* output = new_fvec(outLength);
   aubio_resampler_do(impl_->resampler, input, output);

   std::vector<float> result(output->data, output->data + outLength);

   del_fvec(input);
   del_fvec(output);

   return result;
}

std::vector<float> TemporalProcessor::lowPass(const std::vector<float>& samples,
                                              float cutoffHz) {
   // Core Guidelines: apply a low-pass filter.
   // Core Guidelines: aubio_filter_do takes 2 args (in-place), so we use
   // aubio_filter_do_outplace for separate output.

   if (samples.empty() || impl_ == nullptr || impl_->filter == nullptr) {
      return {};
   }

   // Core Guidelines: set the filter to a biquad low-pass at cutoffHz.
   // Compute biquad coefficients using standard analog prototype.
   float ws = std::tan(M_PI * cutoffHz / impl_->sampleRate);
   float norm = 1.0f / (1.0f + std::sqrt(2.0f) * ws + ws * ws);
   float b0 = ws * ws * norm;
   float b1 = 2.0f * b0;
   float b2 = b0;
   float a1 = 2.0f * (ws * ws - 1.0f) * norm;
   float a2 = (1.0f - std::sqrt(2.0f) * ws + ws * ws) * norm;

   aubio_filter_set_biquad(impl_->filter, static_cast<lsmp_t>(b0),
                           static_cast<lsmp_t>(b1), static_cast<lsmp_t>(b2),
                           static_cast<lsmp_t>(a1), static_cast<lsmp_t>(a2));

   // Core Guidelines: apply the filter.
   // aubio_filter_do_outplace takes (filter, input, output).
   uint32_t numSamples = static_cast<uint32_t>(samples.size());
   fvec_t* input = new_fvec(numSamples);
   std::memcpy(input->data, samples.data(), numSamples * sizeof(float));

   fvec_t* output = new_fvec(numSamples);
   aubio_filter_do_outplace(impl_->filter, input, output);

   std::vector<float> result(output->data, output->data + numSamples);

   del_fvec(input);
   del_fvec(output);

   return result;
}

std::vector<float> TemporalProcessor::highPass(const std::vector<float>& samples,
                                               float cutoffHz) {
   // Core Guidelines: apply a high-pass filter.
   // Core Guidelines: compute biquad high-pass coefficients.

   if (samples.empty() || impl_ == nullptr || impl_->filter == nullptr) {
      return {};
   }

   // Core Guidelines: set the filter to high-pass biquad at cutoffHz.
   float ws = std::tan(M_PI * cutoffHz / impl_->sampleRate);
   float norm = 1.0f / (1.0f + std::sqrt(2.0f) * ws + ws * ws);
   float b0 = norm;
   float b1 = -2.0f * b0;
   float b2 = b0;
   float a1 = 2.0f * (ws * ws - 1.0f) * norm;
   float a2 = (1.0f - std::sqrt(2.0f) * ws + ws * ws) * norm;

   aubio_filter_set_biquad(impl_->filter, static_cast<lsmp_t>(b0),
                           static_cast<lsmp_t>(b1), static_cast<lsmp_t>(b2),
                           static_cast<lsmp_t>(a1), static_cast<lsmp_t>(a2));

   // Core Guidelines: apply the filter.
   // aubio_filter_do_outplace takes (filter, input, output).
   uint32_t numSamples = static_cast<uint32_t>(samples.size());
   fvec_t* input = new_fvec(numSamples);
   std::memcpy(input->data, samples.data(), numSamples * sizeof(float));

   fvec_t* output = new_fvec(numSamples);
   aubio_filter_do_outplace(impl_->filter, input, output);

   std::vector<float> result(output->data, output->data + numSamples);

   del_fvec(input);
   del_fvec(output);

   return result;
}

std::vector<float> TemporalProcessor::aWeighting(
   const std::vector<float>& samples) {
   // Core Guidelines: apply an A-weighting filter.
   // Core Guidelines: aubio_filter_do_outplace takes (filter, input, output).

   if (samples.empty() || impl_ == nullptr || impl_->aWeight == nullptr) {
      return {};
   }

   // Core Guidelines: apply A-weighting.
   uint32_t numSamples = static_cast<uint32_t>(samples.size());
   fvec_t* input = new_fvec(numSamples);
   std::memcpy(input->data, samples.data(), numSamples * sizeof(float));

   fvec_t* output = new_fvec(numSamples);
   aubio_filter_do_outplace(impl_->aWeight, input, output);

   std::vector<float> result(output->data, output->data + numSamples);

   del_fvec(input);
   del_fvec(output);

   return result;
}

std::vector<float> TemporalProcessor::cWeighting(
   const std::vector<float>& samples) {
   // Core Guidelines: apply a C-weighting filter.
   // Core Guidelines: aubio_filter_do_outplace takes (filter, input, output).

   if (samples.empty() || impl_ == nullptr || impl_->cWeight == nullptr) {
      return {};
   }

   // Core Guidelines: apply C-weighting.
   uint32_t numSamples = static_cast<uint32_t>(samples.size());
   fvec_t* input = new_fvec(numSamples);
   std::memcpy(input->data, samples.data(), numSamples * sizeof(float));

   fvec_t* output = new_fvec(numSamples);
   aubio_filter_do_outplace(impl_->cWeight, input, output);

   std::vector<float> result(output->data, output->data + numSamples);

   del_fvec(input);
   del_fvec(output);

   return result;
}

std::vector<std::vector<float>> TemporalProcessor::biquadCoefficients(
   std::string_view filterType, float cutoffHz, float q) {
   // Core Guidelines: compute the Biquad filter coefficients.
   // Core Guidelines: returns [a0, a1, a2] (numerator) and [b0, b1, b2]
   // (denominator).

   std::vector<std::vector<float>> result;

   // Core Guidelines: compute coefficients based on filter type.
   if (filterType == "lowpass" || filterType == "highpass" ||
       filterType == "bandpass") {
      // Placeholder: actual computation is done in the filter methods.
      result.push_back({cutoffHz, q, 0.0f});
   }

   return result;
}
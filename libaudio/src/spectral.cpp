/**
 * @file spectral.cpp
 * @brief Implementation of SpectralAnalyzer — spectral analysis via aubio.
 *
 * This module wraps aubio's spectral analysis functions. It provides
 * various spectral features useful for velocity estimation, pedal
 * detection, and polyphonic separation.
 *
 */

#include <libaudio/spectral.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/cvec.h>
#include <aubio/fmat.h>
#include <aubio/spectral/fft.h>
#include <aubio/spectral/specdesc.h>
#include <aubio/spectral/mfcc.h>
#include <aubio/spectral/filterbank.h>
#include <aubio/spectral/filterbank_mel.h>
#include <aubio/mathutils.h>
#include <stdexcept>
#include <cmath>
#include <numeric>

// ============================================================================
// SpectralAnalyzer::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct SpectralAnalyzer::Impl {
   aubio_fft_t* fft = nullptr;
   aubio_specdesc_t* specdesc = nullptr;
   aubio_mfcc_t* mfcc = nullptr;
   aubio_filterbank_t* melFilterbank = nullptr;
   uint32_t bufSize;
   uint32_t hopSize;
   uint32_t sampleRate;
   uint32_t numBins;
   fvec_t* inputBuffer = nullptr;
   cvec_t* spectrum = nullptr;
   fvec_t* mfccOutput = nullptr;
   fvec_t* specdescOutput = nullptr;
   fvec_t* filterbankOutput = nullptr;

   ~Impl() {
      if (fft) del_aubio_fft(fft);
      if (specdesc) del_aubio_specdesc(specdesc);
      if (mfcc) del_aubio_mfcc(mfcc);
      if (melFilterbank) del_aubio_filterbank(melFilterbank);
      if (inputBuffer) del_fvec(inputBuffer);
      if (spectrum) del_cvec(spectrum);
      if (mfccOutput) del_fvec(mfccOutput);
      if (specdescOutput) del_fvec(specdescOutput);
      if (filterbankOutput) del_fvec(filterbankOutput);
   }
};

// ============================================================================
// SpectralAnalyzer implementation
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

SpectralAnalyzer::SpectralAnalyzer(uint32_t bufSize, uint32_t hopSize,
                                   uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   // Create aubio spectral analysis objects.

   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->sampleRate = sampleRate;
   impl_->numBins = bufSize / 2 + 1;

   // Create FFT, spectral description, MFCC, and
   // mel filterbank objects.
   impl_->fft = new_aubio_fft(bufSize);

   // Create specdesc for spectral centroid (uses "centroid"
   // method). aubio_specdesc_do writes to an output fvec.
   impl_->specdesc = new_aubio_specdesc("centroid", bufSize);

   // Create MFCC object. aubio_mfcc takes 4 args:
   // (buf_size, n_filters, n_coeffs, samplerate).
   impl_->mfcc = new_aubio_mfcc(bufSize, 40, 13, sampleRate);

   // Create mel filterbank. Use new_aubio_filterbank
   // (n_filters, win_s) and then set mel coefficients.
   impl_->melFilterbank = new_aubio_filterbank(40, bufSize);
   aubio_filterbank_set_mel_coeffs_slaney(impl_->melFilterbank, sampleRate);

   // Allocate input and spectrum buffers.
   impl_->inputBuffer = new_fvec(bufSize);
   impl_->spectrum = new_cvec(bufSize);

   // Allocate output buffers for MFCC, specdesc, and
   // filterbank results.
   impl_->mfccOutput = new_fvec(13);  // 13 MFCC coefficients.
   impl_->specdescOutput = new_fvec(1);  // Single value per descriptor.
   impl_->filterbankOutput = new_fvec(40);  // 40 mel filterbank bands.
}

SpectralAnalyzer::~SpectralAnalyzer() = default;

SpectralAnalyzer::SpectralAnalyzer(SpectralAnalyzer&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

SpectralAnalyzer& SpectralAnalyzer::operator=(
   SpectralAnalyzer&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

float SpectralAnalyzer::spectralCentroid(const float* samples) {
   // Compute the spectral centroid (brightness).
   // Aubio_specdesc_do writes to an output fvec;
   // we read the centroid from specdescOutput->data[0].

   if (impl_ == nullptr || impl_->fft == nullptr) {
      return 0.0f;
   }

   // Run forward FFT.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));
   aubio_fft_do(impl_->fft, impl_->inputBuffer, impl_->spectrum);

   // Compute spectral centroid using specdesc.
   // aubio_specdesc_do writes a single value to the output fvec.
   aubio_specdesc_do(impl_->specdesc, impl_->spectrum, impl_->specdescOutput);

   return static_cast<float>(impl_->specdescOutput->data[0]);
}

float SpectralAnalyzer::spectralFlux(const std::vector<float>& prevSpectrum,
                                     const std::vector<float>& currSpectrum) {
   // Compute the spectral flux (change in spectral envelope)
   // between two frames.

   if (prevSpectrum.empty() || currSpectrum.empty()) {
      return 0.0f;
   }

   // Compute the sum of positive differences between
   // consecutive magnitude spectra (spectral flux).
   float flux = 0.0f;
   size_t n = std::min(prevSpectrum.size(), currSpectrum.size());

   for (size_t i = 0; i < n; ++i) {
      float diff = currSpectrum[i] - prevSpectrum[i];
      if (diff > 0.0f) {
         flux += diff;  // Only positive changes (onsets).
      }
   }

   return flux;
}

float SpectralAnalyzer::rmsEnergy(const float* samples, uint32_t length) {
   // Compute the RMS energy of a buffer.

   if (samples == nullptr || length == 0) {
      return 0.0f;
   }

   // Compute RMS energy: sqrt(mean(samples^2)).
   double sumSquares = 0.0;
   for (uint32_t i = 0; i < length; ++i) {
      sumSquares += static_cast<double>(samples[i]) * samples[i];
   }

   return static_cast<float>(std::sqrt(sumSquares / length));
}

float SpectralAnalyzer::zeroCrossingRate(const float* samples,
                                        uint32_t length) {
   // Compute the zero-crossing rate.

   if (samples == nullptr || length == 0) {
      return 0.0f;
   }

   // Count the number of times the signal crosses zero.
   uint32_t crossings = 0;
   for (uint32_t i = 1; i < length; ++i) {
      if ((samples[i] >= 0.0f) != (samples[i - 1] >= 0.0f)) {
         crossings++;
      }
   }

   return static_cast<float>(crossings) / static_cast<float>(length - 1);
}

std::vector<float> SpectralAnalyzer::mfcc(const float* samples) {
   // Compute the MFCC coefficients.
   // Aubio_mfcc_do takes 3 args: (mfcc, spectrum, output_fvec).

   if (impl_ == nullptr || impl_->mfcc == nullptr) {
      return {};
   }

   // Run forward FFT.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));
   aubio_fft_do(impl_->fft, impl_->inputBuffer, impl_->spectrum);

   // Compute MFCC coefficients.
   // aubio_mfcc_do writes 13 coefficients to the output fvec.
   aubio_mfcc_do(impl_->mfcc, impl_->spectrum, impl_->mfccOutput);

   std::vector<float> result(impl_->mfccOutput->length);
   std::memcpy(result.data(), impl_->mfccOutput->data,
               impl_->mfccOutput->length * sizeof(float));

   return result;
}

std::vector<float> SpectralAnalyzer::chroma(const float* samples) {
   // Compute the chroma features (12-bin pitch class).
   // Use mel filterbank to compute energy bands, then
   // map to 12 pitch classes.

   if (impl_ == nullptr || impl_->fft == nullptr) {
      return {};
   }

   // Run forward FFT.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));
   aubio_fft_do(impl_->fft, impl_->inputBuffer, impl_->spectrum);

   // Compute mel filterbank energy.
   // aubio_filterbank_do takes 3 args: (filterbank, spectrum, output_fvec).
   aubio_filterbank_do(impl_->melFilterbank, impl_->spectrum,
                       impl_->filterbankOutput);

   // Convert 40 mel bands to 12-bin chroma (pitch class
   // distribution). Map mel band i to pitch class bin.
   std::vector<float> result(12, 0.0f);
   uint32_t numBins = impl_->filterbankOutput->length;

   for (uint32_t i = 0; i < numBins; ++i) {
      uint32_t bin = static_cast<uint32_t>(
         static_cast<float>(i) * 12.0f /
         std::max(1u, numBins / 12u));
      if (bin < 12) {
         result[bin] += impl_->filterbankOutput->data[i];
      }
   }

   return result;
}

float SpectralAnalyzer::spectralRollOff(const float* samples,
                                       float rollOffRatio) {
   // Compute the spectral roll-off frequency.
   // Create a dedicated specdesc object for "rolloff"
   // method, compute it, and read from the output fvec.

   if (impl_ == nullptr || impl_->fft == nullptr) {
      return 0.0f;
   }

   // Run forward FFT.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));
   aubio_fft_do(impl_->fft, impl_->inputBuffer, impl_->spectrum);

   // Compute spectral roll-off using a dedicated specdesc.
   // aubio_specdesc_do writes a single value to the output fvec.
   aubio_specdesc_t* rolloffSpecdesc = new_aubio_specdesc("rolloff", impl_->bufSize);
   fvec_t* rolloffOutput = new_fvec(1);
   aubio_specdesc_do(rolloffSpecdesc, impl_->spectrum, rolloffOutput);

   float rollOff = static_cast<float>(rolloffOutput->data[0]);

   del_aubio_specdesc(rolloffSpecdesc);
   del_fvec(rolloffOutput);

   return rollOff;
}

float SpectralAnalyzer::spectralBandwidth(const float* samples) {
   // Compute the spectral bandwidth from centroid and
   // roll-off.

   if (impl_ == nullptr || impl_->fft == nullptr) {
      return 0.0f;
   }

   // Compute bandwidth as the difference between
   // spectral roll-off and centroid.
   float centroid = spectralCentroid(samples);
   float rollOff = spectralRollOff(samples);

   return rollOff - centroid;
}
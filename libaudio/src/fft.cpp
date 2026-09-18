/**
 * @file fft.cpp
 * @brief Implementation of FFT — forward and inverse spectral analysis
 *        via aubio.
 *
 * This module wraps aubio's FFT for spectral analysis. It provides
 * forward (time-domain → frequency-domain) and inverse
 * (frequency-domain → time-domain) transforms.
 *
 * Note: aubio 0.4.9's cvec_t stores complex values in polar
 * coordinates (norm and phase arrays), not interleaved real/imag.
 * This makes magnitude and phase extraction trivial — just read
 * the norm and phas members directly.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#include <libaudio/fft.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/cvec.h>
#include <aubio/spectral/fft.h>
#include <stdexcept>
#include <cmath>
#include <algorithm>

// ============================================================================
// FFT::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// Core Guidelines: RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct FFT::Impl {
   aubio_fft_t* fft = nullptr;
   uint32_t windowSize;
   uint32_t numBins;
   fvec_t* inputBuffer = nullptr;
   cvec_t* spectrum = nullptr;

   ~Impl() {
      if (fft) del_aubio_fft(fft);
      if (inputBuffer) del_fvec(inputBuffer);
      if (spectrum) del_cvec(spectrum);
   }
};

// ============================================================================
// FFT implementation
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

FFT::FFT(uint32_t windowSize)
   : impl_(std::make_unique<Impl>()) {
   // Core Guidelines: validate that window size is a power of 2.

   impl_->windowSize = windowSize;
   impl_->numBins = windowSize / 2 + 1;

   // Core Guidelines: create the aubio FFT object.
   impl_->fft = new_aubio_fft(windowSize);

   // Core Guidelines: allocate input and spectrum buffers.
   impl_->inputBuffer = new_fvec(windowSize);
   impl_->spectrum = new_cvec(windowSize);
}

FFT::~FFT() = default;

FFT::FFT(FFT&& other) noexcept = default;
FFT& FFT::operator=(FFT&& other) noexcept = default;

std::pair<std::vector<float>, std::vector<float>>
FFT::forward(const float* timeDomain) {
   // Core Guidelines: forward FFT — converts time-domain samples to
   // frequency-domain complex values (magnitude + phase).
   //
   // Domain context: aubio 0.4.9's cvec_t stores complex values in
   // polar coordinates (norm and phase arrays). After running
   // aubio_fft_do, we simply read spectrum->norm[i] and
   // spectrum->phas[i] to get magnitude and phase directly.

   if (impl_ == nullptr || impl_->fft == nullptr) {
      return {{}, {}};
   }

   // Core Guidelines: copy input samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, timeDomain,
               impl_->windowSize * sizeof(float));

   // Core Guidelines: run the forward FFT (produces complex spectrum).
   aubio_fft_do(impl_->fft, impl_->inputBuffer, impl_->spectrum);

   // Core Guidelines: extract magnitude and phase from the cvec's
   // internal norm and phas arrays (polar coordinates).
   std::vector<float> magnitudes(impl_->numBins);
   std::vector<float> phases(impl_->numBins);

   for (uint32_t i = 0; i < impl_->numBins; ++i) {
      magnitudes[i] = impl_->spectrum->norm[i];
      phases[i] = impl_->spectrum->phas[i];
   }

   return {magnitudes, phases};
}

std::vector<float> FFT::inverse(const std::vector<float>& magnitudes,
                                const std::vector<float>& phases) {
   // Core Guidelines: inverse FFT — reconstructs time-domain samples
   // from magnitude and phase spectra.
   //
   // Domain context: aubio 0.4.9's cvec_t stores complex values in
   // polar coordinates. We set spectrum->norm[i] and
   // spectrum->phas[i] from the input vectors, then call
   // aubio_fft_rdo for the inverse transform.

   if (impl_ == nullptr || impl_->fft == nullptr) {
      return {};
   }

   // Core Guidelines: set the cvec's norm and phas arrays from
   // the input magnitude and phase vectors.
   for (uint32_t i = 0; i < impl_->numBins; ++i) {
      impl_->spectrum->norm[i] = magnitudes[i];
      impl_->spectrum->phas[i] = phases[i];
   }

   // Core Guidelines: run the inverse FFT (real-domain version).
   aubio_fft_rdo(impl_->fft, impl_->spectrum, impl_->inputBuffer);

   // Core Guidelines: copy the result.
   std::vector<float> result(impl_->windowSize);
   std::memcpy(result.data(), impl_->inputBuffer->data,
               impl_->windowSize * sizeof(float));

   return result;
}

uint32_t FFT::windowSize() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->windowSize : 0;
}

uint32_t FFT::numBins() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->numBins : 0;
}

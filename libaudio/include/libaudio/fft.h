/**
 * @file fft.h
 * @brief FFT via aubio — forward and inverse spectral analysis.
 *
 * This module wraps aubio's FFT for spectral analysis. It provides
 * forward (time-domain → frequency-domain) and inverse
 * (frequency-domain → time-domain) transforms.
 *
 */

#ifndef LIBAUDIO_FFT_H
#define LIBAUDIO_FFT_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// FFT — Wraps aubio's FFT for spectral analysis.
//
// Domain context: FFT (Fast Fourier Transform) converts time-domain
// audio samples into frequency-domain complex values. This is the
// foundation for many audio analysis algorithms: pitch detection,
// spectral flux, MFCC, chroma features, etc.
//
// Key design decisions:
// - Window size must be a power of 2 (aubio requirement).
// - Returns magnitude and phase separately (not complex numbers).
// - Inverse FFT reconstructs time-domain samples from magnitude + phase.
//
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class FFT {
 public:
   // Create an FFT with the given window size (must be a power of 2).
   //
   // @param windowSize FFT window size (must be a power of 2, e.g., 1024, 2048, 4096).
   explicit FFT(uint32_t windowSize);

   // Destructor. Frees aubio FFT resources.
   // RAII — resources are released automatically.
   ~FFT();

   // Non-copyable (aubio handles are non-copyable).
   FFT(const FFT&) = delete;
   FFT& operator=(const FFT&) = delete;

   // Movable (aubio handles can be moved).
   FFT(FFT&& other) noexcept;
   FFT& operator=(FFT&& other) noexcept;

   // Forward FFT: time-domain samples → frequency-domain complex values.
   //
   // @param timeDomain Input buffer (length must equal windowSize).
   // @return std::pair of (magnitude, phase) vectors.
   //         Magnitude has windowSize/2+1 bins. Phase has windowSize/2+1 bins.
   std::pair<std::vector<float>, std::vector<float>>
   forward(const float* timeDomain);

   // Inverse FFT: frequency-domain complex values → time-domain samples.
   //
   // @param magnitudes Magnitude spectrum.
   // @param phases Phase spectrum.
   // @return Reconstructed time-domain samples (length = windowSize).
   std::vector<float> inverse(const std::vector<float>& magnitudes,
                              const std::vector<float>& phases);

   // Get the FFT window size.
   //
   // @return Window size (must be a power of 2).
   [[nodiscard]] uint32_t windowSize() const;

   // Get the number of frequency bins (windowSize / 2 + 1).
   //
   // @return Number of frequency bins.
   [[nodiscard]] uint32_t numBins() const;

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_FFT_H

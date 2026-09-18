/**
 * @file spectral.h
 * @brief Spectral analysis via aubio — MFCC, chroma, spectral features.
 *
 * This module wraps aubio's spectral analysis functions. It provides
 * various spectral features useful for velocity estimation, pedal
 * detection, and polyphonic separation.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#ifndef LIBAUDIO_SPECTRAL_H
#define LIBAUDIO_SPECTRAL_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// SpectralAnalyzer — Spectral analysis via aubio (FFT, MFCC, chroma, etc.).
//
// Domain context: Spectral analysis extracts features from the frequency
// domain representation of audio. These features are useful for:
// - Velocity estimation: RMS energy and spectral centroid correlate with
//   how hard a piano key was struck.
// - Pedal detection: Spectral flux and energy decay patterns help detect
//   when sustain pedal is pressed.
// - Polyphonic separation: Chroma features and harmonic binning help
//   separate simultaneous notes.
//
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================
class SpectralAnalyzer {
 public:
   // Create a spectral analyzer.
   //
   // @param bufSize    FFT window size (e.g., 1024, 2048, 4096).
   // @param hopSize    Step size between frames (hop).
   // @param sampleRate Sample rate of the input signal.
   SpectralAnalyzer(uint32_t bufSize, uint32_t hopSize, uint32_t sampleRate);

   // Destructor. Frees aubio spectral analysis resources.
   // Core Guidelines: RAII — resources are released automatically.
   ~SpectralAnalyzer();

   // Core Guidelines: non-copyable (aubio handles are non-copyable).
   SpectralAnalyzer(const SpectralAnalyzer&) = delete;
   SpectralAnalyzer& operator=(const SpectralAnalyzer&) = delete;

   // Core Guidelines: movable (aubio handles can be moved).
   SpectralAnalyzer(SpectralAnalyzer&& other) noexcept;
   SpectralAnalyzer& operator=(SpectralAnalyzer&& other) noexcept;

   // Compute the spectral centroid (brightness).
   //
   // @param samples Input samples (length must equal windowSize).
   // @return Spectral centroid in Hz, or 0 if computation failed.
   float spectralCentroid(const float* samples);

   // Compute the spectral flux (change in spectral envelope).
   //
   // @param prevSpectrum Previous frame's magnitude spectrum.
   // @param currSpectrum Current frame's magnitude spectrum.
   // @return Spectral flux value (higher = more transient).
   float spectralFlux(const std::vector<float>& prevSpectrum,
                      const std::vector<float>& currSpectrum);

   // Compute the RMS energy of a buffer.
   //
   // @param samples Input samples.
   // @param length Number of samples.
   // @return RMS energy value (correlates with velocity).
   float rmsEnergy(const float* samples, uint32_t length);

   // Compute the zero-crossing rate.
   //
   // @param samples Input samples.
   // @param length Number of samples.
   // @return Zero-crossing rate (fraction of frames crossing zero).
   float zeroCrossingRate(const float* samples, uint32_t length);

   // Compute the MFCC coefficients (Mel-frequency cepstral coefficients).
   //
   // @param samples Input samples (length must equal windowSize).
   // @return MFCC coefficients (typically 13 coefficients).
   std::vector<float> mfcc(const float* samples);

   // Compute the chroma features (12-bin pitch class distribution).
   //
   // @param samples Input samples (length must equal windowSize).
   // @return Chroma features (12 values, one per pitch class).
   std::vector<float> chroma(const float* samples);

   // Compute the spectral roll-off (frequency below which X% of energy
   // is contained).
   //
   // @param samples Input samples (length must equal windowSize).
   // @param rollOffRatio Ratio of energy to consider (default 0.85 = 85%).
   // @return Spectral roll-off frequency in Hz.
   float spectralRollOff(const float* samples, float rollOffRatio = 0.85f);

   // Compute the spectral bandwidth.
   //
   // @param samples Input samples (length must equal windowSize).
   // @return Spectral bandwidth in Hz.
   float spectralBandwidth(const float* samples);

 private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_SPECTRAL_H

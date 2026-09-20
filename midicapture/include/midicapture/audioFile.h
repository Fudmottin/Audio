/**
 * @file audioFile.h
 * @brief Lightweight wrapper around libaudio::AudioFileReader.
 *
 * This module provides a simple interface for reading audio files
 * (AIFF, WAV, FLAC, etc.) via libsndfile (through libaudio).
 * It exposes file metadata (sample rate, channels, duration) and
 * frame-based reading.
 *
 * @section design Design
 *
 * We use libaudio::AudioFileReader (wrapping libsndfile) directly.
 * This module is a thin wrapper that adds format name reporting
 * and a convenience method for getting the format name as a string.
 *
 */

#ifndef MIDICAPTURE_AUDIOFILE_H
#define MIDICAPTURE_AUDIOFILE_H

#include <cstdint>
#include <libaudio/audioFile.h>
#include <memory>
#include <string>

// ============================================================================
// AudioFile — Lightweight wrapper around libaudio::AudioFileReader.
//
// Domain context: This class wraps libaudio::AudioFileReader (which
// wraps libsndfile) to provide a simple interface for reading audio
// files. It exposes file metadata (sample rate, channels, duration)
// and frame-based reading.
//
// Key design decisions:
// - Uses libaudio::AudioFileReader internally (Pimpl pattern).
// - Exposes format name as a human-readable string.
// - Supports mono and stereo files (mono downmix for analysis).
//
// RAII resource management — resources are automatically
// freed when the AudioFile is destroyed.
// ============================================================================
class AudioFile {
 public:
   // Open an audio file for reading.
   //
   // @param path Path to the audio file (AIFF, WAV, FLAC, etc.).
   // @throws std::runtime_error if the file cannot be opened.
   explicit AudioFile(const std::string& path);

   // Destructor. Closes the file handle.
   // RAII — resources are released automatically.
   ~AudioFile();

   // Non-copyable (file handles are non-copyable).
   AudioFile(const AudioFile&) = delete;
   AudioFile& operator=(const AudioFile&) = delete;

   // Get the sample rate in Hz (e.g., 44100, 48000).
   //
   // @return Sample rate in Hz, or 0 if the file is not open.
   [[nodiscard]] uint32_t sampleRate() const;

   // Get the number of channels (1 = mono, 2 = stereo).
   //
   // @return Number of channels, or 0 if the file is not open.
   [[nodiscard]] uint32_t channels() const;

   // Get the total number of frames in the file.
   //
   // @return Total frames, or 0 if the file is not open.
   [[nodiscard]] uint32_t totalFrames() const;

   // Get the audio format name (e.g., "AIFF", "WAV", "FLAC").
   //
   // @return Format name string, or "unknown" if the file is not open.
   [[nodiscard]] std::string formatName() const;

   // Read a block of mono samples from the file.
   //
   // Reads the next `hopSize` frames, downmixing stereo to mono
   // by averaging left and right channels. Returns the actual
   // number of frames read (may be less than hopSize at EOF).
   //
   // @param[out] buffer Output buffer (must be at least hopSize elements).
   // @param hopSize Maximum number of frames to read.
   // @return Number of frames actually read (0 at EOF).
   uint32_t readMono(float* buffer, uint32_t hopSize);

   // Check if the file is still readable (not at EOF).
   //
   // @return true if more data can be read, false at EOF.
   [[nodiscard]] bool eof() const;

   // Reset to the beginning of the file.
   void reset();

 private:
   // Private implementation — all libsndfile C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // MIDICAPTURE_AUDIOFILE_H

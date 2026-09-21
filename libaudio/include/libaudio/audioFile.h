/**
 * @file audioFile.h
 * @brief Audio file reading via libsndfile — reads AIFF, WAV, FLAC, etc.
 *
 * This module wraps libsndfile for reading audio files. It provides a
 * clean C++ interface for reading mono and stereo audio data, with
 * support for seeking and resetting.
 *
 * @section audiofile-design Design
 *
 * libsndfile handles audio file I/O (reading and writing). It supports
 * AIFF, WAV, FLAC, OGG, MP3, and many other formats. It is more
 * general-purpose, better documented, and already installed.
 *
 * Key design decisions:
 * - We use libsndfile (not aubio's source/sink) because it supports
 *   more formats and is more general-purpose.
 * - The Pimpl pattern isolates all libsndfile C API calls.
 * - Reading returns float samples in the range [-1.0, 1.0].
 *
 */

#ifndef LIBAUDIO_AUDIOFILE_H
#define LIBAUDIO_AUDIOFILE_H

#include <cstdint>
#include <memory>
#include <string>

// ============================================================================
// AudioFileReader — Reads audio files (AIFF, WAV, FLAC, etc.) via libsndfile.
//
// Domain context: This class wraps libsndfile's SF_INFO and SNDFILE
// types. It provides a clean C++ interface for reading audio data
// as float samples in the range [-1.0, 1.0].
//
// Key design decisions:
// - Pimpl pattern: all libsndfile C API calls are isolated in Impl.
// - Returns float samples (not int16): this is the standard format
//   for DSP processing. Downstream modules convert to int16 if needed.
// - Supports mono and stereo files. Stereo files can be read as
//   interleaved (L, R, L, R...) or downmixed to mono.
//
// RAII resource management — resources are automatically
// freed when the C++ object is destroyed (no manual sf_close() calls).
// ============================================================================
class AudioFileReader {
 public:
   // Create an AudioFileReader for the given file path.
   //
   // Opens the file and reads the header to determine format.
   // Throws std::runtime_error if the file cannot be opened or is
   // not a supported audio format.
   //
   // @param path Path to the audio file (e.g., "recording.aiff").
   AudioFileReader(std::string_view path);

   // Destructor. Closes the file handle.
   // RAII — resources are released automatically.
   ~AudioFileReader();

   // Non-copyable (file handles are non-copyable).
   AudioFileReader(const AudioFileReader&) = delete;
   AudioFileReader& operator=(const AudioFileReader&) = delete;

   // Movable (file handles can be moved).
   AudioFileReader(AudioFileReader&& other) noexcept;
   AudioFileReader& operator=(AudioFileReader&& other) noexcept;

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

   // Read a block of samples (monophonic).
   //
   // Reads the next `hopSize` frames from the file, downmixing stereo
   // to mono by averaging left and right channels. Returns the actual
   // number of frames read (may be less than hopSize at EOF).
   //
   // @param[out] buffer Output buffer (must be at least hopSize elements).
   // @param hopSize Maximum number of frames to read.
   // @return Number of frames actually read (0 at EOF).
   uint32_t read(float* buffer, uint32_t hopSize);

   // Read a block of samples (stereo).
   //
   // Reads the next `hopSize` frames from the file, returning left
   // and right channel samples separately. Returns the actual number
   // of frames read (may be less than hopSize at EOF).
   //
   // @param[out] leftOutput Output buffer for left channel.
   // @param[out] rightOutput Output buffer for right channel.
   // @param hopSize Maximum number of frames to read.
   // @return Number of frames actually read (0 at EOF).
   uint32_t readStereo(float* leftOutput, float* rightOutput, uint32_t hopSize);

   // Downmix stereo to mono (average of L and R).
   //
   // Reads the next `hopSize` frames from the file and returns mono
   // by averaging left and right channels. Returns the actual number
   // of frames read (may be less than hopSize at EOF).
   //
   // @param[out] monoOutput Output buffer.
   // @param hopSize Maximum number of frames to read.
   // @return Number of frames actually read (0 at EOF).
   uint32_t readMono(float* monoOutput, uint32_t hopSize);

   // Seek to a specific frame position.
   //
   // @param frame Frame position to seek to (0-based).
   void seek(uint32_t frame);

   // Reset to the beginning of the file.
   void reset();

   // Check if the file is still readable (not at EOF).
   //
   // @return true if more data can be read, false at EOF.
   [[nodiscard]] bool eof() const;

 private:
   // Private implementation — all libsndfile C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_AUDIOFILE_H

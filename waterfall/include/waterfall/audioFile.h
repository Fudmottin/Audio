/**
 * @file audioFile.h
 * @brief Lightweight wrapper around libaudio::AudioFileReader.
 *
 * This module provides a simple interface for reading audio files
 * (AIFF, WAV, FLAC, etc.) via libsndfile (through libaudio).
 * It exposes file metadata (sample rate, channels, duration) and
 * block-based reading.
 *
 * @section design Design
 *
 * This is the shared audio-file-reading layer used by both `midicapture` and
 * `waterfall`. It mirrors midicapture's `AudioFile` so the file I/O path is
 * identical across both tools: a thin, RAII wrapper over
 * libaudio::AudioFileReader. `midicapture` uses frame-based reading with a
 * fixed hop; `waterfall` uses block-based reading with an arbitrary sample
 * count, so this wrapper exposes both.
 *
 */

#ifndef WATERFALL_AUDIOFILE_H
#define WATERFALL_AUDIOFILE_H

#include <cstdint>
#include <libaudio/audioFile.h>
#include <memory>
#include <string>
#include <vector>

// ============================================================================
// AudioFile — Lightweight wrapper around libaudio::AudioFileReader.
//
// Domain context: This class wraps libaudio::AudioFileReader (which
// wraps libsndfile) to provide a simple interface for reading audio
// files. It exposes file metadata (sample rate, channels, duration) and
// block-based reading.
//
// Key design decisions:
// - Uses libaudio::AudioFileReader internally (Pimpl pattern).
// - Exposes format name as a human-readable string.
// - Supports mono and stereo files (mono downmix for analysis).
// - Block reading accepts any sample count; callers that need a fixed
//   hop (midicapture) or an arbitrary window+overlap (waterfall) both
//   drive it from here.
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

   // Get the duration of the file in seconds.
   //
   // @return Duration in seconds, or 0 if the file is not open.
   [[nodiscard]] double duration() const;

   // Read a block of mono samples from the file, filling a caller buffer.
   //
   // Reads up to `sampleCount` frames, downmixing stereo to mono, and writes
   // them to `buffer` (which must hold at least `sampleCount` floats). Any
   // frames beyond end-of-file are zero-padded, so `buffer` is always fully
   // filled. Returns the number of real frames actually read (may be less
   // than `sampleCount` at end of file; 0 when already at EOF).
   //
   // @param sampleCount Number of frames to read.
   // @param buffer      Destination buffer (length >= sampleCount).
   // @return Number of real frames read.
   uint32_t readInto(uint32_t sampleCount, float* buffer);

   // Reset the read position to the beginning of the file.
   void reset();

 private:
   // Private implementation — delegates to libaudio::AudioFileReader.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // WATERFALL_AUDIOFILE_H

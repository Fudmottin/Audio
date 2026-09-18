/**
 * @file audioFile.cpp
 * @brief Implementation of AudioFileReader — reads AIFF, WAV, FLAC, etc.
 *        via libsndfile.
 *
 * This module wraps libsndfile for reading audio files. It provides
 * a clean C++ interface for reading mono and stereo audio data, with
 * support for seeking and resetting.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 * @see lode/libaudio/decisions.md — Library choices rationale
 * @see lode/terminology.md — Audio concepts (PCM, frame, interleaved)
 */

#include <libaudio/audioFile.h>
#include <sndfile.h>
#include <stdexcept>
#include <cstring>

// ============================================================================
// AudioFileReader::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All libsndfile C API calls are isolated here. The
// public interface never exposes SF_INFO or SNDFILE types.
//
// Core Guidelines: RAII — the SF_INFO and SNDFILE resources are
// automatically freed when the Impl is destroyed.
// ============================================================================
struct AudioFileReader::Impl {
   SNDFILE* file = nullptr;
   SF_INFO info = {};
   uint32_t currentFrame = 0;
   std::string filePath;

   ~Impl() {
      if (file != nullptr) {
         sf_close(file);
         file = nullptr;
      }
   }
};

// ============================================================================
// AudioFileReader implementation
// Core Guidelines: RAII resource management — resources are automatically
// freed when the C++ object is destroyed (no manual sf_close() calls).
// ============================================================================

AudioFileReader::AudioFileReader(std::string_view path)
   : impl_(std::make_unique<Impl>()) {
   // Core Guidelines: open the file and read the header to determine format.
   // Throws std::runtime_error if the file cannot be opened or is not a
   // supported audio format.

   impl_->file = sf_open(path.data(), SFM_READ, &impl_->info);
   if (impl_->file == nullptr) {
      throw std::runtime_error(
         std::string("Could not open audio file: ") + path.data());
   }

   impl_->filePath = std::string(path);
   impl_->currentFrame = 0;
}

AudioFileReader::~AudioFileReader() = default;

AudioFileReader::AudioFileReader(AudioFileReader&& other) noexcept
   : impl_(std::move(other.impl_)) {
   // Core Guidelines: move constructor. Transfer ownership of the file
   // handle and state from the source object.
   other.impl_ = std::make_unique<Impl>();
}

AudioFileReader& AudioFileReader::operator=(AudioFileReader&& other) noexcept {
   // Core Guidelines: move assignment operator. Release current resources
   // and take ownership of the source object's resources.

   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

uint32_t AudioFileReader::sampleRate() const {
   // Core Guidelines: simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->info.samplerate) : 0;
}

uint32_t AudioFileReader::channels() const {
   // Core Guidelines: simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->info.channels) : 0;
}

uint32_t AudioFileReader::totalFrames() const {
   // Core Guidelines: simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->info.frames) : 0;
}

uint32_t AudioFileReader::read(float* buffer, uint32_t hopSize) {
   // Core Guidelines: reads the next `hopSize` frames from the file,
   // downmixing stereo to mono by averaging left and right channels.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return 0;
   }

   // Core Guidelines: read as many frames as requested.
   // libsndfile handles the format conversion internally.
   int framesRead = static_cast<int>(
      sf_readf_float(impl_->file, buffer, static_cast<sf_count_t>(hopSize)));

   if (framesRead < 0) {
      return 0;  // Error.
   }

   // Core Guidelines: if stereo, downmix to mono by averaging.
   if (impl_->info.channels == 2 && framesRead > 0) {
      for (uint32_t i = 0; i < static_cast<uint32_t>(framesRead); ++i) {
         float left = buffer[i * 2];
         float right = buffer[i * 2 + 1];
         buffer[i] = (left + right) / 2.0f;
      }
   }

   impl_->currentFrame += static_cast<uint32_t>(framesRead);
   return static_cast<uint32_t>(framesRead);
}

uint32_t AudioFileReader::readStereo(float* leftOutput, float* rightOutput,
                                     uint32_t hopSize) {
   // Core Guidelines: reads the next `hopSize` frames from the file,
   // returning left and right channel samples separately.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return 0;
   }

   int framesRead = static_cast<int>(
      sf_readf_float(impl_->file, leftOutput,
                     static_cast<sf_count_t>(hopSize * 2)));

   if (framesRead < 0) {
      return 0;
   }

   // Core Guidelines: split interleaved samples into left/right channels.
   for (uint32_t i = 0; i < static_cast<uint32_t>(framesRead); ++i) {
      leftOutput[i] = leftOutput[i * 2];
      rightOutput[i] = leftOutput[i * 2 + 1];
   }

   impl_->currentFrame += static_cast<uint32_t>(framesRead);
   return static_cast<uint32_t>(framesRead);
}

uint32_t AudioFileReader::readMono(float* monoOutput, uint32_t hopSize) {
   // Core Guidelines: reads the next `hopSize` frames from the file and
   // returns mono by averaging left and right channels.

   return read(monoOutput, hopSize);
}

void AudioFileReader::seek(uint32_t frame) {
   // Core Guidelines: seek to a specific frame position.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return;
   }

   // Core Guidelines: SF_SEEK_FRAME is not available in all libsndfile
   // versions; use standard SEEK_SET instead (equivalent behavior).
   sf_seek(impl_->file, static_cast<sf_count_t>(frame), SEEK_SET);
   impl_->currentFrame = frame;
}

void AudioFileReader::reset() {
   // Core Guidelines: reset to the beginning of the file.

   seek(0);
}

bool AudioFileReader::eof() const {
   // Core Guidelines: check if the file is at EOF.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return true;
   }

   return static_cast<uint32_t>(impl_->currentFrame) >= totalFrames();
}

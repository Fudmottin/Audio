/**
 * @file audioFile.cpp
 * @brief Implementation of AudioFileReader — reads AIFF, WAV, FLAC, etc.
 *        via libsndfile.
 *
 * This module wraps libsndfile for reading audio files. It provides
 * a clean C++ interface for reading mono and stereo audio data, with
 * support for seeking and resetting.
 *
 * @section audio-quality Audio Quality Issues
 *
 * These are the common audio quality problems that affect downstream
 * analysis (pitch detection, onset detection, note detection):
 *
 * 1. **Low signal-to-noise ratio** — False pitch detections. Mitigate
 *    by increasing the confidence threshold or using a higher window
 *    size (4096 over 2048).
 *
 * 2. **Room reverb** — Smears transients, making onset detection
 *    unreliable. Mitigate with a pre-emphasis filter or by increasing
 *    the window size.
 *
 * 3. **Stereo recordings** — Phase cancellation when summing stereo
 *    to mono. Mitigate by downmixing (average L+R) or by analyzing
 *    each channel separately.
 *
 * 4. **Dynamic range** — Soft notes near the noise floor are hard to
 *    detect. Mitigate by normalizing to [-1.0, 1.0] and using an
 *    adaptive threshold (lower silence threshold).
 *
 * 5. **Non-piano content** — Confuses pitch detection. Mitigate by
 *    filtering to the piano range (27.5 Hz–4186 Hz, MIDI notes
 *    21–108) before analysis.
 *
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <libaudio/audioFile.h>
#include <sndfile.h>
#include <stdexcept>
#include <string>

namespace libaudio {

// ============================================================================
// AudioFileReader::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All libsndfile C API calls are isolated here. The
// public interface never exposes SF_INFO or SNDFILE types.
//
// RAII — the SF_INFO and SNDFILE resources are
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
// RAII resource management — resources are automatically
// freed when the C++ object is destroyed (no manual sf_close() calls).
// ============================================================================

AudioFileReader::AudioFileReader(std::string_view path)
   : impl_(std::make_unique<Impl>()) {
   // Open the file and read the header to determine format.
   // Throws std::runtime_error if the file cannot be opened or is not a
   // supported audio format.

   impl_->file = sf_open(path.data(), SFM_READ, &impl_->info);
   if (impl_->file == nullptr) {
      throw std::runtime_error(std::string("Could not open audio file: ") +
                               path.data());
   }

   impl_->filePath = std::string(path);
   impl_->currentFrame = 0;
}

AudioFileReader::~AudioFileReader() = default;

AudioFileReader::AudioFileReader(AudioFileReader&& other) noexcept
   : impl_(std::move(other.impl_)) {
   // Move constructor. Transfer ownership of the file
   // handle and state from the source object.
   other.impl_ = std::make_unique<Impl>();
}

AudioFileReader& AudioFileReader::operator=(AudioFileReader&& other) noexcept {
   // Move assignment operator. Release current resources
   // and take ownership of the source object's resources.

   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

uint32_t AudioFileReader::sampleRate() const {
   // Simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->info.samplerate) : 0;
}

uint32_t AudioFileReader::channels() const {
   // Simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->info.channels) : 0;
}

uint32_t AudioFileReader::totalFrames() const {
   // Simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->info.frames) : 0;
}

uint32_t AudioFileReader::read(float* buffer, uint32_t hopSize) {
   // Reads the next `hopSize` frames from the file,
   // downmixing stereo to mono by averaging left and right channels.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return 0;
   }

   // Read as many frames as requested.
   // libsndfile handles the format conversion internally.
   int framesRead = static_cast<int>(
      sf_readf_float(impl_->file, buffer, static_cast<sf_count_t>(hopSize)));

   if (framesRead < 0) {
      return 0; // Error.
   }

   // If stereo, downmix to mono by averaging.
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
   // Reads the next `hopSize` frames from the file,
   // returning left and right channel samples separately.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return 0;
   }

   int framesRead =
      static_cast<int>(sf_readf_float(impl_->file, leftOutput,
                                      static_cast<sf_count_t>(hopSize * 2)));

   if (framesRead < 0) {
      return 0;
   }

   // Split interleaved samples into left/right channels.
   for (uint32_t i = 0; i < static_cast<uint32_t>(framesRead); ++i) {
      leftOutput[i] = leftOutput[i * 2];
      rightOutput[i] = leftOutput[i * 2 + 1];
   }

   impl_->currentFrame += static_cast<uint32_t>(framesRead);
   return static_cast<uint32_t>(framesRead);
}

uint32_t AudioFileReader::readMono(float* monoOutput, uint32_t hopSize) {
   // Reads the next `hopSize` frames from the file and
   // returns mono by averaging left and right channels.

   return read(monoOutput, hopSize);
}

void AudioFileReader::seek(uint32_t frame) {
   // Seek to a specific frame position.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return;
   }

   // SF_SEEK_FRAME is not available in all libsndfile
   // versions; use standard SEEK_SET instead (equivalent behavior).
   sf_seek(impl_->file, static_cast<sf_count_t>(frame), SEEK_SET);
   impl_->currentFrame = frame;
}

void AudioFileReader::reset() {
   // Reset to the beginning of the file.

   seek(0);
}

bool AudioFileReader::eof() const {
   // Check if the file is at EOF.

   if (impl_ == nullptr || impl_->file == nullptr) {
      return true;
   }

   return static_cast<uint32_t>(impl_->currentFrame) >= totalFrames();
}

std::string AudioFileReader::formatName() const {
   // Map the file extension to a human-readable format name.
   //
   // Domain context: libsndfile stores the format as an opaque integer;
   // the file extension is the most reliable human-readable signal. This
   // is the one place both midicapture and waterfall need the format name
   // for user-facing output, so we centralize it here rather than letting
   // each tool re-derive it from the path.
   if (impl_ == nullptr) {
      return "unknown";
   }

   // Lower-case a copy of the path so the suffix check is
   // case-insensitive ("FOO.WAV" and "foo.wav" are the same format).
   std::string lower = impl_->filePath;
   for (char& c : lower) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
   }

   // Suffix checks, longest match first (".aiff" is 5 chars, the others
   // are 4; the order matters for the size guard, not the result).
   if (lower.size() >= 5 && lower.ends_with(".aiff")) {
      return "AIFF";
   }
   if (lower.size() >= 4 && lower.ends_with(".wav")) {
      return "WAV";
   }
   if (lower.size() >= 5 && lower.ends_with(".flac")) {
      return "FLAC";
   }
   if (lower.size() >= 4 && lower.ends_with(".ogg")) {
      return "OGG";
   }
   return "unknown";
}

double AudioFileReader::duration() const {
   // Duration in seconds = total frames / sample rate.
   // Guard against a zero sample rate (file not open) to avoid div-by-zero.
   if (impl_ == nullptr || impl_->info.samplerate == 0) {
      return 0.0;
   }
   return static_cast<double>(impl_->info.frames) /
          static_cast<double>(impl_->info.samplerate);
}

uint32_t AudioFileReader::readInto(uint32_t sampleCount, float* buffer) {
   // Zero-fill the buffer, then read up to sampleCount frames into it.
   //
   // Domain context: a partial final row at EOF should become a fully
   // zero-padded frame, because the downstream analyzer (FFT, etc.)
   // expects a fixed-length window. This is the shape both midicapture
   // (fixed hop) and waterfall (arbitrary window + overlap) want.
   if (sampleCount == 0 || buffer == nullptr) {
      return 0;
   }

   // std::fill_n zero-pads the whole buffer first, so a short read
   // (or an already-closed file) leaves a safe, all-zero frame behind.
   std::fill_n(buffer, static_cast<size_t>(sampleCount), 0.0f);

   // If the file is at EOF, the buffer is already fully zeroed; return 0.
   if (eof()) {
      return 0;
   }

   // readMono downmixes stereo to mono and returns the real frames read.
   return readMono(buffer, sampleCount);
}

} // namespace libaudio

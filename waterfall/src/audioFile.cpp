/**
 * @file audioFile.cpp
 * @brief Implementation of the AudioFile wrapper.
 *
 * Delegates all libsndfile handling to libaudio's AudioFileReader (Pimpl).
 * This is the shared file-reading path for midicapture and waterfall.
 *
 */

#include <waterfall/audioFile.h>

#include <algorithm>
#include <cctype>
#include <libaudio/audioFile.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// ============================================================================
// AudioFile::Impl — Pimpl holding the libaudio reader and cached metadata.
//
// Domain context: All libsndfile C API calls are isolated in AudioFileReader.
// The reader is non-copyable/non-movable, so we own it via a raw pointer and
// free it in the AudioFile destructor (mirrors midicapture's audioFile.cpp).
// ============================================================================
struct AudioFile::Impl {
   // AudioFileReader wraps libsndfile.
   // Raw pointer because AudioFileReader is non-copyable/non-movable.
   AudioFileReader* reader = nullptr;

   // Cached metadata, read once at construction.
   uint32_t sampleRate = 0;
   uint32_t channels = 0;
   uint32_t totalFrames = 0;
   std::string formatName = "unknown";

   // Map file extension to a human-readable format name (a midicapture
   // convenience; waterfall only needs it for the header placeholder).
   static std::string getFormatName(const std::string& path) {
      std::string lower = path;
      for (auto& c : lower) {
         c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
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
};

// ---------------------------------------------------------------------------
// Construction / destruction.
// ---------------------------------------------------------------------------
AudioFile::AudioFile(const std::string& path)
   : impl_(std::make_unique<Impl>()) {
   // Opens the file. Throws std::runtime_error if it cannot be opened or is
   // not a supported audio format. On throw, the raw reader pointer was
   // never set, so Impl holds nothing to leak.
   impl_->reader = new AudioFileReader(path);

   impl_->sampleRate = impl_->reader->sampleRate();
   impl_->channels = impl_->reader->channels();
   impl_->totalFrames = impl_->reader->totalFrames();
   impl_->formatName = Impl::getFormatName(path);
}

AudioFile::~AudioFile() {
   delete impl_->reader;
}

// ---------------------------------------------------------------------------
// Metadata accessors.
// ---------------------------------------------------------------------------
uint32_t AudioFile::sampleRate() const {
   return impl_ ? impl_->sampleRate : 0;
}

uint32_t AudioFile::channels() const {
   return impl_ ? impl_->channels : 0;
}

uint32_t AudioFile::totalFrames() const {
   return impl_ ? impl_->totalFrames : 0;
}

std::string AudioFile::formatName() const {
   return impl_ ? impl_->formatName : std::string("unknown");
}

double AudioFile::duration() const {
   if (!impl_ || impl_->sampleRate == 0) {
      return 0.0;
   }
   return static_cast<double>(impl_->totalFrames) /
          static_cast<double>(impl_->sampleRate);
}

// ---------------------------------------------------------------------------
// Block reading (mono), filling a caller buffer.
// ---------------------------------------------------------------------------
uint32_t AudioFile::readInto(uint32_t sampleCount, float* buffer) {
   if (!impl_ || sampleCount == 0 || buffer == nullptr ||
       impl_->reader == nullptr) {
      return 0;
   }

   // Ensure the buffer is fully filled; zero-init handles end-of-file pad.
   std::fill_n(buffer, sampleCount, 0.0f);

   if (impl_->reader->eof()) {
      return 0;
   }

   // readMono downmixes stereo to mono and returns the real frames read.
   const uint32_t n = impl_->reader->readMono(buffer, sampleCount);
   return n;
}

// ---------------------------------------------------------------------------
// Reset read position.
// ---------------------------------------------------------------------------
void AudioFile::reset() {
   if (impl_ && impl_->reader != nullptr) {
      impl_->reader->reset();
   }
}

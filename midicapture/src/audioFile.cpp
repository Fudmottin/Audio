// /**
//  * @file audioFile.cpp
//  * @brief Implementation of AudioFile — audio file reading via
//  *        libsndfile (through libaudio).
//  *
//  * This module wraps AudioFileReader (which wraps libsndfile)
//  * to provide a simple interface for reading audio files (AIFF, WAV,
//  * FLAC, etc.). It exposes file metadata (sample rate, channels,
//  * duration) and frame-based reading.
//  *
//  * @section design Design
//  *
//  * We use AudioFileReader internally (Pimpl pattern).
//  * This module adds format name reporting as a convenience.
//  *
//  * @see lode/libaudio/summary.md — DSP library design
//  */

#include <libaudio/audioFile.h>
#include <stdexcept>
#include <string>

// Forward declaration of our own header.
#include <midicapture/audioFile.h>

// ============================================================================
// AudioFile::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All libsndfile C API calls are isolated here. The public
// interface never exposes SNDFILE* or SF_INFO types. This makes the rest
// of the codebase framework-free (except for the audioFile.h
// header which only includes audioFile.h).
//
// RAII — the file handle is automatically closed when the Impl is
// destroyed.
// ============================================================================
struct AudioFile::Impl {
   // AudioFileReader wraps libsndfile.
   // Raw pointer because AudioFileReader is non-copyable/non-movable.
   AudioFileReader* reader = nullptr;
   std::string formatName_;

   // Map file extension to human-readable format name.
   //
   // Domain context: libsndfile uses integer constants for format types.
   // We map these to human-readable strings (e.g., "AIFF", "WAV")
   // for user feedback.
   static std::string getFormatName(const std::string& path) {
      std::string lowerPath = path;
      for (auto& c : lowerPath) {
         c = static_cast<char>(
            std::tolower(static_cast<unsigned char>(c)));
      }

      if (lowerPath.size() >= 5 &&
          lowerPath.substr(lowerPath.size() - 5) == ".aiff") {
         return "AIFF";
      } else if (lowerPath.size() >= 4 &&
                 lowerPath.substr(lowerPath.size() - 4) == ".wav") {
         return "WAV";
      } else if (lowerPath.size() >= 5 &&
                 lowerPath.substr(lowerPath.size() - 5) == ".flac") {
         return "FLAC";
      } else if (lowerPath.size() >= 4 &&
                 lowerPath.substr(lowerPath.size() - 4) == ".ogg") {
         return "OGG";
      } else {
         return "unknown";
      }
   }
};

// ============================================================================
// AudioFile implementation
// ============================================================================

AudioFile::AudioFile(const std::string& path)
   : impl_(std::make_unique<Impl>()) {
   // Open the audio file. Throws std::runtime_error if the file cannot
   // be opened or is not a supported audio format.
   impl_->reader = new AudioFileReader(path);
   impl_->formatName_ = Impl::getFormatName(path);
}

AudioFile::~AudioFile() {
   delete impl_->reader;
}

// Destructor handled above.

uint32_t AudioFile::sampleRate() const {
   // Return the sample rate from the underlying reader.
   return impl_->reader->sampleRate();
}

uint32_t AudioFile::channels() const {
   // Return the number of channels from the underlying reader.
   return impl_->reader->channels();
}

uint32_t AudioFile::totalFrames() const {
   // Return the total number of frames from the underlying reader.
   return impl_->reader->totalFrames();
}

std::string AudioFile::formatName() const {
   // Return the format name string.
   return impl_->formatName_;
}

uint32_t AudioFile::readMono(float* buffer, uint32_t hopSize) {
   // Read a block of samples (monophonic).
   // Downmixes stereo to mono by averaging left and right channels.
   return impl_->reader->readMono(buffer, hopSize);
}

bool AudioFile::eof() const {
   // Check if the file is at EOF.
   return impl_->reader->eof();
}

void AudioFile::reset() {
   // Reset to the beginning of the file.
   impl_->reader->reset();
}

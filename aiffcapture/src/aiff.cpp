/**
 * @file aiff.cpp
 * @brief AIFF file format writing via libsndfile.
 *
 * This file implements the AiffWriter class. It wraps libsndfile to write
 * standard AIFF files (80-bit extended float sample rate, 16-bit PCM).
 * libsndfile handles all chunk formatting (FORM, COMM, SSND), byte-order
 * conversion, and file finalization.
 *
 * Key design decisions:
 *
 * 1. **libsndfile**: Delegates all AIFF format logic to libsndfile.
 *    Writes standard AIFF that ffprobe, QuickTime, and all standard
 *    tools accept directly.
 *
 * 2. **16-bit PCM**: Core Audio outputs 32-bit float. The output
 *    callback (main.cpp) converts to 16-bit signed integer and writes
 *    via sf_write_short().
 *
 * 3. **RAII**: The AiffWriter acquires the SNDFILE* in open() and
 *    releases it in close() (or the destructor).
 */

#include <aiffcapture/aiff.h>
#include <sndfile.h>

// ============================================================================
// AiffWriter implementation
// ============================================================================

AiffWriter::~AiffWriter() {
   if (sndfile_ != nullptr) {
      sf_close(sndfile_);
   }
}

AiffWriter::AiffWriter(AiffWriter&& other) noexcept
   : sndfile_(other.sndfile_)
   , format_(std::move(other.format_))
   , framesWritten_(other.framesWritten_) {
   other.sndfile_ = nullptr;
   other.framesWritten_ = 0;
}

AiffWriter& AiffWriter::operator=(AiffWriter&& other) noexcept {
   if (this != &other) {
      if (sndfile_ != nullptr) {
         sf_close(sndfile_);
      }
      sndfile_ = other.sndfile_;
      format_ = std::move(other.format_);
      framesWritten_ = other.framesWritten_;
      other.sndfile_ = nullptr;
      other.framesWritten_ = 0;
   }
   return *this;
}

bool AiffWriter::open(const std::string& filePath, const AudioFormat& format) {
   if (sndfile_ != nullptr) {
      return false;
   }

   SF_INFO info;
   info.samplerate = static_cast<int>(format.sampleRate);
   info.channels = static_cast<int>(format.channels);
   info.format = SF_FORMAT_AIFF | SF_FORMAT_PCM_16;

   sndfile_ = sf_open(filePath.c_str(), SFM_WRITE, &info);
   if (sndfile_ == nullptr) {
      return false;
   }

   format_ = format;
   framesWritten_ = 0;
   return true;
}

bool AiffWriter::writeSamples(const int16_t* data, uint32_t numFrames) {
   if (sndfile_ == nullptr || data == nullptr || numFrames == 0) {
      return false;
   }

   sf_count_t written = sf_write_short(sndfile_, data, numFrames);
   if (written != static_cast<sf_count_t>(numFrames)) {
      return false;
   }

   framesWritten_ += numFrames;
   return true;
}

bool AiffWriter::close() {
   if (sndfile_ == nullptr) {
      return false;
   }

   if (sf_close(sndfile_) != 0) {
      return false;
   }

   sndfile_ = nullptr;
   return true;
}

uint64_t AiffWriter::getBytesWritten() const {
   return framesWritten_ * format_.bytesPerFrame;
}

bool AiffWriter::isOpen() const {
   return sndfile_ != nullptr;
}

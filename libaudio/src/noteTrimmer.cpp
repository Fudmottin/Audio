/**
 * @file noteTrimmer.cpp
 * @brief Implementation of NoteTrimmer — trim detected notes to musical
 *        boundaries.
 *
 * This module refines detected note durations by trimming them to
 * musical boundaries (note off events, silence gaps).
 *
 */

#include <algorithm>
#include <cmath>
#include <libaudio/audioFile.h>
#include <libaudio/hir.h>
#include <libaudio/noteTrimmer.h>
#include <libaudio/spectral.h>
#include <vector>

// ============================================================================
// NoteTrimmer::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All note trimming logic is isolated here.
//
// RAII — no external resources to manage.
// ============================================================================
struct NoteTrimmer::Impl {
   uint32_t sampleRate;
   float silenceThreshold = -40.0f;
   double minNoteDuration = 0.05; // 50 ms default

   Impl(uint32_t sampleRate)
      : sampleRate(sampleRate) {}
   ~Impl() = default;

   // Find the note-off point by looking for a sustained drop in energy
   // below the silence threshold.
   double findNoteOff(const std::vector<float>& samples, uint32_t startFrame,
                      uint32_t totalFrames, float silenceThresholdDb) const {
      // Find the frame where energy drops below the
      // silence threshold, indicating the note has ended.

      constexpr uint32_t WINDOW_SIZE = 2048;
      constexpr uint32_t HOP_SIZE = WINDOW_SIZE / 4;

      // Convert dB threshold to RMS amplitude.
      // RMS = 10^(dB/20). The std::pow overload taking a float
      // exponent is not available, so cast explicitly to double.
      float rmsThreshold =
         static_cast<float>(std::pow(10.0, static_cast<double>(silenceThresholdDb) / 20.0));

      uint32_t currentFrame = startFrame;

      while (currentFrame + WINDOW_SIZE < totalFrames) {
         // Compute RMS energy for the current window.
         double sumSquares = 0.0;
         for (uint32_t i = 0; i < WINDOW_SIZE; ++i) {
            sumSquares += static_cast<double>(samples[currentFrame + i]) *
                          samples[currentFrame + i];
         }

         float rms = static_cast<float>(std::sqrt(sumSquares / WINDOW_SIZE));

         // If RMS drops below threshold, this is the
         // note-off point.
         if (rms < rmsThreshold) {
            return static_cast<double>(currentFrame + WINDOW_SIZE / 2) /
                   sampleRate;
         }

         currentFrame += HOP_SIZE;
      }

      // If no silence found, return the end of the file.
      return static_cast<double>(totalFrames) / sampleRate;
   }
};

// ============================================================================
// NoteTrimmer implementation
// RAII resource management — no external resources.
// ============================================================================

NoteTrimmer::NoteTrimmer(uint32_t sampleRate)
   : impl_(std::make_unique<Impl>(sampleRate)) {
   // Constructor.
}

NoteTrimmer::~NoteTrimmer() = default;

NoteTrimmer::NoteTrimmer(NoteTrimmer&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ =
      std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
}

NoteTrimmer& NoteTrimmer::operator=(NoteTrimmer&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ =
         std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
   }
   return *this;
}

std::vector<Note> NoteTrimmer::refine(const std::vector<Note>& notes,
                                      AudioFileReader& reader) {
   // Refine note durations by trimming to musical
   // boundaries.

   if (impl_ == nullptr || reader.totalFrames() == 0) {
      return notes;
   }

   std::vector<Note> refined = notes;

   // Read the entire audio file into memory for analysis.
   uint32_t totalFrames = reader.totalFrames();
   std::vector<float> audioData(totalFrames);
   reader.readMono(audioData.data(), totalFrames);

   // Reset the reader to the beginning.
   reader.reset();

   // Refine each note's endTime.
   for (auto& note : refined) {
      uint32_t startFrame =
         static_cast<uint32_t>(note.startTime * impl_->sampleRate);

      // Find the actual note-off point.
      double actualOffTime =
         impl_->findNoteOff(audioData, startFrame, totalFrames,
                            impl_->silenceThreshold);

      // Only refine if the new endTime is before the
      // original endTime (trimming, not extending).
      double newEndTime =
         std::min(actualOffTime,
                  static_cast<double>(totalFrames) / impl_->sampleRate);

      // Enforce minimum note duration.
      double duration = newEndTime - note.startTime;
      if (duration < impl_->minNoteDuration) {
         // Note is too short — remove it (silence/rest).
         note.startTime = 0.0;
         note.endTime = 0.0;
      } else {
         note.endTime =
            static_cast<double>(
               std::max(startFrame, static_cast<uint32_t>(newEndTime *
                                                          impl_->sampleRate))) /
            impl_->sampleRate;
      }
   }

   return refined;
}

void NoteTrimmer::setSilenceThreshold(float db) {
   // Set the silence threshold (in dB).

   if (impl_) {
      impl_->silenceThreshold = db;
   }
}

float NoteTrimmer::silenceThreshold() const {
   // Return the current silence threshold.

   return impl_ ? impl_->silenceThreshold : -40.0f;
}

void NoteTrimmer::setMinNoteDuration(double seconds) {
   // Set the minimum note duration (in seconds).

   if (impl_) {
      impl_->minNoteDuration = std::max(0.0, seconds);
   }
}

double NoteTrimmer::minNoteDuration() const {
   // Return the minimum note duration.

   return impl_ ? impl_->minNoteDuration : 0.05;
}

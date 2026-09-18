/**
 * @file noteTrimmer.cpp
 * @brief Implementation of NoteTrimmer — trim detected notes to musical
 *        boundaries.
 *
 * This module refines detected note durations by trimming them to
 * musical boundaries (note off events, silence gaps).
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 */

#include <libaudio/noteTrimmer.h>
#include <libaudio/hir.h>
#include <libaudio/audioFile.h>
#include <libaudio/spectral.h>
#include <cmath>
#include <algorithm>
#include <vector>

// ============================================================================
// NoteTrimmer::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All note trimming logic is isolated here.
//
// Core Guidelines: RAII — no external resources to manage.
// ============================================================================
struct NoteTrimmer::Impl {
   uint32_t sampleRate;
   float silenceThreshold = -40.0f;
   double minNoteDuration = 0.05;  // 50 ms default

   Impl(uint32_t sampleRate) : sampleRate(sampleRate) {}
   ~Impl() = default;

   // Find the note-off point by looking for a sustained drop in energy
   // below the silence threshold.
   double findNoteOff(const std::vector<float>& samples,
                      uint32_t startFrame, uint32_t totalFrames,
                      float silenceThresholdDb) const {
      // Core Guidelines: find the frame where energy drops below the
      // silence threshold, indicating the note has ended.

      constexpr uint32_t WINDOW_SIZE = 2048;
      constexpr uint32_t HOP_SIZE = WINDOW_SIZE / 4;

      // Core Guidelines: convert dB threshold to RMS amplitude.
      // RMS = 10^(dB/20).
      float rmsThreshold = std::pow(10.0, silenceThresholdDb / 20.0);

      uint32_t currentFrame = startFrame;

      while (currentFrame + WINDOW_SIZE < totalFrames) {
         // Core Guidelines: compute RMS energy for the current window.
         double sumSquares = 0.0;
         for (uint32_t i = 0; i < WINDOW_SIZE; ++i) {
            sumSquares += static_cast<double>(samples[currentFrame + i]) *
                          samples[currentFrame + i];
         }

         float rms = static_cast<float>(std::sqrt(sumSquares / WINDOW_SIZE));

         // Core Guidelines: if RMS drops below threshold, this is the
         // note-off point.
         if (rms < rmsThreshold) {
            return static_cast<double>(currentFrame + WINDOW_SIZE / 2) /
                   sampleRate;
         }

         currentFrame += HOP_SIZE;
      }

      // Core Guidelines: if no silence found, return the end of the file.
      return static_cast<double>(totalFrames) / sampleRate;
   }
};

// ============================================================================
// NoteTrimmer implementation
// Core Guidelines: RAII resource management — no external resources.
// ============================================================================

NoteTrimmer::NoteTrimmer(uint32_t sampleRate)
   : impl_(std::make_unique<Impl>(sampleRate)) {
   // Core Guidelines: constructor.
}

NoteTrimmer::~NoteTrimmer() = default;

NoteTrimmer::NoteTrimmer(NoteTrimmer&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
}

NoteTrimmer& NoteTrimmer::operator=(NoteTrimmer&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
   }
   return *this;
}

std::vector<Note> NoteTrimmer::refine(const std::vector<Note>& notes,
                                      AudioFileReader& reader) {
   // Core Guidelines: refine note durations by trimming to musical
   // boundaries.

   if (impl_ == nullptr || reader.totalFrames() == 0) {
      return notes;
   }

   std::vector<Note> refined = notes;

   // Core Guidelines: read the entire audio file into memory for analysis.
   uint32_t totalFrames = reader.totalFrames();
   std::vector<float> audioData(totalFrames);
   reader.readMono(audioData.data(), totalFrames);

   // Core Guidelines: reset the reader to the beginning.
   reader.reset();

   // Core Guidelines: refine each note's endTime.
   for (auto& note : refined) {
      uint32_t startFrame = static_cast<uint32_t>(
         note.startTime * impl_->sampleRate);
      uint32_t endFrame = static_cast<uint32_t>(
         note.endTime * impl_->sampleRate);

      // Core Guidelines: find the actual note-off point.
      double actualOffTime = impl_->findNoteOff(
         audioData, startFrame, totalFrames, impl_->silenceThreshold);

      // Core Guidelines: only refine if the new endTime is before the
      // original endTime (trimming, not extending).
      double newEndTime = std::min(actualOffTime,
                                    static_cast<double>(totalFrames) / impl_->sampleRate);

      // Core Guidelines: enforce minimum note duration.
      double duration = newEndTime - note.startTime;
      if (duration < impl_->minNoteDuration) {
         // Note is too short — remove it (silence/rest).
         note.startTime = 0.0;
         note.endTime = 0.0;
      } else {
         note.endTime = static_cast<double>(
            std::max(startFrame, static_cast<uint32_t>(newEndTime * impl_->sampleRate))) /
                        impl_->sampleRate;
      }
   }

   return refined;
}

void NoteTrimmer::setSilenceThreshold(float db) {
   // Core Guidelines: set the silence threshold (in dB).

   if (impl_) {
      impl_->silenceThreshold = db;
   }
}

float NoteTrimmer::silenceThreshold() const {
   // Core Guidelines: return the current silence threshold.

   return impl_ ? impl_->silenceThreshold : -40.0f;
}

void NoteTrimmer::setMinNoteDuration(double seconds) {
   // Core Guidelines: set the minimum note duration (in seconds).

   if (impl_) {
      impl_->minNoteDuration = std::max(0.0, seconds);
   }
}

double NoteTrimmer::minNoteDuration() const {
   // Core Guidelines: return the minimum note duration.

   return impl_ ? impl_->minNoteDuration : 0.05;
}

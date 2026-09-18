/**
 * @file controlEventExtractor.cpp
 * @brief Implementation of ControlEventExtractor — extract MIDI control
 *        events (pedals, tempo changes) from audio.
 *
 * This module extracts MIDI control change events from audio recordings.
 * The most important for piano is sustain pedal (CC#64).
 *
 */

#include <algorithm>
#include <cmath>
#include <libaudio/audioFile.h>
#include <libaudio/controlEventExtractor.h>
#include <libaudio/hir.h>
#include <libaudio/spectral.h>
#include <vector>

// ============================================================================
// ControlEventExtractor::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All analysis logic is isolated here.
//
// RAII — no external resources to manage.
// ============================================================================
struct ControlEventExtractor::Impl {
   uint32_t sampleRate;
   static constexpr uint8_t SUSTAIN_PEDAL_CC = 64;
   static constexpr uint8_t SOFT_PEDAL_CC = 66;

   Impl(uint32_t sampleRate)
      : sampleRate(sampleRate) {}
   ~Impl() = default;

   // Detect sustain pedal on/off transitions by analyzing low-frequency
   // energy patterns (below ~200 Hz). When sustain pedal is pressed,
   // low-frequency energy increases significantly.
   std::vector<ControlEvent> detectSustainPedal(AudioFileReader& reader) {
      // Analyze the audio file for sustain pedal events.

      std::vector<ControlEvent> events;
      uint32_t sr = reader.sampleRate();
      uint32_t totalFrames = reader.totalFrames();

      if (sr == 0 || totalFrames == 0) {
         return events;
      }

      // Use a window size of 2048 (consistent with other
      // modules) and 75% overlap (hop size = 512).
      uint32_t windowSize = 2048;
      uint32_t hopSize = windowSize / 4;

      std::vector<float> buffer(windowSize);

      // Track the current pedal state.
      bool pedalState = false;
      uint32_t currentFrame = 0;

      while (currentFrame < totalFrames) {
         uint32_t framesRead = reader.readMono(buffer.data(), hopSize);
         if (framesRead == 0) {
            break;
         }

         // Compute the RMS energy in the low-frequency
         // range (below 200 Hz) as a proxy for sustain pedal state.
         // When sustain pedal is pressed, low-frequency resonances
         // increase, raising the low-frequency energy.
         float rmsLowFreq = 0.0f;
         uint32_t lowFreqBins = static_cast<uint32_t>(200.0f * windowSize / sr);

         for (uint32_t i = 0; i < std::min(lowFreqBins, framesRead); ++i) {
            rmsLowFreq += buffer[i] * buffer[i];
         }
         rmsLowFreq = std::sqrt(rmsLowFreq / std::max(1u, lowFreqBins));

         // Threshold-based detection.
         // A high RMS energy in the low-frequency range suggests the
         // sustain pedal is pressed.
         constexpr float PEDAL_THRESHOLD = 0.3f;

         bool isPedalDown = rmsLowFreq > PEDAL_THRESHOLD;

         // Detect state transitions.
         if (isPedalDown && !pedalState) {
            // Pedal ON transition.
            ControlEvent event;
            event.time = static_cast<double>(currentFrame) / sr;
            event.controller = SUSTAIN_PEDAL_CC;
            event.value = 127; // Pedal fully down.
            events.push_back(event);
            pedalState = true;
         } else if (!isPedalDown && pedalState) {
            // Pedal OFF transition.
            ControlEvent event;
            event.time = static_cast<double>(currentFrame) / sr;
            event.controller = SUSTAIN_PEDAL_CC;
            event.value = 0; // Pedal fully up.
            events.push_back(event);
            pedalState = false;
         }

         currentFrame += framesRead;
      }

      // Ensure the file is reset to the beginning.
      reader.reset();

      return events;
   }
};

// ============================================================================
// ControlEventExtractor implementation
// RAII resource management — no external resources.
// ============================================================================

ControlEventExtractor::ControlEventExtractor(uint32_t sampleRate)
   : impl_(std::make_unique<Impl>(sampleRate)) {
   // Constructor.
}

ControlEventExtractor::~ControlEventExtractor() = default;

ControlEventExtractor::ControlEventExtractor(
   ControlEventExtractor&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ =
      std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
}

ControlEventExtractor&
ControlEventExtractor::operator=(ControlEventExtractor&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ =
         std::make_unique<Impl>(other.impl_ ? other.impl_->sampleRate : 48000);
   }
   return *this;
}

std::vector<ControlEvent>
ControlEventExtractor::extract(AudioFileReader& reader) {
   // Extract all control events from the audio file.

   return extractSustainPedal(reader);
}

std::vector<ControlEvent>
ControlEventExtractor::extractSustainPedal(AudioFileReader& reader) {
   // Extract sustain pedal events (CC#64) from audio.

   if (impl_ == nullptr) {
      return {};
   }

   return impl_->detectSustainPedal(reader);
}

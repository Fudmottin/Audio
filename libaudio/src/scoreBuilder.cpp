/**
 * @file scoreBuilder.cpp
 * @brief Implementation of ScoreBuilder — assemble Note + ControlEvent
 *        into a Score (HIR).
 *
 * This module assembles detected notes and control events into a
 * complete Score (HIR). It is the final step of the transcription
 * pipeline before MIDI export.
 *
 */

#include <algorithm>
#include <cmath>
#include <libaudio/hir.h>
#include <libaudio/scoreBuilder.h>

namespace libaudio {

// ============================================================================
// ScoreBuilder::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All score assembly logic is isolated here.
//
// RAII — no external resources to manage.
// ============================================================================
struct ScoreBuilder::Impl {
   std::vector<Note> notes;
   std::vector<ControlEvent> controls;
   double tempo = 120.0;
   std::string title;
   std::string composer;

   ~Impl() = default;

   // Estimate tempo from note timing (average note duration).
   double estimateTempo() const {
      // Estimate tempo from the average note duration.
      // If notes are about 0.5 seconds apart, that's roughly 120 BPM.
      if (notes.size() < 2) {
         return 120.0; // Default tempo.
      }

      double totalDuration = 0.0;
      uint32_t noteCount = 0;

      for (const auto& note : notes) {
         double duration = note.endTime - note.startTime;
         if (duration > 0.01) { // Ignore very short notes (noise).
            totalDuration += duration;
            noteCount++;
         }
      }

      if (noteCount == 0) {
         return 120.0; // Default tempo.
      }

      // Average note duration → BPM estimate.
      // Assuming quarter notes: BPM = 60 / avgDuration.
      double avgDuration = totalDuration / noteCount;
      return 60.0 / std::max(0.01, avgDuration);
   }
};

// ============================================================================
// ScoreBuilder implementation
// RAII resource management — no external resources.
// ============================================================================

ScoreBuilder::ScoreBuilder()
   : impl_(std::make_unique<Impl>()) {
   // Constructor.
}

ScoreBuilder::~ScoreBuilder() = default;

ScoreBuilder::ScoreBuilder(ScoreBuilder&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

ScoreBuilder& ScoreBuilder::operator=(ScoreBuilder&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

void ScoreBuilder::addNote(Note note) {
   // Add a note to the score.

   if (impl_) {
      impl_->notes.push_back(std::move(note));
   }
}

void ScoreBuilder::addControlEvent(ControlEvent event) {
   // Add a control event to the score.

   if (impl_) {
      impl_->controls.push_back(std::move(event));
   }
}

void ScoreBuilder::setTempo(double bpm) {
   // Set the score tempo (BPM).

   if (impl_) {
      impl_->tempo = std::max(20.0, std::min(300.0, bpm));
   }
}

void ScoreBuilder::setTitle(std::string title) {
   // Set the score title.

   if (impl_) {
      impl_->title = std::move(title);
   }
}

void ScoreBuilder::setComposer(std::string composer) {
   // Set the score composer.

   if (impl_) {
      impl_->composer = std::move(composer);
   }
}

Score ScoreBuilder::build() const {
   // Build and return the final Score (HIR).

   Score score;

   // Clone and sort notes by startTime.
   score.notes = impl_->notes;
   std::sort(score.notes.begin(), score.notes.end(),
             [](const Note& a, const Note& b) {
                return a.startTime < b.startTime;
             });

   // Merge control events.
   score.controls = impl_->controls;
   std::sort(score.controls.begin(), score.controls.end(),
             [](const ControlEvent& a, const ControlEvent& b) {
                return a.time < b.time;
             });

   // Set metadata.
   score.tempo = impl_->tempo;
   score.title = impl_->title;
   score.composer = impl_->composer;

   // Estimate tempo from note timing if not explicitly set.
   // (We can't tell if tempo was explicitly set, so we always use the
   // stored value, which defaults to 120.0.)

   return score;
}

uint32_t ScoreBuilder::noteCount() const {
   // Simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->notes.size()) : 0;
}

uint32_t ScoreBuilder::controlEventCount() const {
   // Simple accessor.
   return impl_ ? static_cast<uint32_t>(impl_->controls.size()) : 0;
}

} // namespace libaudio

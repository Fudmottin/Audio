/**
 * @file scoreBuilder.h
 * @brief Assemble Note + ControlEvent into a Score (HIR).
 *
 * This module assembles detected notes and control events into a
 * complete Score (HIR). It is the final step of the transcription
 * pipeline before MIDI export.
 *
 */

#ifndef LIBAUDIO_SCOREBUILDER_H
#define LIBAUDIO_SCOREBUILDER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace libaudio {

// Forward declarations (from hir.h).
struct Note;
struct ControlEvent;
struct Score;

// ============================================================================
// ScoreBuilder — Assemble Note + ControlEvent into a Score (HIR).
//
// Domain context: The ScoreBuilder is the final step of the audio
// analysis pipeline. It takes raw notes (from NoteDetector or manual
// analysis) and control events (from ControlEventExtractor) and
// assembles them into a complete Score (HIR).
//
// Key design decisions:
// - Sorts notes by startTime for proper ordering.
// - Merges control events into the Score's controls vector.
// - Estimates tempo from note timing if not provided.
// - Sets default metadata (title, composer).
//
// RAII resource management — no external resources.
// ============================================================================
class ScoreBuilder {
 public:
   // Create a ScoreBuilder.
   ScoreBuilder();

   // Destructor.
   // RAII — no external resources to release.
   ~ScoreBuilder();

   // Non-copyable (stateful object).
   ScoreBuilder(const ScoreBuilder&) = delete;
   ScoreBuilder& operator=(const ScoreBuilder&) = delete;

   // Movable.
   ScoreBuilder(ScoreBuilder&& other) noexcept;
   ScoreBuilder& operator=(ScoreBuilder&& other) noexcept;

   // Add a note to the score.
   //
   // @param note Note to add.
   void addNote(Note note);

   // Add a control event to the score.
   //
   // @param event Control event to add.
   void addControlEvent(ControlEvent event);

   // Set the score tempo (BPM).
   //
   // @param bpm Tempo in BPM. Default: 120.0.
   void setTempo(double bpm);

   // Set the score title.
   //
   // @param title Score title.
   void setTitle(std::string title);

   // Set the score composer.
   //
   // @param composer Composer name.
   void setComposer(std::string composer);

   // Build and return the final Score (HIR).
   //
   // Sorts notes by startTime, merges all data into a Score,
   // and estimates tempo from note timing if not explicitly set.
   //
   // @return Complete Score (HIR) ready for MIDI export.
   Score build() const;

   // Get the number of notes added so far.
   //
   // @return Number of notes.
   [[nodiscard]] uint32_t noteCount() const;

   // Get the number of control events added so far.
   //
   // @return Number of control events.
   [[nodiscard]] uint32_t controlEventCount() const;

 private:
   // Private implementation — all score assembly logic is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_SCOREBUILDER_H

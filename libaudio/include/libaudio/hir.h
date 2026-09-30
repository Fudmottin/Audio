/**
 * @file hir.h
 * @brief High-level Instrumentation Representation — the single source of
 *        truth for the audio-to-MIDI transcription pipeline.
 *
 * The HIR is a pure C++ data structure with no dependencies on aubio,
 * Core Audio, AIFF, MIDI, or LilyPond. It is the intermediate language
 * between audio analysis and both MIDI file output and LilyPond source
 * output.
 *
 * From a single `Score`, you can generate:
 * - A `.mid` file (Standard MIDI File format — documented in `MIDI.md`)
 * - A `.ly` file (LilyPond source — documented in `LilyPond.md`)
 * - A JSON export for editing in a custom tool
 * - A CSV for spreadsheet analysis
 *
 * The HIR is the **single source of truth**. The MIDI writer and LilyPond
 * exporter are just **renderers** for the same data.
 *
 * @section hir-data-structures Data Structures
 *
 * The HIR defines three structures:
 *
 * 1. `Note` — A single note event (pitch, velocity, timing, channel, sustain,
 *    pitch-bend).
 * 2. `ControlEvent` — A control change event (pedals, tempo changes, etc.).
 * 3. `Score` — A complete score (notes + controls + metadata).
 *
 * Key design decisions:
 * - `startTime` and `endTime` are in **seconds** (not MIDI ticks). This
 *   makes them platform-independent and human-readable. Conversion to
 *   MIDI ticks happens in the MIDI writer (using 480 ticks per quarter
 *   note, as documented in `MIDI.md`).
 * - `pitch` is a `uint8_t` (MIDI note number 0–127). For piano, valid
 *   range is 21 (A0) to 108 (C8).
 * - `velocity` is a `uint8_t` (0–127). Derived from RMS energy of the
 *   note segment during analysis.
 * - `channel` defaults to 0 (MIDI channel 0, Acoustic Grand Piano).
 * - `sustain` is a boolean flag derived from pedal detection analysis.
 *
 * @section hir-timing Timing in Seconds vs MIDI Ticks
 *
 * The HIR stores all timing in seconds (double precision). This is
 * intentional: it decouples the representation from any specific MIDI
 * tick rate or tempo. The MIDI writer converts seconds to ticks using:
 *
 *   ticks = round(seconds × (ticksPerQuarterNote / 60) × (tempoBPM / 60))
 *
 * With 480 ticks per quarter note and 120 BPM:
 *   ticks = seconds × 960
 *
 */

#ifndef LIBAUDIO_HIR_H
#define LIBAUDIO_HIR_H

#include <cstdint>
#include <string>
#include <vector>

namespace libaudio {

// ============================================================================
// Note — A single note event — the atomic unit of musical representation.
//
// Domain context: A Note represents one key press on a piano (or other
// instrument). It captures the pitch (MIDI note number), velocity (how
// hard the key was struck), timing (start and end in seconds), channel
// (MIDI channel, default 0 for piano), whether it overlaps with sustain
// pedal, and any pitch-bend (a per-frame deviation from the base pitch,
// in MIDI ticks; empty = a constant-pitch note).
//
// Key design decisions:
// - `startTime` and `endTime` are in **seconds** (not MIDI ticks). This
//   makes the representation platform-independent and human-readable.
// - `pitch` is a `uint8_t` (MIDI note number 0–127). For piano, valid
//   range is 21 (A0) to 108 (C8).
// - `velocity` is a `uint8_t` (0–127). Derived from RMS energy of the
//   note segment during analysis.
// - `channel` defaults to 0 (MIDI channel 0, Acoustic Grand Piano).
// - `sustain` is a boolean flag derived from pedal detection analysis.
// - `pitchBends` is a vector of MIDI pitch-bend ticks (one per frame); empty
//   means no bend. It is the HIR home for the fine-pitch contour the model
//   emits, converted to MIDI 14-bit ticks by the post-processor.
//
// Aggregate type with no hidden state or side effects.
// ============================================================================
struct Note {
   /**
    * Start time in seconds from the beginning of the recording.
    * Double precision for sub-sample accuracy.
    */
   double startTime = 0.0;

   /**
    * End time in seconds from the beginning of the recording.
    * Double precision for sub-sample accuracy.
    */
   double endTime = 0.0;

   /**
    * MIDI note number (0–127). For piano, valid range is 21 (A0) to
    * 108 (C8).
    * Uint8_t because MIDI note numbers fit in 7 bits.
    */
   uint8_t pitch = 60; // Default: middle C

   /**
    * Note velocity (0–127). Derived from RMS energy of the note segment
    * during analysis.
    * Uint8_t because MIDI velocity fits in 7 bits.
    */
   uint8_t velocity = 100; // Default: medium velocity

   /**
    * MIDI channel (0–15). Default is 0 (channel 1, Acoustic Grand Piano).
    * Uint8_t because MIDI channels fit in 4 bits.
    */
   uint8_t channel = 0;

   /**
    * True if this note overlaps with sustain pedal. Derived from pedal
    * detection analysis.
    * Explicit boolean semantics.
    */
   bool sustain = false;

   /**
    * Pitch-bend values for this note, in MIDI pitch-bend ticks
    * (-8192..8191), where 0 is the note's center (base) pitch. Each entry is
    * the *deviation* from `pitch` for one frame of the note; the MIDI writer
    * spreads them evenly across the note's duration as 0xE0 messages. An empty
    * vector means the note has no pitch bend (a constant-pitch note).
    * int16_t because a MIDI pitch bend is 14 bits (8192 steps either side of
    * center); by General MIDI convention the full scale is 2 semitones, so
    * 4096 ticks is one semitone.
    */
   std::vector<int16_t> pitchBends;

   // Default copy/move semantics are fine for this aggregate type (the
   // pitchBends vector moves/copy-constructs like any other member).
   Note() = default;
   Note(const Note&) = default;
   Note(Note&&) = default;
   Note& operator=(const Note&) = default;
   Note& operator=(Note&&) = default;
};

// ============================================================================
// ControlEvent — A MIDI control change event (pedals, tempo changes, etc.).
//
// Domain context: Control events represent MIDI Control Change messages.
// The most important for piano is CC#64 (sustain pedal). Other events
// include CC#66 (soft pedal), CC#67 (sostenuto), CC#11 (expression),
// and tempo changes.
//
// Key design decisions:
// - `time` is in **seconds** (not MIDI ticks). Conversion happens in
//   the MIDI writer.
// - `controller` is the CC# (64 = sustain pedal, 66 = soft pedal, etc.).
// - `value` is 0–127 (pedal position: 0 = up, 127 = down).
//
// Aggregate type with no hidden state or side effects.
// ============================================================================
struct ControlEvent {
   /**
    * Time in seconds from the beginning of the recording.
    * Double precision for sub-sample accuracy.
    */
   double time = 0.0;

   /**
    * MIDI Control Change number (0–127). Common values:
    * - 64 = Sustain Pedal (most important for piano)
    * - 66 = Soft Pedal
    * - 67 = Sostenuto Pedal
    * - 11 = Expression
    * - 123 = All Notes Off (safety reset)
    * Uint8_t because MIDI CC numbers fit in 7 bits.
    */
   uint8_t controller = 0;

   /**
    * Controller value (0–127). For sustain pedal: 0 = up, 127 = down.
    * Values 1–63 represent half-pedal.
    * Uint8_t because MIDI controller values fit in 7 bits.
    */
   uint8_t value = 0;

   // Default copy/move semantics are fine for this aggregate type.
   ControlEvent() = default;
   ControlEvent(const ControlEvent&) = default;
   ControlEvent(ControlEvent&&) = default;
   ControlEvent& operator=(const ControlEvent&) = default;
   ControlEvent& operator=(ControlEvent&&) = default;
};

// ============================================================================
// Score — A complete score — the output of the transcription pipeline.
//
// Domain context: The Score is the top-level HIR structure. It owns all
// notes and control events. It is the single source of truth for both
// MIDI file output and LilyPond source output.
//
// Key design decisions:
// - `tempo` is in BPM (quarter notes per minute). Default 120.0.
// - `title` and `composer` are optional metadata fields.
// - `notes` and `controls` are vectors of Note and ControlEvent.
//
// Aggregate type with no hidden state or side effects.
// ============================================================================
struct Score {
   /** All detected notes. */
   std::vector<Note> notes;

   /** All control change events (pedals, tempo changes, etc.). */
   std::vector<ControlEvent> controls;

   /** Tempo in BPM (quarter notes per minute). Default 120.0. */
   double tempo = 120.0;

   /** Score title (optional). */
   std::string title;

   /** Composer name (optional). */
   std::string composer;

   // Default copy/move semantics are fine for this aggregate type.
   Score() = default;
   Score(const Score&) = default;
   Score(Score&&) = default;
   Score& operator=(const Score&) = default;
   Score& operator=(Score&&) = default;
};

} // namespace libaudio

#endif // LIBAUDIO_HIR_H

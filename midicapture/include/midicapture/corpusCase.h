/**
 * @file corpusCase.h
 * @brief The shared corpus performance spec — one source of truth for both
 *        corpus generators.
 *
 * A `Case` is a small performance: a tempo, a set of (possibly simultaneous)
 * `CaseNote`s (timing in beats), and optional control events (e.g. sustain
 * pedal). It is the single definition both generators consume, so they can
 * never drift apart:
 *   - `--generate-test-midi-files` (main.cpp) writes each case as a `.mid`;
 *   - `--run-corpus --clean` (corpusHarness.cpp) writes the `.mid` and renders
 *     it to the synthetic-voice `.mp3`.
 *
 * `coreCorpus()` is the fixed 14-case monophonic set (the regression gate);
 * it is expressed with the `sequential()` helper. `buildCorpusScore` converts
 * a `Case` to a `libaudio::Score` (beats -> seconds at the case tempo). The
 * header depends only on the HIR, so it is usable in Tier-1 and Tier-2 builds
 * alike (the gate itself is Tier-2; the generated files are not).
 */

#ifndef MIDICAPTURE_CORPUS_CASE_H
#define MIDICAPTURE_CORPUS_CASE_H

#include <cstdint>
#include <libaudio/hir.h>
#include <string>
#include <vector>

// ============================================================================
// CaseNote — one note in a corpus performance.
//
// Domain context: Timing is in *beats* (a beat is one quarter note) so a case
// reads in musical terms; `buildCorpusScore` converts to seconds using the
// case tempo. `pitchBends` is empty for a constant-pitch note; a non-empty
// vector makes the writer emit 0xE0 messages across the note's duration.
// Aggregate type with no hidden state or side effects.
// ============================================================================
struct CaseNote {
   double onsetBeats = 0.0;         // when the note sounds (beats from t=0).
   double durBeats = 1.0;           // note length (beats).
   uint8_t pitch = 60;              // MIDI note number.
   uint8_t velocity = 100;          // MIDI velocity.
   std::vector<int16_t> pitchBends; // 14-bit ticks; empty = constant pitch.
};

// ============================================================================
// Case — one corpus performance.
//
// Domain context: an output filename, a tempo, a set of (possibly
// simultaneous) notes, and optional control events. General enough for
// monophonic runs, polyphonic chords, bent notes, and pedal cases alike; the
// fixed 14-case gate set is expressed with `sequential()`.
// Aggregate type with no hidden state or side effects.
// ============================================================================
struct Case {
   std::string fileName;                         // output file (relative).
   double tempoBpm = 120.0;                      // tempo (BPM).
   std::vector<CaseNote> notes;                  // the performance.
   std::vector<libaudio::ControlEvent> controls; // e.g. sustain pedal.
};

// ============================================================================
// sequential — a compact monophonic run (one pitch at a time).
//
// Builds a `Case` of `pitches` sounded one after another, each `noteBeats`
// long, with an optional `gapBeats` of silence between successive notes and an
// optional per-note `velocities` ladder (one value per pitch; a mismatched size
// falls back to a uniform 100). Note i sounds at beat i*(noteBeats + gapBeats)
// — the same timeline the historical monophonic builder produced, so a
// regenerated asset stays byte-identical.
// ============================================================================
Case sequential(const std::string& fileName, const std::vector<int>& pitches,
                double tempoBpm, double noteBeats,
                const std::vector<int>& velocities = {}, double gapBeats = 0.0);

// ============================================================================
// coreCorpus — the fixed 14-case monophonic corpus (the regression gate set).
//
// Defined once here and shared by `--generate-test-midi-files` and
// `--run-corpus --clean` so the two generators cannot drift apart.
// ============================================================================
std::vector<Case> coreCorpus();

// ============================================================================
// buildCorpusScore — convert a `Case` to a `libaudio::Score`.
//
// Beats -> seconds at the case tempo (seconds = beats * 60 / tempoBpm). Every
// note lands on channel 0 (Acoustic Grand Piano) with `sustain` false; the
// case's control events are copied onto the score.
// ============================================================================
libaudio::Score buildCorpusScore(const Case& c);

#endif // MIDICAPTURE_CORPUS_CASE_H

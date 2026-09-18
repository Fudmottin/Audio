/**
 * @file controlEventExtractor.h
 * @brief Extract MIDI control events (pedals, tempo changes) from audio.
 *
 * This module extracts MIDI control change events from audio recordings.
 * The most important for piano is sustain pedal (CC#64).
 *
 * @section control-events Control Change Events
 *
 * Common CC# values for piano (from `lode/MIDI.md`):
 * - 64 = Sustain Pedal (most important for piano)
 * - 66 = Soft Pedal
 * - 67 = Sostenuto Pedal
 * - 11 = Expression
 * - 123 = All Notes Off (safety reset)
 *
 */

#ifndef LIBAUDIO_CONTROLEVENTEXTRACTOR_H
#define LIBAUDIO_CONTROLEVENTEXTRACTOR_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declarations.
struct ControlEvent;
class AudioFileReader;

// ============================================================================
// ControlEventExtractor — Extract MIDI control events from audio.
//
// Domain context: This class analyzes audio recordings to detect and
// extract MIDI control change events. The most important for piano is
// sustain pedal (CC#64) detection.
//
// Key design decisions:
// - Analyzes low-frequency energy patterns to detect sustain pedal.
// - Returns ControlEvent objects compatible with the HIR.
// - Supports CC#64 (sustain), CC#66 (soft pedal), CC#67 (sostenuto).
//
// RAII resource management — no external resources.
// ============================================================================
class ControlEventExtractor {
 public:
   // Create a ControlEventExtractor.
   //
   // @param sampleRate Sample rate of the input signal.
   explicit ControlEventExtractor(uint32_t sampleRate);

   // Destructor.
   // RAII — no external resources to release.
   ~ControlEventExtractor();

   // Non-copyable (stateful object).
   ControlEventExtractor(const ControlEventExtractor&) = delete;
   ControlEventExtractor& operator=(const ControlEventExtractor&) = delete;

   // Movable.
   ControlEventExtractor(ControlEventExtractor&& other) noexcept;
   ControlEventExtractor& operator=(ControlEventExtractor&& other) noexcept;

   // Extract control events from a recorded audio file.
   //
   // Analyzes the audio file for sustain pedal events (CC#64), soft
   // pedal events (CC#66), and other control change events.
   //
   // @param reader Audio file reader (already opened).
   // @return Vector of ControlEvent objects.
   std::vector<ControlEvent> extract(AudioFileReader& reader);

   // Extract sustain pedal events (CC#64) from audio.
   //
   // Analyzes low-frequency energy patterns to detect sustain pedal
   // on/off transitions. Returns ControlEvent objects with
   // controller = 64 and value = 127 (pedal down) or 0 (pedal up).
   //
   // @param reader Audio file reader (already opened).
   // @return Vector of sustain pedal ControlEvent objects.
   std::vector<ControlEvent> extractSustainPedal(
      AudioFileReader& reader);

 private:
   // Private implementation — all analysis logic is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif // LIBAUDIO_CONTROLEVENTEXTRACTOR_H

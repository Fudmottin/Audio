/**
 * @file transcriber.h
 * @brief The abstract audio → Score port — the analyzer-agnostic seam.
 *
 * A `Transcriber` is anything that turns an audio file path into a HIR
 * `Score`. Two concrete adapters implement it:
 *   - `BasicPitch` (ONNX neural net — Tier-2, polyphonic; in libaudio)
 *   - `AubioTranscriber` (Tier-1 monophonic aubio engine; in midicapture)
 *
 * The port is header-only (no .cpp): it is a pure abstract interface with
 * no state. Concrete adapters live in their own files.
 *
 * @section transcriber-tier2 Tier-2 grouping
 *
 * Part of libaudio's Tier-2 layer; gated behind `LIBAUDIO_HAS_TIER2`.
 */

#ifndef LIBAUDIO_TRANSCRIBER_H
#define LIBAUDIO_TRANSCRIBER_H

#include <libaudio/hir.h>
#include <string>
#include <string_view>

namespace libaudio {

// ============================================================================
// Transcriber — the abstract port for audio → Score transcription.
//
// Any analyzer that can read an audio file and produce a HIR Score is a
// Transcriber. The corpus harness and any future CLI tool can be
// analyzer-agnostic: they take a `const Transcriber&` and call `transcribe`.
//
// Implementations own their model / DSP state; `transcribe` is const because
// a single analyzer instance can transcribe many files.
// ============================================================================
class Transcriber {
 public:
   virtual ~Transcriber() = default;

   /**
    * Transcribe an audio file to a HIR Score.
    *
    * @param path  Path to the input audio file.
    * @return A Score with detected notes, sorted by start time.
    * @throws std::runtime_error if the file cannot be read or the analysis
    *         fails.
    */
   [[nodiscard]] virtual Score transcribe(std::string_view path) const = 0;

   /**
    * A short, stable identifier for this analyzer (e.g. "basic-pitch").
    * Used by the corpus harness to select an analyzer by name.
    */
   [[nodiscard]] virtual std::string name() const = 0;
};

} // namespace libaudio

#endif // LIBAUDIO_TRANSCRIBER_H

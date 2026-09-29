/**
 * @file analyzer.h
 * @brief The abstract audio → Score port — the analyzer-agnostic seam.
 *
 * An `Analyzer` is anything that turns an audio file path into a HIR `Score`.
 * Two concrete analyzers implement it, both in libaudio:
 *   - `Transcriber` (Tier-1 monophonic aubio engine; see transcriber.h)
 *   - `BasicPitch` (Tier-2 ONNX neural net, polyphonic; see basicPitch.h)
 *
 * The port is header-only (no .cpp): it is a pure abstract interface with no
 * state. Concrete analyzers live in their own files.
 *
 * This is the seam the corpus harness (and any future CLI tool) uses to score
 * analyzers interchangeably: take a `const Analyzer&` and call `transcribe`.
 *
 * @section analyzer-tier1 Tier-1 grouping
 *
 * Part of libaudio's Tier-1 layer (always compiled). It depends only on the
 * HIR `Score`, so it carries no ONNX / aubio dependency.
 */

#ifndef LIBAUDIO_ANALYZER_H
#define LIBAUDIO_ANALYZER_H

#include <libaudio/hir.h>
#include <string>
#include <string_view>

namespace libaudio {

// ============================================================================
// Analyzer — the abstract port for audio → Score transcription.
//
// Any component that can read an audio file and produce a HIR Score is an
// Analyzer. Consumers that want to be analyzer-agnostic take a
// `const Analyzer&` and call `transcribe`; the corpus harness selects a
// concrete analyzer by its `name()`.
//
// Implementations own their model / DSP state; `transcribe` is const because
// a single analyzer instance can transcribe many files.
// ============================================================================
class Analyzer {
 public:
   virtual ~Analyzer() = default;

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

#endif // LIBAUDIO_ANALYZER_H

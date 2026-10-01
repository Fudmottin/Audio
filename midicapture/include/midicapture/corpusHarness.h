/**
 * @file corpusHarness.h
 * @brief Analyzer-agnostic evaluation over the 14-file monophonic corpus.
 *
 * `runCorpus` is a C++ port of `midicapture/render_test_suite.py`: for each
 * `.mp3` in a directory it runs the selected analyzer (a
 * `libaudio::Analyzer` — `basic-pitch` or the Tier-1 `aubio` engine),
 * compares the detected notes to the ground-truth `.mid` beside each file, and
 * prints the per-file recall / precision / Δonset / Δdur / Δvel / Δoct /
 * Δchroma table plus the suite summary. It is the *evaluator* half of the
 * test-suite harness; the *renderer* half (synthesizing each performance to a
 * 192 k MP3) is the optional `--clean` regeneration path.
 *
 * This type lives in the global namespace to match the module's other types.
 * It is Tier-2-only: the harness scores analyzers through the `libaudio`
 * `Analyzer` port, which exists only when the build has Tier-2.
 *
 * @section corpus-tier2 Tier-2 grouping
 *
 * Gated behind `LIBAUDIO_HAS_TIER2`.
 */

#ifndef MIDICAPTURE_CORPUS_HARNESS_H
#define MIDICAPTURE_CORPUS_HARNESS_H

#include <cstdint>
#include <libaudio/analyzer.h>
#include <memory>
#include <string>

// The analyzer-agnostic corpus evaluator. Only present in a Tier-2 build.
#ifdef LIBAUDIO_HAS_TIER2

/// The engine-configuration bundle, shared by the direct transcription path
/// (main.cpp) and the corpus evaluator so the model-name-to-engine mapping has
/// one source of truth. The aubio fields tune the Tier-1 monophonic
/// `Transcriber` (ignored by `BasicPitch`, whose window and frame rate are
/// fixed by the model); the pitch-bend fields tune `BasicPitch` (ignored by
/// `Transcriber`, which never bends); `ffmpegPath` is used by the aubio engine
/// (container-decode fallback) and for corpus MP3 encoding. Not used by
/// `BasicPitch` (its decode + resample is in-process via FFmpeg libraries).
struct AnalyzerParams {
   // aubio (Tier-1) tuning — ignored by basic-pitch.
   uint32_t windowSize = 2048;
   uint32_t hopSize = 512;
   float silenceDb = -40.0f;
   std::string pitchMethod = "yinfft";
   double tempoBpm = 120.0;
   // aubio container-decode fallback + corpus MP3 encoding.
   std::string ffmpegPath = "/opt/homebrew/bin/ffmpeg";
   // basic-pitch tuning — ignored by aubio.
   bool includePitchBends = true;
   bool multiplePitchBends = false;
};

/// Select an `Analyzer` by model name.
///
/// @param name  "basic" / "basic-pitch" (synonyms, both select the
///              polyphonic `BasicPitch`) or "aubio" (the Tier-1 monophonic
///              `Transcriber`).
/// @param p     The engine parameters (see `AnalyzerParams`).
/// @throws std::runtime_error if `name` is not a known model.
std::unique_ptr<libaudio::Analyzer> makeAnalyzer(const std::string& name,
                                                 const AnalyzerParams& p);

/// Run the evaluation harness.
///
/// For every `.mp3` in `dir` (sorted), transcribe it with the named analyzer,
/// compare to the `.mid` ground truth beside it, and print the metrics table.
///
/// @param dir           Directory holding the corpus (`*.mp3` + `*.mid`).
/// @param analyzerName  Model to run: "basic"/"basic-pitch" (the polyphonic
///                      neural engine) or "aubio" (the monophonic YIN engine).
/// @param clean         When true, regenerate the 14 assets (`.mid` + `.mp3`)
///                      into `dir` before evaluating (the renderer half).
/// @param ffmpegPath    Path to the ffmpeg executable (decode + mp3 encode).
/// @param includePitchBends  basic-pitch only: whether to extract pitch bends
///                      into `Note::pitchBends`. Ignored by the aubio engine
///                      (it never bends). Default true (Python parity). This
///                      does not change the onset/length/pitch/velocity
///                      metrics (those exclude bends); it only affects the
///                      bend vectors the analyzer attaches to notes.
/// @param multiplePitchBends basic-pitch only: when true, route each distinct
///                      bent pitch to its own MIDI channel (1..15). Ignored by
///                      the aubio engine. Default false (reference: one
///                      channel).
///
/// @return 0 on success, 1 on a setup/missing-tool error, 2 if the run fails
///         partway through (no files could be evaluated).
int runCorpus(const std::string& dir, const std::string& analyzerName,
              bool clean, const std::string& ffmpegPath,
              bool includePitchBends = true, bool multiplePitchBends = false);

#endif // LIBAUDIO_HAS_TIER2

#endif // MIDICAPTURE_CORPUS_HARNESS_H

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

#include <string>

// The analyzer-agnostic corpus evaluator. Only present in a Tier-2 build.
#ifdef LIBAUDIO_HAS_TIER2

/// Run the evaluation harness.
///
/// For every `.mp3` in `dir` (sorted), transcribe it with the named analyzer,
/// compare to the `.mid` ground truth beside it, and print the metrics table.
///
/// @param dir           Directory holding the corpus (`*.mp3` + `*.mid`).
/// @param analyzerName  Analyzer to run: "basic-pitch" or "aubio".
/// @param clean         When true, regenerate the 14 assets (`.mid` + `.mp3`)
///                      into `dir` before evaluating (the renderer half).
/// @param ffmpegPath    Path to the ffmpeg executable (decode + mp3 encode).
/// @param includePitchBends  basic-pitch only: whether to extract pitch bends
///                      into `Note::pitchBends`. Ignored by the aubio engine
///                      (it never bends). Default true (Python parity). This
///                      does not change the onset/length/pitch/velocity
///                      metrics (those exclude bends); it only affects the
///                      bend vectors the analyzer attaches to notes.
///
/// @return 0 on success, 1 on a setup/missing-tool error, 2 if the run fails
///         partway through (no files could be evaluated).
int runCorpus(const std::string& dir, const std::string& analyzerName,
              bool clean, const std::string& ffmpegPath,
              bool includePitchBends = true);

#endif // LIBAUDIO_HAS_TIER2

#endif // MIDICAPTURE_CORPUS_HARNESS_H

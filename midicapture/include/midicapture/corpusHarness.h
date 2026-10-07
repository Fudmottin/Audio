/**
 * @file corpusHarness.h
 * @brief Analyzer-agnostic evaluation over the corpus.
 *
 * `runCorpus` scores a directory of rendered test assets: for each `.mp3` in
 * the directory (sorted) it runs the selected analyzer (a `libaudio::Analyzer`
 * — `basic-pitch` or the Tier-1 `aubio` engine), compares the detected notes
 * to the ground-truth `.mid` beside it, and prints the per-file recall /
 * precision / Δonset / Δdur / Δvel / Δoct / Δchroma table plus the suite
 * summary. The matching + metrics core is factored into
 * `midicapture/noteMatcher.h` (shared with the MAESTRO ground-truth sweep).
 *
 * It is a *pure evaluator*: it reads the `.mp3` + `.mid` already on disk and
 * never synthesizes audio. The test-suite voice is owned by the Python harness
 * (`render_test_suite.py`), which renders each `.mid` with timidity — so the
 * ground truth and its render live in one place and cannot drift apart.
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
/// fixed by the model); the `BasicPitch` fields (note-detection thresholds,
/// the frequency band, the decode gates, the output tempo, and the pitch-bend
/// policy) map 1:1 onto `BasicPitchOptions` and are ignored by `Transcriber`
/// (which never bends). `ffmpegPath` is used by the aubio engine
/// (container-decode fallback) and reported in the corpus header; it is not
/// used by `BasicPitch` (its decode + resample is in-process via FFmpeg
/// libraries). The corpus MP3 encode is the Python harness's job, not this
/// evaluator's.
/// The `BasicPitch` defaults are the no-op baseline, so a run with no tuning
/// stays byte-identical to the reference.
struct AnalyzerParams {
   // aubio (Tier-1) tuning — ignored by basic-pitch.
   uint32_t windowSize = 2048;
   uint32_t hopSize = 512;
   float silenceDb = -40.0f;
   std::string pitchMethod = "yinfft";
   double tempoBpm = 120.0;
   // aubio container-decode fallback + corpus MP3 encoding.
   std::string ffmpegPath = "/opt/homebrew/bin/ffmpeg";
   // basic-pitch tuning — ignored by aubio. These map 1:1 onto
   // `BasicPitchOptions` (see `makeAnalyzer`); the defaults are the no-op
   // baseline, so a no-flag run is byte-identical to the reference.
   float onsetThreshold = 0.5f;   // min onset activation (0..1)
   float frameThreshold = 0.3f;   // min frame activation (0..1)
   double minNoteLenMs = 127.7;   // min note length (ms)
   int velocityScale = 127;       // velocity = clamp(round(scale*amp), 1, 127)
   double minFrequency = 27.5;    // lowest Hz to keep (A0)
   double maxFrequency = 4186.0;  // highest Hz to keep (C8)
   bool inferOnsets = true;       // infer onsets the model missed
   bool melodiaTrick = true;      // the melodia trick
   double midiTempo = 120.0;      // output tempo (bpm)
   double bendDeadbandBins = 1.0; // near-flat bend floor (bins)
   bool includePitchBends = true; // decode the contour into bends
   bool multiplePitchBends = false; // route bent notes to distinct channels
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
/// @param ffmpegPath    Path to the ffmpeg executable (reported; the aubio
///                      engine uses it for container-decode). Not used by
///                      basic-pitch (its decode + resample is in-process).
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
              const std::string& ffmpegPath, bool includePitchBends = true,
              bool multiplePitchBends = false);

#endif // LIBAUDIO_HAS_TIER2

#endif // MIDICAPTURE_CORPUS_HARNESS_H

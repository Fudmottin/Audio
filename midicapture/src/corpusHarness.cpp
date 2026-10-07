/**
 * @file corpusHarness.cpp
 * @brief Implementation of the analyzer-agnostic corpus evaluator.
 *
 * The whole file is Tier-2: it scores analyzers through the `libaudio`
 * `Analyzer` port. Every `libaudio` type is written with the `libaudio::`
 * qualifier (never `using namespace libaudio;`) to keep the library's types
 * distinct from this module's own globals.
 *
 * @section corpus-metrics The metrics (a faithful port of render_test_suite.py)
 *
 * The matching, median, and per-file / suite-summary printing reproduce
 * `midicapture/render_test_suite.py`, so the aubio run prints the same table
 * the Python harness does. That scoring core is factored into
 * `midicapture/noteMatcher.h` (shared with the MAESTRO ground-truth sweep);
 * `runCorpus` imports it and drives it over the corpus files.
 *
 * One convention in the Python is reproduced *exactly
 * as written* (a deliberate choice, not a bug):
 *   - the octave error uses **integer** `pitch // 12` (floor division), so a
 *     12-octave-mistake reads 12, not a fractional distance.
 *
 * One Python bug is deliberately *not* reproduced: the old summary line scaled
 * the (already-millisecond) onset/duration averages by 1000 a second time.
 * That double-scaling is fixed in both the Python harness and this port, which
 * keeps the two tables in lockstep; the summary onset/duration are the true ms.
 *
 * @section corpus-evaluator The harness is a pure evaluator
 *
 * `runCorpus` reads the `.mp3` assets and the `.mid` ground truth already on
 * disk and scores them; it never synthesizes audio. The test-suite voice is
 * owned by the Python harness (`render_test_suite.py`), which renders each
 * `.mid` with timidity. Keeping the voice in one place means the ground truth
 * and its render can never drift apart (a C++ synth that ignored CC#64 and
 * pitch bends would silently mute the pedal and glissando cases).
 */

#ifdef LIBAUDIO_HAS_TIER2

#include <midicapture/corpusCase.h>
#include <midicapture/corpusHarness.h>
#include <midicapture/noteMatcher.h>

// libaudio's public API (the Analyzer port, the Transcriber engine,
// BasicPitch, MidiFileReader/Writer, Score/Note, and AudioSource).
#include <libaudio/libaudio.h>

// Standard library includes.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// The note matcher + metrics were factored into `midicapture/noteMatcher.h`
// (shared with the MAESTRO ground-truth sweep). `runCorpus` consumes the same
// symbols; these import them at global scope so the function body below is
// unchanged. The corpus passes the default rescale factor (1.0), so a run is
// byte-identical to the pre-factor path.
using midicapture::evaluateFile;
using midicapture::FileMetrics;
using midicapture::Note4;
using midicapture::printPerFile;
using midicapture::printSummary;
using midicapture::toNote4;

// ============================================================================
// Analyzer factory — select an analyzer by model name.
//
// Shared by the direct transcription path (main.cpp) and the corpus evaluator
// so the model-name-to-engine mapping has a single source of truth. "basic"
// and "basic-pitch" are synonyms; the aubio fields of `p` tune only the
// Tier-1 `Transcriber`, and the pitch-bend fields only `BasicPitch`.
// It lives at global scope (matching its declaration in corpusHarness.h), not
// in an anonymous namespace: a definition there would be a *distinct*
// function and would leave unqualified calls ambiguous.
// ============================================================================

std::unique_ptr<libaudio::Analyzer> makeAnalyzer(const std::string& name,
                                                 const AnalyzerParams& p) {
   if (name == "basic" || name == "basic-pitch") {
      // The model is embedded in the binary; decode + resample is in-process
      // (FFmpeg shared libraries). No external tool path needed.
      auto bp = std::make_unique<libaudio::BasicPitch>();
      // Apply the full post-processing policy from `p` (note-creation knobs +
      // the pitch-bend policy); `setOptions` clamps each to its sane range.
      libaudio::BasicPitchOptions options = bp->options();
      options.includePitchBends = p.includePitchBends;
      options.multiplePitchBends = p.multiplePitchBends;
      options.bendDeadbandBins = p.bendDeadbandBins;
      options.onsetThreshold = p.onsetThreshold;
      options.frameThreshold = p.frameThreshold;
      options.minNoteLenMs = p.minNoteLenMs;
      options.velocityScale = p.velocityScale;
      options.minFrequency = p.minFrequency;
      options.maxFrequency = p.maxFrequency;
      options.inferOnsets = p.inferOnsets;
      options.melodiaTrick = p.melodiaTrick;
      options.midiTempo = p.midiTempo;
      bp->setOptions(options);
      return bp;
   }
   if (name == "aubio") {
      // The Tier-1 monophonic engine, now in libaudio; one instance is reused
      // for every file (transcribe() resets its per-file state each call).
      return std::make_unique<libaudio::Transcriber>(p.windowSize, p.hopSize,
                                                     p.silenceDb, p.pitchMethod,
                                                     p.tempoBpm, p.ffmpegPath);
   }
   throw std::runtime_error("unknown model: '" + name +
                            "' (expected 'basic', 'basic-pitch', or 'aubio')");
}

// ============================================================================
// runCorpus — the public entry point (a pure evaluator).
// ============================================================================

int runCorpus(const std::string& dir, const std::string& analyzerName,
              const std::string& ffmpegPath, bool includePitchBends,
              bool multiplePitchBends) {
   namespace fs = std::filesystem;

   std::cout << "midicapture test-suite evaluator\n";
   std::cout << std::string(60, '=') << "\n";
   std::cout << "  Analyzer:     " << analyzerName << "\n";
   std::cout << "  Directory:    " << dir << "\n";
   std::cout << "  ffmpeg:       " << ffmpegPath << "\n";

   // Collect the corpus: the .mp3 files in `dir`, in a stable (sorted) order.
   // Matching the `.mp3` extension skips the .mid, .mid.detected, .mp4 and
   // .txt files that share the directory.
   std::vector<fs::path> mp3s;
   std::error_code ec;
   if (fs::is_directory(dir, ec)) {
      for (const auto& entry : fs::directory_iterator(dir, ec)) {
         if (entry.is_regular_file() && entry.path().extension() == ".mp3") {
            mp3s.push_back(entry.path());
         }
      }
   }
   std::sort(mp3s.begin(), mp3s.end());
   if (mp3s.empty()) {
      std::cerr << "No .mp3 files found in " << dir
                << " (render them with render_test_suite.py first)\n";
      return 1;
   }

   // Build the analyzer (this is where a bad name or a missing model fails).
   // The corpus uses fixed aubio tuning (the struct defaults); only the ffmpeg
   // path and the pitch-bend policy come from the command line.
   AnalyzerParams params;
   params.ffmpegPath = ffmpegPath;
   params.includePitchBends = includePitchBends;
   params.multiplePitchBends = multiplePitchBends;
   std::unique_ptr<libaudio::Analyzer> analyzer;
   try {
      analyzer = makeAnalyzer(analyzerName, params);
   } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
   }

   // Report the execution path for the neural analyzer (Core ML or CPU). The
   // pitch-bend policy is applied inside makeAnalyzer; here we only report it
   // (the 'basic'/'basic-pitch' check covers both model-name synonyms).
   if (analyzerName == "basic" || analyzerName == "basic-pitch") {
      auto* bp = dynamic_cast<libaudio::BasicPitch*>(analyzer.get());
      std::cout << "  Core ML:      "
                << (bp && bp->coreMlActive() ? "active" : "cpu-fallback")
                << "   Pitch bends: " << (includePitchBends ? "on" : "off")
                << (multiplePitchBends ? " (multi-channel)" : "") << "\n";
   }
   std::cout << "\n";
   std::cout << "\xE2\x86\x92 Evaluating each MP3 with " << analyzerName
             << "...\n";

   // Evaluate each file.
   std::vector<FileMetrics> results;
   for (const fs::path& mp3 : mp3s) {
      const std::string name = mp3.filename().string();
      const std::string midPath =
         (mp3.parent_path() / (mp3.stem().string() + ".mid")).string();

      libaudio::Score detected;
      try {
         detected = analyzer->transcribe(mp3.string());
      } catch (const std::exception& e) {
         std::cerr << "  " << name << ":  transcription failed (" << e.what()
                   << "); skipped\n";
         continue;
      }

      libaudio::MidiFileReader reader(midPath);
      if (!reader.ok()) {
         // A malformed generated ground-truth file is a bug in our writer;
         // abort loudly rather than score a half-parsed ground truth.
         std::cerr << "  " << name << ":  invalid ground-truth MIDI, "
                   << "aborting: " << reader.error() << "\n";
         return 2;
      }
      if (reader.score().notes.empty()) {
         std::cout << "  " << name << ":  no notes in the ground truth "
                   << "(skipped)\n";
         continue;
      }

      std::vector<Note4> truth;
      std::vector<Note4> det;
      for (const auto& n : reader.score().notes) {
         truth.push_back(toNote4(n));
      }
      for (const auto& n : detected.notes) {
         det.push_back(toNote4(n));
      }

      const FileMetrics m = evaluateFile(truth, det, name);
      printPerFile(m);
      results.push_back(m);
   }

   if (results.empty()) {
      std::cerr << "\nNo files could be evaluated.\n";
      return 2;
   }
   printSummary(results);
   return 0;
}

#endif // LIBAUDIO_HAS_TIER2

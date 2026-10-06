/**
 * @file corpusHarness.cpp
 * @brief Implementation of the analyzer-agnostic 14-file corpus evaluator.
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
 * `runCorpus` imports it and drives it over the 14 corpus files.
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
 * @section corpus-clean The `--clean` renderer
 *
 * The `--clean` path re-creates the 14 corpus assets the way the Python
 * harness did: each performance is synthesized as a decaying six-harmonic
 * stack (no soundfont), peak-normalized, written as a 48 kHz mono WAV, and
 * encoded to a 192 k MP3 with ffmpeg. The ground-truth `.mid` is written by
 * `libaudio::MidiFileWriter`. Only `--clean` synthesizes; a normal run reuses
 * the `.mp3` files already on disk.
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
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

namespace {

// ============================================================================
// The `--clean` renderer (synthetic-voice assets).
// ============================================================================

// The 14-case corpus and its Score builder now live in
// `midicapture/corpusCase.{h,cpp}` (shared with `--generate-test-midi-files`);
// `cleanScaleSet` below consumes that single source of truth.

// A decaying six-harmonic stack for one note (the corpus's synthetic voice;
// no soundfont). Mirrors render_test_suite.py::synth_note.
constexpr double kRenderRate = 48000.0;
const std::vector<double> HARMONIC_AMP = {1.0, 0.5, 0.3, 0.2, 0.12, 0.08};

double midiToHz(int m) { return 440.0 * std::pow(2.0, (m - 69) / 12.0); }

std::vector<double> synthNote(int midi, double seconds, int velocity) {
   const double hz = midiToHz(midi);
   const int n = static_cast<int>(seconds * kRenderRate);
   const double tau = std::max(0.15, 0.5 * seconds);
   const double amp = (static_cast<double>(velocity) / 127.0) * 0.2;
   std::vector<double> out(static_cast<size_t>(std::max(0, n)), 0.0);
   for (int i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / kRenderRate;
      const double env = std::exp(-t / tau);
      const double fade = std::min(1.0, t / 0.005); // hide the attack click
      double s = 0.0;
      for (int k = 1; k <= 6; ++k) {
         s += HARMONIC_AMP[static_cast<size_t>(k - 1)] *
              std::sin(2.0 * M_PI * static_cast<double>(k) * hz * t);
      }
      out[static_cast<size_t>(i)] = amp * env * fade * s;
   }
   return out;
}

// Mix all notes into a peak-normalized buffer and quantize to int16 PCM.
std::vector<int16_t> synthesize(const std::vector<libaudio::Note>& notes) {
   if (notes.empty()) {
      return {};
   }
   double maxEnd = 0.0;
   for (const auto& n : notes) {
      maxEnd = std::max(maxEnd, n.endTime);
   }
   const int total = static_cast<int>((maxEnd + 0.5) * kRenderRate);
   std::vector<double> buf(static_cast<size_t>(std::max(0, total)), 0.0);

   for (const auto& n : notes) {
      const double length = std::max(0.02, n.endTime - n.startTime);
      std::vector<double> note = synthNote(static_cast<int>(n.pitch), length,
                                           static_cast<int>(n.velocity));
      const int s0 = static_cast<int>(n.startTime * kRenderRate);
      if (s0 >= total || note.empty()) {
         continue;
      }
      const int s1 = std::min(total, s0 + static_cast<int>(note.size()));
      for (int i = s0; i < s1; ++i) {
         buf[static_cast<size_t>(i)] += note[static_cast<size_t>(i - s0)];
      }
   }

   double peak = 0.0;
   for (double v : buf) {
      peak = std::max(peak, std::abs(v));
   }
   if (peak > 0.0) {
      const double scale = 0.85 / peak; // headroom for the loudest hop
      for (double& v : buf) {
         v *= scale;
      }
   }

   std::vector<int16_t> pcm(static_cast<size_t>(std::max(0, total)));
   for (int i = 0; i < total; ++i) {
      long long x = std::llround(buf[static_cast<size_t>(i)] * 32767.0);
      if (x > 32767) x = 32767;
      if (x < -32768) x = -32768;
      pcm[static_cast<size_t>(i)] = static_cast<int16_t>(x);
   }
   return pcm;
}

// Write a canonical 44-byte-header mono 16-bit WAV (RIFF/WAVE, fmt PCM).
void writeWavMono16(const std::string& path, const std::vector<int16_t>& pcm,
                    int sr) {
   const uint32_t dataSize = static_cast<uint32_t>(pcm.size()) * 2u;
   std::vector<uint8_t> hdr(44, 0);
   auto putU32 = [&hdr](size_t off, uint32_t v) {
      hdr[off] = static_cast<uint8_t>(v & 0xFF);
      hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
      hdr[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
      hdr[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
   };
   auto putU16 = [&hdr](size_t off, uint16_t v) {
      hdr[off] = static_cast<uint8_t>(v & 0xFF);
      hdr[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
   };
   std::memcpy(hdr.data(), "RIFF", 4);
   putU32(4, 36u + dataSize);
   std::memcpy(hdr.data() + 8, "WAVE", 4);
   std::memcpy(hdr.data() + 12, "fmt ", 4);
   putU32(16, 16u); // fmt chunk size
   putU16(20, 1);   // PCM
   putU16(22, 1);   // mono
   putU32(24, static_cast<uint32_t>(sr));
   putU32(28, static_cast<uint32_t>(sr) * 2u); // byte rate
   putU16(32, 2);                              // block align
   putU16(34, 16);
   std::memcpy(hdr.data() + 36, "data", 4);
   putU32(40, dataSize);

   std::ofstream f(path, std::ios::binary);
   f.write(reinterpret_cast<const char*>(hdr.data()),
           static_cast<std::streamsize>(hdr.size()));
   f.write(reinterpret_cast<const char*>(pcm.data()),
           static_cast<std::streamsize>(dataSize));
}

// Single-quote a string for safe POSIX shell embedding.
std::string shellQuote(const std::string& s) {
   std::string out = "'";
   for (char c : s) {
      out += (c == '\'') ? "'\\''" : std::string(1, c);
   }
   out += "'";
   return out;
}

// Encode a WAV to a 192 k MP3 with ffmpeg (LAME); true on success.
bool encodeWavToMp3(const std::string& ffmpegPath, const std::string& wav,
                    const std::string& mp3) {
   const std::string cmd = ffmpegPath + " -y -loglevel error -i " +
                           shellQuote(wav) + " -c:a libmp3lame -b:a 192k " +
                           shellQuote(mp3);
   const int status = std::system(cmd.c_str());
   std::error_code ec;
   return status == 0 && fs::exists(mp3, ec) && !fs::is_empty(mp3, ec);
}

// Regenerate the 14 corpus assets into `dir`: for each performance, write the
// ground-truth .mid, synthesize the .wav, encode the .mp3, and delete the wav.
void cleanScaleSet(const std::string& dir, const std::string& ffmpegPath) {
   namespace fs = std::filesystem;
   std::error_code ec;
   fs::create_directories(dir, ec);

   const std::vector<Case> cases = coreCorpus();
   int ok = 0;
   for (const Case& c : cases) {
      const std::string stem = fs::path(c.fileName).stem().string();
      const std::string midPath = (fs::path(dir) / c.fileName).string();
      const std::string wavPath = (fs::path(dir) / (stem + ".wav")).string();
      const std::string mp3Path = (fs::path(dir) / (stem + ".mp3")).string();

      const libaudio::Score score = buildCorpusScore(c);

      libaudio::MidiFileWriter writer(midPath);
      if (!writer.write(score)) {
         std::cerr << "  " << c.fileName << ":  failed to write .mid\n";
         continue;
      }
      std::vector<int16_t> pcm = synthesize(score.notes);
      writeWavMono16(wavPath, pcm, static_cast<int>(kRenderRate));
      if (!encodeWavToMp3(ffmpegPath, wavPath, mp3Path)) {
         std::error_code rm;
         fs::remove(wavPath, rm);
         std::cerr << "  " << c.fileName << ":  mp3 encode failed\n";
         continue;
      }
      std::error_code rm;
      fs::remove(wavPath, rm);
      ++ok;
   }
   std::cout << "  Regenerated " << ok << " of " << cases.size()
             << " corpus assets in " << dir << "\n";
}

} // namespace

// ============================================================================
// Analyzer factory — select an analyzer by model name.
//
// Shared by the direct transcription path (main.cpp) and the corpus evaluator
// so the model-name-to-engine mapping has a single source of truth. "basic"
// and "basic-pitch" are synonyms; the aubio fields of `p` tune only the
// Tier-1 `Transcriber`, and the pitch-bend fields only `BasicPitch`.
// It lives at global scope (matching its declaration in corpusHarness.h), not
// in the anonymous namespace above: a definition there would be a *distinct*
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
// runCorpus — the public entry point.
// ============================================================================

int runCorpus(const std::string& dir, const std::string& analyzerName,
              bool clean, const std::string& ffmpegPath, bool includePitchBends,
              bool multiplePitchBends) {
   namespace fs = std::filesystem;

   std::cout << "midicapture test-suite evaluator\n";
   std::cout << std::string(60, '=') << "\n";
   std::cout << "  Analyzer:     " << analyzerName << "\n";
   std::cout << "  Directory:    " << dir << "\n";
   std::cout << "  ffmpeg:       " << ffmpegPath << "\n";

   // Renderer half: regenerate the assets before evaluating.
   if (clean) {
      std::cout << "\n→ Regenerating corpus assets...\n";
      cleanScaleSet(dir, ffmpegPath);
   }

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
      std::cerr << "No .mp3 files found in " << dir << "\n";
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

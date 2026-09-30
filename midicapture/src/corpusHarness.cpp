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
 * the Python harness does. One convention in the Python is reproduced *exactly
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

#include <midicapture/corpusHarness.h>

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

namespace {

// ============================================================================
// HIR note → the (start, end, pitch, vel) tuple the reference metrics consume.
// ============================================================================

struct Note4 {
   double s = 0.0; // start time, seconds
   double e = 0.0; // end time, seconds
   int pitch = 0;  // MIDI note number
   int vel = 0;    // velocity 0..127
};

Note4 toNote4(const libaudio::Note& n) {
   return {n.startTime, n.endTime, static_cast<int>(n.pitch),
           static_cast<int>(n.velocity)};
}

// Python's floored modulo: ((a % b) + b) % b, so the result is in [0, b).
int pyMod(int a, int b) {
   const int r = a % b;
   return r < 0 ? r + b : r;
}

// Median of a list (empty → 0). Matches render_test_suite.py::median: odd
// count takes the middle element, even count averages the two middle ones.
double median(std::vector<double> xs) {
   if (xs.empty()) {
      return 0.0;
   }
   std::sort(xs.begin(), xs.end());
   const size_t n = xs.size();
   if (n % 2 == 1) {
      return xs[n / 2];
   }
   return 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

// ============================================================================
// The metrics.
// ============================================================================

struct FileMetrics {
   std::string name; // the full .mp3 filename (the per-file label)
   int nTruth = 0;
   int nDet = 0;
   int nMissed = 0;
   int nFalse = 0;
   double recall = 0.0;
   double precision = 0.0;
   double onsetMs = 0.0;
   double durMs = 0.0;
   double vel = 0.0;
   double octave = 0.0;
   double chroma = 0.0;
};

// A detected note matches a truth note when the two time intervals overlap and
// the pitch *class* is within one semitone. A full-octave error still matches
// here (it is the same pitch class) — the octave magnitude is reported in a
// separate column rather than forgiven.
bool notesMatch(const Note4& d, const Note4& t) {
   if (d.s > t.e || d.e < t.s) {
      return false; // no time overlap
   }
   const int dMod = pyMod(d.pitch - t.pitch, 12);
   const int tMod = pyMod(t.pitch - d.pitch, 12);
   // Match when either direction of the 12-class circle distance is ≤ 1.
   return !(dMod > 1 && tMod > 1);
}

// Greedy, time-ordered match of detected notes to truth notes. For each truth
// note (in start order) claim the *earliest-starting* unused detected note that
// matches; an unclaimed truth note is a miss, an unclaimed detected note is a
// false positive. Returns (matched pairs, missed truth, false detected).
struct MatchResult {
   std::vector<std::pair<Note4, Note4>> pairs; // (truth, detected)
   int missed = 0;
   int falseDet = 0;
};

MatchResult matchNotes(std::vector<Note4> truth, std::vector<Note4> detected) {
   // The reference sorts both lists by start before matching.
   std::sort(truth.begin(), truth.end(),
             [](const Note4& a, const Note4& b) { return a.s < b.s; });
   std::sort(detected.begin(), detected.end(),
             [](const Note4& a, const Note4& b) { return a.s < b.s; });

   MatchResult out;
   const int nDet = static_cast<int>(detected.size());
   std::vector<bool> used(static_cast<size_t>(nDet), false);

   for (const Note4& t : truth) {
      int best = -1;
      for (int i = 0; i < nDet; ++i) {
         if (used[static_cast<size_t>(i)]) {
            continue;
         }
         if (!notesMatch(detected[static_cast<size_t>(i)], t)) {
            continue;
         }
         // Keep the earliest-starting unused candidate.
         if (best < 0 || detected[static_cast<size_t>(i)].s <
                            detected[static_cast<size_t>(best)].s) {
            best = i;
         }
      }
      if (best < 0) {
         ++out.missed;
      } else {
         used[static_cast<size_t>(best)] = true;
         out.pairs.emplace_back(t, detected[static_cast<size_t>(best)]);
      }
   }
   for (const bool u : used) {
      if (!u) {
         ++out.falseDet;
      }
   }
   return out;
}

// Score one file: run the matcher, then the per-pair medians. A file with no
// ground-truth notes is not evaluated (the caller skips it).
FileMetrics evaluateFile(const std::vector<Note4>& truth,
                         const std::vector<Note4>& detected,
                         const std::string& name) {
   FileMetrics m;
   m.name = name;
   m.nTruth = static_cast<int>(truth.size());
   m.nDet = static_cast<int>(detected.size());

   const MatchResult mr = matchNotes(truth, detected);
   m.nMissed = mr.missed;
   m.nFalse = mr.falseDet;
   m.recall =
      (m.nTruth > 0) ? static_cast<double>(mr.pairs.size()) / m.nTruth : 0.0;
   m.precision =
      (m.nDet > 0) ? static_cast<double>(mr.pairs.size()) / m.nDet : 0.0;

   std::vector<double> onset, dur, vel, oct, chrom;
   onset.reserve(mr.pairs.size());
   dur.reserve(mr.pairs.size());
   vel.reserve(mr.pairs.size());
   oct.reserve(mr.pairs.size());
   chrom.reserve(mr.pairs.size());
   for (const auto& [t, d] : mr.pairs) {
      onset.push_back(std::abs(d.s - t.s)); // onset offset, seconds
      dur.push_back(std::abs((d.e - d.s) - (t.e - t.s))); // duration offset
      vel.push_back(
         std::abs(static_cast<double>(d.vel) - static_cast<double>(t.vel)));
      // Integer (floor) division — matches the reference `pitch // 12`. MIDI
      // numbers are non-negative, so C++ integer division is floor.
      oct.push_back(std::abs(static_cast<double>(d.pitch / 12) -
                             static_cast<double>(t.pitch / 12)));
      // 12-class circle distance (0 = same class, 1 = adjacent, 6 = maximally).
      chrom.push_back(
         std::min(pyMod(d.pitch - t.pitch, 12), pyMod(t.pitch - d.pitch, 12)));
   }
   m.onsetMs = 1000.0 * median(onset);
   m.durMs = 1000.0 * median(dur);
   m.vel = median(vel);
   m.octave = median(oct);
   m.chroma = median(chrom);
   return m;
}

// ============================================================================
// Printing (the exact reference table).
// ============================================================================

void printPerFile(const FileMetrics& m) {
   std::printf("  %s:  recall %5.1f%%  prec   %5.1f%%  "
               "Δonset %6.1f ms  Δdur   %6.1f ms  "
               "Δvel   %4.1f  Δoct   %3.1f  Δchroma %3.1f  "
               "(%d/%d notes, %d missed, %d false)\n",
               m.name.c_str(), 100.0 * m.recall, 100.0 * m.precision, m.onsetMs,
               m.durMs, m.vel, m.octave, m.chroma, m.nDet, m.nTruth, m.nMissed,
               m.nFalse);
}

// Left-justify a fixed label into a 32-column field, then print the value —
// the reference aligns all summary values to the same column.
void summaryLine(const std::string& label, const char* valueFmt, ...) {
   char valbuf[64];
   va_list ap;
   va_start(ap, valueFmt);
   std::vsnprintf(valbuf, sizeof(valbuf), valueFmt, ap);
   va_end(ap);
   const size_t W = 32;
   std::printf("%s", label.c_str());
   if (label.size() < W) {
      std::printf("%*s", static_cast<int>(W - label.size()), "");
   }
   std::printf("%s\n", valbuf);
}

void printSummary(const std::vector<FileMetrics>& results) {
   const int n = static_cast<int>(results.size());
   auto avg = [&results, n](const char* key) -> double {
      double sum = 0.0;
      for (const FileMetrics& r : results) {
         // `key` selects the field; the small fixed set keeps this readable.
         if (std::strcmp(key, "recall") == 0)
            sum += r.recall;
         else if (std::strcmp(key, "precision") == 0)
            sum += r.precision;
         else if (std::strcmp(key, "onset_ms") == 0)
            sum += r.onsetMs;
         else if (std::strcmp(key, "dur_ms") == 0)
            sum += r.durMs;
         else if (std::strcmp(key, "vel") == 0)
            sum += r.vel;
         else if (std::strcmp(key, "octave") == 0)
            sum += r.octave;
         else if (std::strcmp(key, "chroma") == 0)
            sum += r.chroma;
      }
      return (n > 0) ? sum / n : 0.0;
   };

   std::printf("\n");
   std::printf("Suite summary\n");
   std::printf("%s\n", std::string(60, '-').c_str());
   summaryLine("  Files evaluated:", "%d", n);
   summaryLine("  Avg note recall:", "%5.1f%%", 100.0 * avg("recall"));
   summaryLine("  Avg note precision:", "%5.1f%%", 100.0 * avg("precision"));
   // Onset/duration per-file values are already in ms, so average them
   // directly. (The reference's extra ×1000 here was a double-scaling bug; it
   // is fixed in both the Python harness and this port to keep them in
   // lockstep.)
   summaryLine("  Median onset error:", "%6.1f ms", avg("onset_ms"));
   summaryLine("  Median duration error:", "%6.1f ms", avg("dur_ms"));
   summaryLine("  Median velocity error:", "%6.1f", avg("vel"));
   summaryLine("  Median octave error:", "%6.1f", avg("octave"));
   summaryLine("  Median chroma error:", "%6.1f", avg("chroma"));
}

// ============================================================================
// Analyzer factory — select an analyzer by name.
// ============================================================================

std::unique_ptr<libaudio::Analyzer>
makeAnalyzer(const std::string& name, const std::string& ffmpegPath) {
   if (name == "basic-pitch") {
      // The model is embedded in the binary; BasicPitch only needs ffmpeg.
      return std::make_unique<libaudio::BasicPitch>(ffmpegPath);
   }
   if (name == "aubio") {
      // The Tier-1 monophonic engine, now in libaudio; one instance is reused
      // for every file (transcribe() resets its per-file state each call).
      return std::make_unique<libaudio::Transcriber>(2048u, 512u, -40.0f,
                                                     "yinfft", 120.0,
                                                     ffmpegPath);
   }
   throw std::runtime_error("unknown analyzer: '" + name +
                            "' (expected 'basic-pitch' or 'aubio')");
}

// ============================================================================
// The `--clean` renderer (synthetic-voice assets).
// ============================================================================

// A corpus performance: a filename, a pitch sequence, a tempo, a per-note
// duration, an optional velocity ladder, and an optional inter-note gap.
struct ScaleSpec {
   std::string fileName;
   std::vector<int> pitches;
   double tempoBpm = 120.0;
   double noteBeats = 1.0;
   std::vector<int> velocities;
   double gapBeats = 0.0;
};

// The fixed 14-case corpus, duplicated from the `--generate-test-midi-files`
// set so the renderer is self-contained (and the main entry point stays
// untouched).
std::vector<ScaleSpec> scaleSet() {
   const std::vector<int> majorUp = {60, 64, 67, 72};
   const std::vector<int> majorDown = {72, 67, 64, 60};
   const std::vector<int> minorUp = {69, 72, 76, 81};
   std::vector<int> chromaticUp;
   for (int p = 60; p <= 71; ++p) {
      chromaticUp.push_back(p);
   }
   const std::vector<int> majorLow = {48, 52, 55, 60};
   const std::vector<int> majorMid = {60, 64, 67, 72};
   const std::vector<int> majorHigh = {72, 76, 79, 84};
   const std::vector<int> restSeparated = {60, 60};
   const std::vector<int> sustainedRun = {60, 60, 60, 60};

   return {
      {"scale-major-ascending-whole-notes-60bpm.mid",
       majorUp,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-ascending-half-notes-90bpm.mid",
       majorUp,
       90.0,
       1.0,
       {},
       0.0},
      {"scale-major-descending-whole-notes-60bpm.mid",
       majorDown,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-descending-half-notes-90bpm.mid",
       majorDown,
       90.0,
       1.0,
       {},
       0.0},
      {"scale-chromatic-ascending-quarter-notes-120bpm.mid",
       chromaticUp,
       120.0,
       0.5,
       {},
       0.0},
      {"scale-minor-ascending-whole-notes-60bpm.mid",
       minorUp,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-low-octave-whole-notes-60bpm.mid",
       majorLow,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-mid-octave-whole-notes-60bpm.mid",
       majorMid,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-high-octave-whole-notes-60bpm.mid",
       majorHigh,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-ascending-whole-notes-30bpm.mid",
       majorUp,
       30.0,
       2.0,
       {},
       0.0},
      {"scale-major-ascending-whole-notes-180bpm.mid",
       majorUp,
       180.0,
       2.0,
       {},
       0.0},
      {"velocity-soft-loud-quarter-notes-60bpm.mid",
       std::vector<int>{60, 60, 60, 60, 60, 60}, 60.0, 0.5,
       std::vector<int>{30, 50, 70, 90, 110, 127}, 0.0},
      {"sustained-run-whole-notes-60bpm.mid", sustainedRun, 60.0, 2.0, {}, 0.0},
      {"rest-separated-whole-notes-60bpm.mid",
       restSeparated,
       60.0,
       2.0,
       {},
       1.0},
   };
}

// Build a monophonic Score from a spec (duplicated from main.cpp's
// buildScaleScore: tempo-relative beats, per-note velocity ladder, optional
// inter-note gap; the final note simply ends the timeline).
libaudio::Score buildScaleScore(double tempoBpm,
                                const std::vector<int>& pitches,
                                double noteBeats, const std::string& title,
                                const std::vector<int>& velocities = {},
                                double gapBeats = 0.0) {
   libaudio::Score score;
   score.tempo = tempoBpm;
   score.title = title;

   const double beatSeconds = 60.0 / tempoBpm;
   const double noteSeconds = noteBeats * beatSeconds;
   const double gapSeconds = gapBeats * beatSeconds;
   const bool haveVelocities = velocities.size() == pitches.size();

   double cursor = 0.0;
   for (size_t i = 0; i < pitches.size(); ++i) {
      libaudio::Note note;
      note.startTime = cursor;
      note.endTime = cursor + noteSeconds;
      note.pitch = static_cast<uint8_t>(pitches[i]);
      note.velocity =
         static_cast<uint8_t>(haveVelocities ? velocities[i] : 100);
      note.channel = 0;
      note.sustain = false;
      score.notes.push_back(note);
      cursor += noteSeconds + gapSeconds;
   }
   return score;
}

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

   int ok = 0;
   for (const ScaleSpec& spec : scaleSet()) {
      const std::string stem = fs::path(spec.fileName).stem().string();
      const std::string midPath = (fs::path(dir) / spec.fileName).string();
      const std::string wavPath = (fs::path(dir) / (stem + ".wav")).string();
      const std::string mp3Path = (fs::path(dir) / (stem + ".mp3")).string();

      const libaudio::Score score =
         buildScaleScore(spec.tempoBpm, spec.pitches, spec.noteBeats,
                         spec.fileName, spec.velocities, spec.gapBeats);

      libaudio::MidiFileWriter writer(midPath);
      if (!writer.write(score)) {
         std::cerr << "  " << spec.fileName << ":  failed to write .mid\n";
         continue;
      }
      std::vector<int16_t> pcm = synthesize(score.notes);
      writeWavMono16(wavPath, pcm, static_cast<int>(kRenderRate));
      if (!encodeWavToMp3(ffmpegPath, wavPath, mp3Path)) {
         std::error_code rm;
         fs::remove(wavPath, rm);
         std::cerr << "  " << spec.fileName << ":  mp3 encode failed\n";
         continue;
      }
      std::error_code rm;
      fs::remove(wavPath, rm);
      ++ok;
   }
   std::cout << "  Regenerated " << ok << " of " << scaleSet().size()
             << " corpus assets in " << dir << "\n";
}

} // namespace

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
   std::unique_ptr<libaudio::Analyzer> analyzer;
   try {
      analyzer = makeAnalyzer(analyzerName, ffmpegPath);
   } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
   }

   // Report the execution path for the neural analyzer (Core ML or CPU).
   if (analyzerName == "basic-pitch") {
      auto* bp = dynamic_cast<libaudio::BasicPitch*>(analyzer.get());
      if (bp) {
         // Apply the pitch-bend policy from the command line; the other knobs
         // (deadband) keep their defaults.
         libaudio::BasicPitchOptions options = bp->options();
         options.includePitchBends = includePitchBends;
         options.multiplePitchBends = multiplePitchBends;
         bp->setOptions(options);
      }
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
      if (!reader.ok() || reader.score().notes.empty()) {
         std::cout << "  " << name << ":  no evaluation (skipped)\n";
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

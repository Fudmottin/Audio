/**
 * @file noteMatcher.h
 * @brief The shared note-matching + metrics scorer for audio→MIDI evaluation.
 *
 * This is the scoring core of the corpus evaluator and the MAESTRO ground-truth
 * sweep, factored out so the two can share one exact definition. It is a
 * faithful port of `midicapture/render_test_suite.py`: it matches detected
 * notes to ground-truth notes (greedy, time-ordered), then reports per-file
 * recall / precision and the median onset / duration / velocity / octave /
 * chroma errors, plus the suite summary.
 *
 * One convention in the Python is reproduced *exactly as written* (a deliberate
 * choice, not a bug): the octave error uses **integer** `pitch // 12` (floor
 * division), so a 12-octave-mistake reads 12, not a fractional distance. One
 * Python bug is deliberately *not* reproduced: the old summary line scaled the
 * (already-millisecond) onset/duration averages by 1000 a second time; that
 * double-scaling is fixed here so the summary onset/duration are the true ms.
 *
 * All functions are `inline` (header-only: no separate translation unit) and
 * live in `namespace midicapture`. The only libaudio dependency is
 * `libaudio::Note` (a Tier-1 type), so the header compiles in any tier; its
 * *users* (the corpus harness, the MAESTRO sweep) are Tier-2.
 *
 * The one extension over the reference harness is the optional
 * **duration-match rescale** in `evaluateFile`: a trailing `rescaleFactor`
 * (default `1.0`) that scales a *copy* of the ground-truth notes' times before
 * matching. `1.0` is a no-op (the corpus path), so a run with it stays
 * byte-identical; the MAESTRO sweep passes the per-file factor that stretches
 * the ground-truth timeline to the audio's (see the sweep, and the dataset's
 * placeholder-tempo convention).
 */

#ifndef MIDICAPTURE_NOTE_MATCHER_H
#define MIDICAPTURE_NOTE_MATCHER_H

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <libaudio/hir.h>
#include <string>
#include <utility>
#include <vector>

namespace midicapture {

// ============================================================================
// The note shape the reference metrics consume, and the conversion from HIR.
// ============================================================================

// A (start, end, pitch, velocity) note — the tuple the reference metrics use.
struct Note4 {
   double s = 0.0; // start time, seconds
   double e = 0.0; // end time, seconds
   int pitch = 0;  // MIDI note number
   int vel = 0;    // velocity 0..127
};

// A HIR note → the (start, end, pitch, vel) tuple.
inline Note4 toNote4(const libaudio::Note& n) {
   return {n.startTime, n.endTime, static_cast<int>(n.pitch),
           static_cast<int>(n.velocity)};
}

// Python's floored modulo: ((a % b) + b) % b, so the result is in [0, b).
inline int pyMod(int a, int b) {
   const int r = a % b;
   return r < 0 ? r + b : r;
}

// Median of a list (empty → 0). Matches render_test_suite.py::median: odd
// count takes the middle element, even count averages the two middle ones.
inline double median(std::vector<double> xs) {
   if (xs.empty()) {
      return 0.0;
   }
   std::sort(xs.begin(), xs.end());
   const std::size_t n = xs.size();
   if (n % 2 == 1) {
      return xs[n / 2];
   }
   return 0.5 * (xs[n / 2 - 1] + xs[n / 2]);
}

// ============================================================================
// The metrics.
// ============================================================================

struct FileMetrics {
   std::string name; // the full source filename (the per-file label)
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
inline bool notesMatch(const Note4& d, const Note4& t) {
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

inline MatchResult matchNotes(std::vector<Note4> truth,
                             std::vector<Note4> detected) {
   // The reference sorts both lists by start before matching.
   std::sort(truth.begin(), truth.end(),
             [](const Note4& a, const Note4& b) { return a.s < b.s; });
   std::sort(detected.begin(), detected.end(),
             [](const Note4& a, const Note4& b) { return a.s < b.s; });

   MatchResult out;
   const int nDet = static_cast<int>(detected.size());
   std::vector<bool> used(static_cast<std::size_t>(nDet), false);

   for (const Note4& t : truth) {
      int best = -1;
      for (int i = 0; i < nDet; ++i) {
         if (used[static_cast<std::size_t>(i)]) {
            continue;
         }
         if (!notesMatch(detected[static_cast<std::size_t>(i)], t)) {
            continue;
         }
         // Keep the earliest-starting unused candidate.
         if (best < 0 || detected[static_cast<std::size_t>(i)].s <
                            detected[static_cast<std::size_t>(best)].s) {
            best = i;
         }
      }
      if (best < 0) {
         ++out.missed;
      } else {
         used[static_cast<std::size_t>(best)] = true;
         out.pairs.emplace_back(t, detected[static_cast<std::size_t>(best)]);
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
//
// `rescaleFactor` (default `1.0`) optionally stretches the ground-truth
// timeline to the detected timeline *before* matching: each truth note's start
// and end are scaled by the factor on a working copy, so the note's duration
// is scaled by the same amount. `1.0` is a no-op (the corpus path) and keeps a
// run byte-identical; the MAESTRO sweep passes the per-file factor (audio
// duration ÷ ground-truth MIDI duration) that compensates the dataset's
// placeholder-tempo convention. The truth *count* is the unscaled size:
// rescaling never creates or destroys a note.
inline FileMetrics evaluateFile(const std::vector<Note4>& truthIn,
                               const std::vector<Note4>& detected,
                               const std::string& name,
                               double rescaleFactor = 1.0) {
   FileMetrics m;
   m.name = name;
   m.nTruth = static_cast<int>(truthIn.size());
   m.nDet = static_cast<int>(detected.size());

   std::vector<Note4> truth = truthIn;
   if (rescaleFactor != 1.0) {
      for (Note4& t : truth) {
         t.s *= rescaleFactor;
         t.e *= rescaleFactor;
      }
   }

   const MatchResult mr = matchNotes(std::move(truth), detected);
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

inline void printPerFile(const FileMetrics& m) {
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
inline void summaryLine(const std::string& label, const char* valueFmt, ...) {
   char valbuf[64];
   va_list ap;
   va_start(ap, valueFmt);
   std::vsnprintf(valbuf, sizeof(valbuf), valueFmt, ap);
   va_end(ap);
   const std::size_t W = 32;
   std::printf("%s", label.c_str());
   if (label.size() < W) {
      std::printf("%*s", static_cast<int>(W - label.size()), "");
   }
   std::printf("%s\n", valbuf);
}

inline void printSummary(const std::vector<FileMetrics>& results) {
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
   // is fixed here to keep the summary in lockstep with the per-file table.)
   summaryLine("  Median onset error:", "%6.1f ms", avg("onset_ms"));
   summaryLine("  Median duration error:", "%6.1f ms", avg("dur_ms"));
   summaryLine("  Median velocity error:", "%6.1f", avg("vel"));
   summaryLine("  Median octave error:", "%6.1f", avg("octave"));
   summaryLine("  Median chroma error:", "%6.1f", avg("chroma"));
}

} // namespace midicapture

#endif // MIDICAPTURE_NOTE_MATCHER_H

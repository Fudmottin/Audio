/**
 * @file corpusCase.cpp
 * @brief Implementation of the shared corpus performance spec (corpusCase.h).
 *
 * Defines the fixed 14-case monophonic corpus (`coreCorpus`), the compact
 * `sequential()` monophonic helper, and `buildCorpusScore` (beats -> seconds).
 * Everything here depends only on the HIR, so it is usable in Tier-1 and
 * Tier-2 builds alike.
 */

#include <libaudio/hir.h>
#include <midicapture/corpusCase.h>

Case sequential(const std::string& fileName, const std::vector<int>& pitches,
                double tempoBpm, double noteBeats,
                const std::vector<int>& velocities, double gapBeats) {
   Case c;
   c.fileName = fileName;
   c.tempoBpm = tempoBpm;

   // A per-note velocity ladder: the i-th note takes velocities[i] when there
   // is one for it, otherwise the uniform default (100). This is how a dynamics
   // test (soft -> loud) is expressed without changing pitch or timing.
   const bool haveVelocities = velocities.size() == pitches.size();

   c.notes.reserve(pitches.size());
   for (size_t i = 0; i < pitches.size(); ++i) {
      CaseNote n;
      // Monophonic: note i starts i*(noteBeats + gapBeats) beats in and runs
      // for noteBeats — the next note begins one gap after this one ends.
      n.onsetBeats = static_cast<double>(i) * (noteBeats + gapBeats);
      n.durBeats = noteBeats;
      n.pitch = static_cast<uint8_t>(pitches[i]);
      n.velocity = static_cast<uint8_t>(haveVelocities ? velocities[i] : 100);
      c.notes.push_back(std::move(n));
   }
   return c;
}

std::vector<Case> coreCorpus() {
   // C-major arpeggio: C4=60, E4=64, G4=67, C5=72.
   const std::vector<int> majorUp = {60, 64, 67, 72};
   const std::vector<int> majorDown = {72, 67, 64, 60};
   // A-minor arpeggio: A4=69, C5=72, E5=76, A5=81.
   const std::vector<int> minorUp = {69, 72, 76, 81};
   // One-octave chromatic run: C4=60 .. B4=71.
   std::vector<int> chromaticUp;
   for (int p = 60; p <= 71; ++p) {
      chromaticUp.push_back(p);
   }
   // One-octave runs at three heights: separates an *octave* error from a
   // *chroma* (pitch-class) error.
   const std::vector<int> majorLow = {48, 52, 55, 60};  // C3..C4
   const std::vector<int> majorMid = {60, 64, 67, 72};  // C4..C5 (== majorUp)
   const std::vector<int> majorHigh = {72, 76, 79, 84}; // C5..C6
   // A sustained note, one beat of rest, then another (a distinct physical
   // note, not a wobble fragment); and a wobble run that must recombine.
   const std::vector<int> restSeparated = {60, 60};
   const std::vector<int> sustainedRun = {60, 60, 60, 60};

   return {
      // Scales: pitch + timing + defragmentation.
      sequential("scale-major-ascending-whole-notes-60bpm.mid", majorUp, 60.0,
                 2.0),
      sequential("scale-major-ascending-half-notes-90bpm.mid", majorUp, 90.0,
                 1.0),
      sequential("scale-major-descending-whole-notes-60bpm.mid", majorDown,
                 60.0, 2.0),
      sequential("scale-major-descending-half-notes-90bpm.mid", majorDown, 90.0,
                 1.0),
      sequential("scale-chromatic-ascending-quarter-notes-120bpm.mid",
                 chromaticUp, 120.0, 0.5),
      sequential("scale-minor-ascending-whole-notes-60bpm.mid", minorUp, 60.0,
                 2.0),
      // Octave: the same major arpeggio at three heights.
      sequential("scale-major-low-octave-whole-notes-60bpm.mid", majorLow, 60.0,
                 2.0),
      sequential("scale-major-mid-octave-whole-notes-60bpm.mid", majorMid, 60.0,
                 2.0),
      sequential("scale-major-high-octave-whole-notes-60bpm.mid", majorHigh,
                 60.0, 2.0),
      // Timing: the same scale at two tempos (slow + fast).
      sequential("scale-major-ascending-whole-notes-30bpm.mid", majorUp, 30.0,
                 2.0),
      sequential("scale-major-ascending-whole-notes-180bpm.mid", majorUp, 180.0,
                 2.0),
      // Dynamics: a soft -> loud velocity ladder on one pitch (the suite's
      // velocity test — the only varying signal is loudness).
      sequential("velocity-soft-loud-quarter-notes-60bpm.mid",
                 std::vector<int>{60, 60, 60, 60, 60, 60}, 60.0, 0.5,
                 std::vector<int>{30, 50, 70, 90, 110, 127}),
      // Defrag / merge: a wobble run that must collapse, and a rest-separated
      // run that must NOT merge across the gap (one-beat rest between them).
      sequential("sustained-run-whole-notes-60bpm.mid", sustainedRun, 60.0,
                 2.0),
      sequential("rest-separated-whole-notes-60bpm.mid", restSeparated, 60.0,
                 2.0, std::vector<int>{}, 1.0),
   };
}

libaudio::Score buildCorpusScore(const Case& c) {
   libaudio::Score score;
   score.tempo = c.tempoBpm;
   score.title = c.fileName;

   // A beat is one quarter note: seconds = beats * 60 / tempoBpm.
   const double beatSeconds = 60.0 / c.tempoBpm;

   score.notes.reserve(c.notes.size());
   for (const CaseNote& cn : c.notes) {
      libaudio::Note note;
      note.startTime = cn.onsetBeats * beatSeconds;
      note.endTime = (cn.onsetBeats + cn.durBeats) * beatSeconds;
      note.pitch = cn.pitch;
      note.velocity = cn.velocity;
      note.channel = 0; // channel 1 (Acoustic Grand Piano).
      note.sustain = false;
      note.pitchBends = cn.pitchBends;
      score.notes.push_back(std::move(note));
   }
   score.controls = c.controls;
   return score;
}

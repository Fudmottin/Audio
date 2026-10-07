/**
 * @file corpusCase.cpp
 * @brief Implementation of the shared corpus performance spec (corpusCase.h).
 *
 * Defines the fixed corpus (`coreCorpus`): the 14 monophonic cases (the
 * historical regression gate) plus four gesture cases the monophonic set
 * cannot express (a simultaneous chord, a single bent note, a sustain-pedal
 * phrase, and an overlapping legato run). Also the compact `sequential()`
 * monophonic helper and `buildCorpusScore` (beats -> seconds). Everything here
 * depends only on the HIR, so it is usable in Tier-1 and Tier-2 builds alike.
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

// ============================================================================
// The four gesture cases, each a small static builder. `sequential()` cannot
// express these (one pitch at a time, no control events, no bends), so each is
// built directly. All are at 60 BPM; note timing is in beats, and the sustain
// pedal times are computed in *seconds* here (ControlEvent.time is seconds,
// unlike CaseNote's beats) so the phrase-bracket lands where intended.
// ============================================================================

// A simultaneous C-major triad (C4 + E4 + G4) held for two beats: the set's
// polyphony case — three pitches sounding at once, which `sequential()`
// (monophonic) cannot produce.
static Case makeChordTriad() {
   Case c;
   c.fileName = "chord-major-triad-whole-notes-60bpm.mid";
   c.tempoBpm = 60.0;
   const std::vector<int> triad = {60, 64, 67}; // C4, E4, G4.
   for (int pitch : triad) {
      CaseNote n;
      n.onsetBeats = 0.0; // struck together.
      n.durBeats = 2.0;   // a whole note (2 beats in the corpus).
      n.pitch = static_cast<uint8_t>(pitch);
      n.velocity = 100;
      c.notes.push_back(n);
   }
   return c;
}

// A single C4 with a smooth +1-semitone bend across its duration: the set's
// pitch-bend (glissando) case. The non-empty `pitchBends` makes the writer
// emit 0xE0 messages across the note and the reader round-trips them, so the
// ground-truth file carries a real pitch contour (a constant-pitch note
// emits none).
static Case makeGlissando() {
   Case c;
   c.fileName = "glissando-pitch-bend-60bpm.mid";
   c.tempoBpm = 60.0;
   CaseNote n;
   n.onsetBeats = 0.0;
   n.durBeats = 2.0; // a whole note (2.0 s at 60 BPM).
   n.pitch = 60;     // C4.
   n.velocity = 100;
   // Five evenly spaced 14-bit ticks from 0 (no bend) to +4096 (+1 semitone;
   // the bend scale is 4096 ticks per semitone).
   for (int16_t v = 0; v <= 4096; v += 1024) {
      n.pitchBends.push_back(v);
   }
   c.notes.push_back(n);
   return c;
}

// C4-E4-G4-C5 one beat each, with the sustain pedal (CC#64) held for the whole
// phrase (down at the first note, up at the last note's end). The notes keep
// their nominal (finger) lengths; the pedal is a *deliberate* control event
// (buildCorpusScore leaves note.sustain false), and timidity honours it, so
// the earlier notes keep ringing under the pedal — the signature this case
// exists to catch.
static Case makeSustainPedal() {
   Case c;
   c.fileName = "sustain-pedal-legato-60bpm.mid";
   c.tempoBpm = 60.0;
   const std::vector<int> run = {60, 64, 67, 72}; // C4, E4, G4, C5.
   const double beatSec = 60.0 / c.tempoBpm;      // 1.0 s at 60 BPM.
   for (size_t i = 0; i < run.size(); ++i) {
      CaseNote n;
      n.onsetBeats = static_cast<double>(i); // 0, 1, 2, 3 beats.
      n.durBeats = 1.0;
      n.pitch = static_cast<uint8_t>(run[i]);
      n.velocity = 100;
      c.notes.push_back(n);
   }
   // The pedal is up exactly when the last note (C5) ends: noteCount*beatSec.
   const double phraseEnd = static_cast<double>(run.size()) * beatSec;
   libaudio::ControlEvent down;
   down.time = 0.0; // engage at the first note.
   down.controller = 64;
   down.value = 127;
   c.controls.push_back(down);
   libaudio::ControlEvent up;
   up.time = phraseEnd; // release at the phrase end.
   up.controller = 64;
   up.value = 0;
   c.controls.push_back(up);
   return c;
}

// C4-E4-G4-C5, one beat each, but starting every *half* beat so successive
// notes overlap by half a beat: a smooth connected run with two-note chords
// throughout. The overlap is what distinguishes it from the monophonic runs
// (a note that ends before the next begins) — the set's legato/polyphonic-
// timing case.
static Case makeLegatoOverlap() {
   Case c;
   c.fileName = "legato-overlapping-half-notes-60bpm.mid";
   c.tempoBpm = 60.0;
   const std::vector<int> run = {60, 64, 67, 72}; // C4, E4, G4, C5.
   for (size_t i = 0; i < run.size(); ++i) {
      CaseNote n;
      n.onsetBeats = static_cast<double>(i) * 0.5; // 0, 0.5, 1.0, 1.5 beats.
      n.durBeats = 1.0;                            // -> a 0.5-beat overlap.
      n.pitch = static_cast<uint8_t>(run[i]);
      n.velocity = 100;
      c.notes.push_back(n);
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

   std::vector<Case> cases;

   // The 14 monophonic cases (the historical regression gate): pitch, timing,
   // dynamics, and defrag/merge, all one note at a time. These are the
   // corpus's regression core and are written to disk byte-identically to the
   // pre-gesture-extension set.
   // Scales: pitch + timing + defragmentation.
   cases.push_back(sequential("scale-major-ascending-whole-notes-60bpm.mid",
                              majorUp, 60.0, 2.0));
   cases.push_back(sequential("scale-major-ascending-half-notes-90bpm.mid",
                              majorUp, 90.0, 1.0));
   cases.push_back(sequential("scale-major-descending-whole-notes-60bpm.mid",
                              majorDown, 60.0, 2.0));
   cases.push_back(sequential("scale-major-descending-half-notes-90bpm.mid",
                              majorDown, 90.0, 1.0));
   cases.push_back(
      sequential("scale-chromatic-ascending-quarter-notes-120bpm.mid",
                 chromaticUp, 120.0, 0.5));
   cases.push_back(sequential("scale-minor-ascending-whole-notes-60bpm.mid",
                              minorUp, 60.0, 2.0));
   // Octave: the same major arpeggio at three heights.
   cases.push_back(sequential("scale-major-low-octave-whole-notes-60bpm.mid",
                              majorLow, 60.0, 2.0));
   cases.push_back(sequential("scale-major-mid-octave-whole-notes-60bpm.mid",
                              majorMid, 60.0, 2.0));
   cases.push_back(sequential("scale-major-high-octave-whole-notes-60bpm.mid",
                              majorHigh, 60.0, 2.0));
   // Timing: the same scale at two tempos (slow + fast).
   cases.push_back(sequential("scale-major-ascending-whole-notes-30bpm.mid",
                              majorUp, 30.0, 2.0));
   cases.push_back(sequential("scale-major-ascending-whole-notes-180bpm.mid",
                              majorUp, 180.0, 2.0));
   // Dynamics: a soft -> loud velocity ladder on one pitch (the suite's
   // velocity test — the only varying signal is loudness).
   cases.push_back(sequential("velocity-soft-loud-quarter-notes-60bpm.mid",
                              std::vector<int>{60, 60, 60, 60, 60, 60}, 60.0,
                              0.5, std::vector<int>{30, 50, 70, 90, 110, 127}));
   // Defrag / merge: a wobble run that must collapse, and a rest-separated
   // run that must NOT merge across the gap (one-beat rest between them).
   cases.push_back(sequential("sustained-run-whole-notes-60bpm.mid",
                              sustainedRun, 60.0, 2.0));
   cases.push_back(sequential("rest-separated-whole-notes-60bpm.mid",
                              restSeparated, 60.0, 2.0, std::vector<int>{},
                              1.0));

   // The four gesture cases (chord, bend, pedal, overlap) that extend the set
   // beyond monophony. Timidity honours the pedal and the bend, so these
   // render a real polyphonic / pedalled / gliding signature to transcribe.
   cases.push_back(makeChordTriad());
   cases.push_back(makeGlissando());
   cases.push_back(makeSustainPedal());
   cases.push_back(makeLegatoOverlap());

   return cases;
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

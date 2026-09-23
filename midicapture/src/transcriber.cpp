/**
 * @file transcriber.cpp
 * @brief Implementation of Transcriber — audio-to-MIDI transcription.
 *
 * This module implements the Transcriber class, which orchestrates
 * the full transcription pipeline:
 * 1. Open the input audio file.
 * 2. For each hop of audio: measure energy, estimate pitch (YINfft),
 *    and detect onsets (spectral flux) from a single sequential read.
 * 3. Gate note boundaries with a hysteresis state machine.
 * 4. Build a HIR Score with the detected notes.
 *
 * @section gating Gating: energy with hysteresis, not confidence
 *
 * YINfft reports a usable fundamental even for noise (and on real
 * piano notes its confidence reads ~0), so the pitch detector's
 * confidence is NOT used to gate note boundaries. Instead a hop is
 * *tonal* only when its RMS is above the silence threshold. Because a
 * decaying piano note oscillates around that level (its harmonics beat
 * against each other), the gate is a hysteresis pair: a note *arms*
 * when energy rises above `silenceDb` and *disarms* only after energy
 * has stayed below `silenceDb − kReleaseHysteresisDb` for
 * `kReleaseHops` consecutive hops.
 *
 * @section pitch Pitch: chroma voting, octave anchored to the loudest hop
 *
 * A sampled instrument is rarely perfectly tuned, so a single hop's
 * YINfft estimate can land on either side of a semitone boundary and a
 * raw round-to-nearest-MIDI would flip note number hop to hop. The
 * fundamental is therefore resolved in two stages at a note's *end*:
 *  - **Chroma voting.** Every valid hz estimate over the note's lifetime
 *    is folded to one of 12 pitch classes (Hz mod one octave, nearest);
 *    the majority pitch class is the note's chroma. A piano note keeps
 *    the same pitch class across its lifetime, so the vote is stable.
 *  - **Octave anchored to the loudest hop.** A pitch class has no octave.
 *    YINfft is a *harmonic* estimator, and over a decaying note the higher
 *    partials fade first so the lifetime-mean frequency drifts to a sub
 *    -octave of the true fundamental (measured 91 Hz mean for a 440 Hz
 *    note). The *loudest* hop is the one nearest the attack, where the
 *    full harmonic series is still present, so its YIN estimate is the most
 *    reliable octave the detector will give; the winning pitch class is
 *    re-anchored to the octave nearest that loudest hop. (The lifetime
 *    mean is used only as a fallback for a note with no tonal hop.)
 *
 * This makes pitch robust to: (a) a detuned or octave-error fundamental,
 * (b) harmonics (a double period folds to the same chroma), and (c)
 * wobble across a semitone boundary. Note *changes* additionally require
 * `kPitchStability` consecutive hops to agree (pitch hysteresis) so a
 * single stray hop cannot fragment a note.
 *
 * @section defrag Defragmentation: minimum lifetime + same-pitch merge
 *
 * A *decaying* note is closed and re-opened hop to hop by the onset / energy
 * wobble of its own decay tail, so a single physical note arrives as a run of
 * short same-pitch fragments. Two stages undo that: (1) a note shorter than
 * `kMinNoteHops` (and a same-pitch pitch-change shorter than `kMinReplaceHops`)
 * is dropped, and (2) after the scan, consecutive same-pitch fragments
 * closer than a quarter-note gap are merged into one note. Velocity is the
 * loudest hop of the merged lifetime, so a merged note's dynamics read as a
 * single attack-and-decay rather than a per-hop tremolo.
 *
 * @section timing Timing model
 *
 * Every detector consumes consecutive, non-overlapping hops of hopSize
 * samples; a single sequential read of the file feeds both detectors.
 * Note boundaries are reported at the window end:
 * (i+1) * hop / sampleRate seconds, which matches the writer's
 * seconds-to-ticks conversion.
 *
 * Known limitation: the attack of the very first note of a file is
 * frequently missed by the onset detector (aubio's first-frame
 * artifact — onset detection needs a spectral *change*, and a file that
 * begins with audio has none on its first frames). A performance that
 * starts at t=0 loses its first note.
 *
 * @section tempo Tempo
 *
 * The output Score's tempo is the user's `--tempo` (BPM), forwarded from the
 * CLI to the constructor. It drives the fragment-merge gap in defrag and the
 * tick conversion in the MIDI writer. (Tempo *tracking* of the source —
 * `aubio_tempo` — is a future phase, not implemented here.)
 *
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <libaudio/audioFile.h>
#include <libaudio/libaudio.h>
#include <libaudio/onset.h>
#include <stdexcept>
#include <string>
#include <vector>

// Forward declarations of our own modules.
#include <midicapture/transcriber.h>

using namespace libaudio;

// ============================================================================
// Transcriber::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All analysis state is isolated here. The public
// interface never exposes aubio types (aubio_pitchyinfft_t*, etc.).
//
// The state machine has two states:
// - isPlaying_ = false: IDLE — waiting for an onset.
// - isPlaying_ = true: PLAYING — a note is active.
//
// Gating: energy hysteresis (on/off thresholds + run length) and pitch
// hysteresis (a change needs K agreeing hops). See the file header.
// ============================================================================
struct Transcriber::Impl {
   // Energy hysteresis band, in dB below the silence (on) threshold.
   //
   // Domain context: a decaying piano note oscillates ±3 dB or more
   // around the silence level, so on/off must be separated. -10 dB keeps
   // the off threshold well inside the noise floor.
   static constexpr float kReleaseHysteresisDb = -10.0f;
   // Consecutive below-off-threshold hops required to end a note.
   static constexpr uint32_t kReleaseHops = 3;
   // Consecutive hops that must agree on a *different* pitch class
   // before a note is allowed to change pitch (pitch hysteresis).
   //
   // Domain context: 5 hops is 100 ms at the default 512-sample hop.
   // A decaying note vibrates and its YIN estimate wanders across a
   // semitone boundary for tens of ms, so a short run (3) fragments
   // one physical note into many. At ~120 BPM a new note arriving every
   // 500 ms still gives 2-3 clean hops of the new pitch before the
   // change is accepted, so 5 costs no timing for real passages.
   static constexpr uint32_t kPitchStability = 5;
   // Minimum lifetime a note must have to be kept (in hops).
   //
   // Domain context: after the release-hysteresis fix, each surviving
   // fragment of a decaying note is 2-5 hops long (22-80 ms). A real
   // note is at least one hop of attack plus several hops of sustained
   // tone (>= 5 hops = 100 ms), so this removes the fragmentation
   // debris without touching real notes. It replaces the 50 ms minimum
   // that NoteTrimmer was meant to provide.
   static constexpr uint32_t kMinNoteHops = 5;

   // Minimum duration of the audio buffer, in samples, that the current
   // note must have accumulated before it is allowed to be *replaced*
   // by the pitch-change branch or a new onset. Shorter notes are
   // finalized at their current end time and the replacement is
   // suppressed (the same pitch-class run continues, so the next
   // qualifying window of the new pitch can still restart the note).
   //
   // Domain context: an attack hop's window is dominated by the
   // previous note's decay (the window lags by bufSize - hopSize), so
   // YIN reports the OLD pitch during the attack; the new pitch only
   // takes over 2-3 hops later. Without this guard the detector closes
   // the previous note, opens a note stamped with the old pitch, then
   // re-closes it when the new pitch stabilizes 3 hops later — every
   // real note arrives as 2-3 fragments. Allowing a replacement only
   // after a credible lifetime collapses each physical note to a single
   // fragment: the last pitch-change window carries the new pitch, so
   // the surviving fragment is stamped with the correct note.
   static constexpr uint32_t kMinReplaceHops = 5;

   // Analysis parameters.
   uint32_t bufSize;
   uint32_t hopSize;
   float silenceDb;
   std::string pitchMethod;
   // Tempo (BPM) of the output Score. Used to size the fragment-merge gap
   // (a same-pitch run closer than a quarter note is one sustained note) and
   // stamped onto the returned Score.
   double tempoBpm = 120.0;

   // Analysis objects (Pimpl pattern).
   std::unique_ptr<PitchDetector> pitchDetector;
   std::unique_ptr<OnsetDetector> onsetDetector;

   // Rolling buffer of the last bufSize samples. Each hop, the oldest
   // hopSize samples are shifted out and the new hop is appended, so
   // pitch detection always runs over the window that just ended.
   std::vector<float> window;
   // Hop-sized scratch buffer for the next chunk of input.
   std::vector<float> hopBuf;

   // Per-note fundamental-frequency accumulator.
   //
   // Domain context: every valid hz estimate since the note started is kept
   // so the final pitch can be resolved from the *whole* note (chroma voted
   // over all of them; octave anchored to the loudest hop). Storing the raw
   // estimates also lets peakHz be computed at finalize time.
   std::vector<float> noteHzs_;
   // Per-hop RMS paired 1:1 with noteHzs_ (same hop index). Used only to
   // find which hop is loudest (for peakHz()); velocity uses the running
   // max directly instead.
   std::vector<float> noteRms_;

   // Running peak hop-RMS of the current note (in linear amplitude).
   //
   // Domain context: velocity is the loudest moment of the note, not the
   // attack hop alone (the attack window lags the note by bufSize-hopSize
   // samples and is dominated by the *previous* note's decay), and not the
   // last hop (the release). The running max over the note's lifetime
   // captures the strongest moment the ear hears as "loudness".
   float peakRms = 0.0f;

   // Current note being tracked (monophonic state machine).
   bool isPlaying_ = false;
   uint32_t currentNoteHops_ = 0;
   Note currentNote_;

   // Results.
   std::vector<Note> notes_;

   // Sample rate (set during transcribe()).
   uint32_t sampleRate_ = 48000;

   // Convert a frequency in Hz to a MIDI note number (uint8_t), rounding.
   //
   // Domain context: MIDI 0 is the "no pitch" sentinel. We clamp to the
   // piano range (A0=21 .. C8=108) and return 0 for anything out of
   // range or non-positive.
   static uint8_t hzToMidiNote(float hz) {
      if (hz <= 0.0f) return 0;
      double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
      if (midi < 21.0 || midi > 108.0) return 0;
      return static_cast<uint8_t>(std::round(midi));
   }

   // Reduce a frequency to a pitch class (0..11) and its octave offset.
   //
   // @param hz      Fundamental frequency in Hz (> 0).
   // @param midiFloat  69 + 12·log2(hz/440) — the unrounded MIDI value.
   // @return pitch class c (0..11) and the nearest integer MIDI value,
   //         whose octave determines the re-anchoring base.
   static int pitchClassOf(float hz, double& midiFloat) {
      midiFloat = 69.0 + 12.0 * std::log2(hz / 440.0);
      int c = static_cast<int>(midiFloat) % 12;
      return c < 0 ? c + 12 : c;
   }

   // Convert dB to linear amplitude.
   static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

   // Calculate the RMS energy of a buffer.
   static float rmsEnergy(const float* buffer, uint32_t length) {
      if (length == 0) return 0.0f;
      float sum = 0.0f;
      for (uint32_t i = 0; i < length; ++i) {
         float sample = buffer[i];
         sum += sample * sample;
      }
      return std::sqrt(sum / static_cast<float>(length));
   }

   // Convert a sample position to a timestamp in seconds.
   double sampleToTimestamp(uint64_t sample) const {
      return static_cast<double>(sample) / static_cast<double>(sampleRate_);
   }

   // Finalize the current note (end it) and add it to the results.
   //
   // The note is kept only if it accumulated at least `kMinNoteHops` hops:
   // a genuine note is an attack plus several hops of sustained tone, so
   // a run of 1-3 hop spectral-wobble fragments (the debris of a decaying
   // note being closed and re-opened hop to hop) is dropped instead of
   // emitted. The minimum replaces the 50 ms floor that NoteTrimmer was
   // meant to provide. State is reset whether or not the note is kept.
   void finalizeCurrentNote(double endTime) {
      if (isPlaying_ && currentNote_.startTime < endTime) {
         currentNote_.endTime = endTime;
         if (currentNoteHops_ >= kMinNoteHops) {
            notes_.push_back(currentNote_);
         }
      }
      isPlaying_ = false;
      currentNoteHops_ = 0;
      currentNote_ = Note();
      noteHzs_.clear();
      noteRms_.clear();
      peakRms = 0.0f;
   }

   // Start a new note, resetting the fundamental and velocity accumulators.
   void startNewNote(double startTime) {
      isPlaying_ = true;
      currentNoteHops_ = 0;
      currentNote_ = Note();
      currentNote_.startTime = startTime;
      currentNote_.channel = 0; // Channel 0 = Acoustic Grand Piano.
      currentNote_.sustain = false;
      noteHzs_.clear();
      noteRms_.clear();
      peakRms = 0.0f;
   }

   // Map a hop's linear RMS to a MIDI velocity (1..127).
   //
   // Domain context: a rendered piano note peaks around 0.05 linear RMS;
   // the 1800 gain puts that at ~90, mid-scale. The max over the note's
   // hops (see peakRms) is what the ear reads as the note's loudness.
   static uint8_t velocityFromPeakRms(float peakRms) {
      return static_cast<uint8_t>(
         std::max(1.0, std::min(127.0, peakRms * 1800.0)));
   }

   // Record a valid hz estimate into the current note's accumulator.
   void recordHz(float hz, float rms) {
      if (hz > 0.0f) {
         noteHzs_.push_back(hz);
         noteRms_.push_back(rms);
      }
   }

   // The fundamental of the *loudest* hop of the current note, in Hz.
   //
   // Domain context: the loudest hop is (up to the 1-hop release hysteresis)
   // the hop nearest the *attack*. Pitch is anchored to it — not to the
   // lifetime mean — because YIN is a *harmonic* estimator: over a decaying
   // note the higher partials fade first and the lifetime-mean frequency
   // drifts to a sub-octave of the true fundamental (measured: 91 Hz mean
   // for a 440 Hz note), which would re-anchor the octave down. The
   // attack hop still carries the full harmonic series, so its YIN estimate
   // is the most reliable octave the detector will give for this note.
   float peakHz() const {
      if (noteHzs_.empty()) return 0.0f;
      float bestHz = noteHzs_.front();
      float bestRms = 0.0f;
      for (size_t i = 0; i < noteHzs_.size(); ++i) {
         if (noteRms_[i] > bestRms) {
            bestRms = noteRms_[i];
            bestHz = noteHzs_[i];
         }
      }
      return bestHz;
   }

   // The note's lifetime mean fundamental (Hz), or 0 if none recorded.
   // Used only as the octave base for the very first note of a file.
   float meanHz() const {
      if (noteHzs_.empty()) return 0.0f;
      double sum = 0.0;
      for (float hz : noteHzs_) sum += hz;
      return static_cast<float>(sum / static_cast<double>(noteHzs_.size()));
   }

   // Stamp the current note's final pitch (and velocity) from the lifetime.
   //
   // Domain context: the *chroma* (pitch class) is voted over all the note's
   // valid hz estimates — a piano note keeps the same pitch class across its
   // lifetime, so the vote is stable against hop-to-hop wobble. The *octave*
   // is anchored to the loudest hop (peakHz): the lifetime mean drifts to a
   // sub-octave on a decaying note (YIN is a harmonic estimator), so it is
   // used only when the note has no loudest-hop fallback. Velocity comes
   // from the loudest hop's RMS (peakRms).
   void stampPitch() {
      if (!isPlaying_ || noteHzs_.empty()) return;

      // Chroma vote over every valid estimate of this note's lifetime.
      int votes[12] = {0};
      for (float hz : noteHzs_) {
         double mf = 0.0;
         int c = pitchClassOf(hz, mf);
         if (mf >= 21.0 && mf <= 108.0) votes[c]++;
      }
      int best = -1;
      int bestCount = 0;
      for (int c = 0; c < 12; ++c) {
         if (votes[c] > bestCount) {
            bestCount = votes[c];
            best = c;
         }
      }
      if (best < 0) return;

      // Octave anchor: the loudest hop; fall back to the lifetime mean for
      // the very first note of a file (no current note to anchor by
      // continuity), then to a neutral A3.
      double base = peakHz();
      if (base <= 0.0f) base = meanHz();
      if (base <= 0.0f) base = 262.0;
      const double baseMidi = 69.0 + 12.0 * std::log2(base / 440.0);

      // The winning pitch class, re-anchored to the octave nearest `base`.
      int midi = best;
      while (midi < 21) midi += 12;
      while (midi > 108) midi -= 12;
      while (midi + 12 <= 108 &&
             std::abs(midi + 12 - baseMidi) < std::abs(midi - baseMidi)) {
         midi += 12;
      }
      while (midi - 12 >= 21 &&
             std::abs(midi - 12 - baseMidi) < std::abs(midi - baseMidi)) {
         midi -= 12;
      }
      currentNote_.pitch = static_cast<uint8_t>(std::clamp(midi, 21, 108));
      currentNote_.velocity = velocityFromPeakRms(peakRms);
   }
};

// ============================================================================
// Transcriber implementation
// ============================================================================

Transcriber::Transcriber(uint32_t bufSize, uint32_t hopSize, float silenceDb,
                         const std::string& pitchMethod, double tempoBpm)
   : impl_(std::make_unique<Impl>()) {
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->silenceDb = silenceDb;
   impl_->pitchMethod = pitchMethod;
   impl_->tempoBpm = tempoBpm;

   // Rolling analysis window and hop scratch buffer.
   impl_->window.assign(bufSize, 0.0f);
   impl_->hopBuf.assign(hopSize, 0.0f);

   // Pitch detector (YINfft by default). The tolerance is the YIN
   // minimum-search threshold; 0.1 matches aubio's aubiopitch CLI.
   impl_->pitchDetector = std::make_unique<PitchDetector>(bufSize, 0.1f);
   impl_->pitchDetector->setMethod(pitchMethod);
   impl_->pitchDetector->setHopSize(hopSize);

   // Onset detector (spectral flux by default). Constructed with a
   // placeholder sample rate; transcribe() sets the file's actual rate
   // before the first detect() call.
   impl_->onsetDetector =
      std::make_unique<OnsetDetector>("specflux", bufSize, hopSize, 48000);
   // aubioonset CLI parity: 0.3 peak threshold, 12 ms minioi.
   impl_->onsetDetector->setThreshold(0.3f);
   impl_->onsetDetector->setMinIoI(0.012);
}

Transcriber::~Transcriber() = default;

Score Transcriber::transcribe(const std::string& inputPath) const {
   // Per-hop transcription. See the file header for the gating and
   // pitch-resolution strategy; the body below is a single sequential
   // read driving energy, pitch, and onset in one pass.

   auto* p = impl_.get();

   AudioFileReader audioReader(inputPath);
   p->sampleRate_ = audioReader.sampleRate();
   p->onsetDetector->setSampleRate(p->sampleRate_);

   const uint32_t bufSize = p->bufSize;
   const uint32_t hopSize = p->hopSize;
   const uint32_t channels = audioReader.channels();
   // Stereo interleaved scratch (the reader downmixes in place).
   std::vector<float> rawBuf(hopSize * std::max(1u, channels));

   // One hop in seconds — used for the replacement-start offset.
   const double hopSec = static_cast<double>(hopSize) / p->sampleRate_;

   // Energy hysteresis thresholds, linear amplitude. The on threshold is
   // the user's silence level; the off threshold sits below it so a
   // decaying note that hovers near the on level cannot bounce on/off.
   const float onLinear = Impl::dbToLinear(p->silenceDb);
   const float offLinear =
      Impl::dbToLinear(p->silenceDb - Impl::kReleaseHysteresisDb);
   uint32_t releaseRun = 0; // consecutive hops below the off threshold.

   // Pitch hysteresis: consecutive hops agreeing on a candidate class.
   int lastCandidateClass = -1;
   uint32_t candidateRun = 0;

   // Resolve a note's final pitch (chroma vote + octave anchor) and velocity,
   // then close it. Replaces the old per-hop pendingHz flush: the hz and rms
   // estimates are accumulated on the Impl, so stamping reads them directly.
   auto stampAndClose = [&](double endTime) {
      if (!p->isPlaying_) return;
      p->stampPitch();
      p->finalizeCurrentNote(endTime);
   };

   uint64_t hopIndex = 0;
   while (!audioReader.eof()) {
      uint32_t framesRead = audioReader.readMono(rawBuf.data(), hopSize);
      if (framesRead == 0) break; // EOF.

      // readMono downmixes stereo in place: after a partial read near
      // EOF the tail of the buffer still holds stale samples from an
      // earlier full read. Zero the tail before any downstream use.
      if (framesRead < hopSize) {
         std::fill(rawBuf.begin() + framesRead, rawBuf.begin() + hopSize, 0.0f);
      }
      std::copy(rawBuf.begin(), rawBuf.begin() + hopSize, p->hopBuf.begin());

      // Energy of this hop (computed before the window shift so all
      // state in this iteration describes the same hop).
      const float rms = Impl::rmsEnergy(p->hopBuf.data(), hopSize);

      // Pitch over the window that just ended.
      //
      // Domain context: the analysis window lags the audio by bufSize -
      // hopSize samples (~33 ms here): the hop landing on an attack
      // carries mostly the PREVIOUS note's decay. Pitch is estimated
      // BEFORE the window shift; for an attack that is corrected by the
      // accumulation that follows, and for a decay it is the same note.
      const bool tonal = (rms > onLinear);
      float hz = 0.0f;
      if (tonal) {
         hz = p->pitchDetector->detect(p->window.data(), bufSize).first;
      }

      // Shift the hop into the rolling window.
      p->window.erase(p->window.begin(), p->window.begin() + hopSize);
      p->window.insert(p->window.end(), p->hopBuf.begin(), p->hopBuf.end());

      // Window-end timestamp: the window for hop i ends at sample
      // (i+1)*hop.
      const double windowEnd =
         p->sampleToTimestamp((hopIndex + 1) * static_cast<uint64_t>(hopSize));

      // Onset detection over this hop (edge-detected internally).
      const bool onset = p->onsetDetector->detect(p->hopBuf.data(), hopSize);

      if (onset && tonal) {
         // A new note just started. Close the in-flight note (stamping
         // its pitch from its loudest hop) if there is one, then open the
         // replacement. The new note is stamped with its own accumulated
         // lifetime, so the attack's lagged window is harmless here.
         if (p->isPlaying_) {
            stampAndClose(windowEnd);
         }
         if (Impl::hzToMidiNote(hz) >= 21) {
            p->startNewNote(windowEnd);
            p->recordHz(hz, rms);
            p->peakRms = std::max(p->peakRms, rms);
         }
      } else if (p->isPlaying_) {
         // Sustained note: accumulate pitch + peak energy, decide whether
         // it ends. This hop belongs to the current note, so it is the
         // loudest candidate for velocity and (via peakHz) the octave.
         if (hz > 0.0f) {
            p->recordHz(hz, rms);
         }
         p->peakRms = std::max(p->peakRms, rms);

         // (a) Energy hysteresis: end after N consecutive quiet hops.
         releaseRun = (rms < offLinear) ? releaseRun + 1 : 0;
         if (releaseRun >= Impl::kReleaseHops) {
            stampAndClose(windowEnd);
         }
         // (b) Pitch hysteresis: end + replace when K hops agree that
         // the pitch class has moved. The replacement is allowed only once
         // the in-flight note has a credible lifetime; younger than that,
         // the change is a sub-octave wobble of the *same* note, so we
         // just keep it (no fragment, no spurious new note).
         else if (hz > 0.0f) {
            double mf = 0.0;
            int c = Impl::pitchClassOf(hz, mf);
            const bool sameNote =
               (Impl::hzToMidiNote(hz) == p->currentNote_.pitch);
            if (!sameNote) {
               candidateRun = (c == lastCandidateClass) ? candidateRun + 1 : 1;
               lastCandidateClass = c;
               if (candidateRun >= Impl::kPitchStability) {
                  // The window for the change hop *started* one hop earlier
                  // (a note begins when the new pitch enters the window), so
                  // back-date the replacement start by one hop.
                  const double replacementStart = windowEnd - hopSec;
                  if (p->currentNoteHops_ >= Impl::kMinReplaceHops) {
                     stampAndClose(windowEnd);
                     p->startNewNote(replacementStart);
                  } else {
                     // Too young to be a real note change: the pitch is
                     // settling within the same sustained note. Finalize it
                     // (drops if it is shorter than the min lifetime) and do
                     // not open a replacement.
                     stampAndClose(windowEnd);
                  }
               }
            } else {
               candidateRun = 0;
               lastCandidateClass = -1;
            }
         } else {
            candidateRun = 0;
            lastCandidateClass = -1;
         }
      } else {
         // IDLE: no note to track; reset the change counter.
         candidateRun = 0;
         lastCandidateClass = -1;
      }

      // Advance the in-flight note's hop counter (a tonal hop counts toward
      // its lifetime for the min-lifetime and min-replacement guards).
      if (p->isPlaying_) ++p->currentNoteHops_;

      ++hopIndex;
   }

   // A note still active at EOF: stamp it and end it at the last sample
   // read. (finalizeCurrentNote drops it if it is shorter than the min
   // lifetime, which filters trailing wobble fragments.)
   if (p->isPlaying_) {
      stampAndClose(
         p->sampleToTimestamp(static_cast<uint64_t>(hopIndex) * hopSize));
   }

   // Merge same-pitch fragments: a decaying note is closed and re-opened
   // hop to hop by the onset/spectral-wobble state machine, so what is one
   // physical note arrives as a run of same-pitch fragments. Merge runs of
   // the same pitch whose fragments are closer together than a full new
   // attack (a quarter-note gap at the working tempo) into a single note,
   // keeping the earliest start and the latest end.
   const double maxGap = 60.0 / p->tempoBpm * 0.5; // one quarter-note gap.
   std::vector<Note> merged;
   for (const Note& n : p->notes_) {
      if (!merged.empty() && merged.back().pitch == n.pitch &&
          n.startTime - merged.back().endTime <= maxGap) {
         merged.back().endTime = n.endTime;
      } else {
         merged.push_back(n);
      }
   }

   // Build and return the Score (HIR). The tempo comes from the user's CLI
   // option (previously discarded); notes are in performance order.
   Score score;
   score.notes = std::move(merged);
   score.tempo = p->tempoBpm;
   score.title = "";
   score.composer = "";

   return score;
}

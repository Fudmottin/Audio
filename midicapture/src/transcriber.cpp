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
 * @section pitch Pitch: chroma voting with octave re-anchoring
 *
 * A sampled instrument is rarely perfectly tuned, so a single hop's
 * YINfft estimate can land on either side of a semitone boundary and a
 * raw round-to-nearest-MIDI would flip note number hop to hop. The
 * fundamental is therefore resolved in two stages:
 *  - **Chroma voting.** Every a note's valid hz estimates are folded
 *    to one of 12 pitch classes (Hz mod one octave, nearest); the
 *    majority pitch class is the note's chroma. A piano note keeps the
 *    same pitch class across its lifetime, so the vote is stable.
 *  - **Octave re-anchoring.** A pitch class has no octave, so the
 *    note's octave is chosen as the one whose MIDI value is closest to
 *    the current note's (continuity). For the *first* note of a file
 *    (no current note) the octave that best matches the hz data is used.
 *
 * This makes pitch robust to: (a) a detuned or octave-error fundamental,
 * (b) harmonics (a double period folds to the same chroma), and (c)
 * wobble across a semitone boundary. Note *changes* additionally require
 * `kPitchStability` consecutive hops to agree (pitch hysteresis) so a
 * single stray hop cannot fragment a note.
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
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <libaudio/libaudio.h>
#include <libaudio/onset.h>
#include <stdexcept>
#include <string>
#include <vector>

// Forward declarations of our own modules.
#include <midicapture/audioFile.h>
#include <midicapture/transcriber.h>

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
   static constexpr uint32_t kPitchStability = 3;

   // Analysis parameters.
   uint32_t bufSize;
   uint32_t hopSize;
   float silenceDb;
   std::string pitchMethod;

   // Analysis objects (Pimpl pattern).
   std::unique_ptr<PitchDetector> pitchDetector;
   std::unique_ptr<OnsetDetector> onsetDetector;

   // Rolling buffer of the last bufSize samples. Each hop, the oldest
   // hopSize samples are shifted out and the new hop is appended, so
   // pitch detection always runs over the window that just ended.
   std::vector<float> window;
   // Hop-sized scratch buffer for the next chunk of input.
   std::vector<float> hopBuf;

   // Per-note fundamental-frequency accumulator, in Hz^2.
   //
   // Domain context: a note's hz estimates are accumulated so the
   // lifetime *mean* (a robust center, not a single wobbly hop) can be
   // used for octave placement. Squaring avoids needing sqrt in the
   // running sum; the count tracks how many valid estimates were added.
   double noteHz2Sum = 0.0;
   uint32_t noteHzCount = 0;

   // Current note being tracked (monophonic state machine).
   bool isPlaying_ = false;
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
   void finalizeCurrentNote(double endTime) {
      if (isPlaying_ && currentNote_.startTime < endTime) {
         currentNote_.endTime = endTime;
         notes_.push_back(currentNote_);
      }
      isPlaying_ = false;
      currentNote_ = Note();
      noteHz2Sum = 0.0;
      noteHzCount = 0;
   }

   // Start a new note, resetting the fundamental accumulator.
   void startNewNote(double startTime, uint8_t velocity) {
      isPlaying_ = true;
      currentNote_ = Note();
      currentNote_.startTime = startTime;
      currentNote_.velocity = velocity;
      currentNote_.channel = 0; // Channel 0 = Acoustic Grand Piano.
      currentNote_.sustain = false;
      noteHz2Sum = 0.0;
      noteHzCount = 0;
   }

   // Record a valid hz estimate into the current note's accumulator.
   void recordHz(float hz) {
      if (hz > 0.0f) {
         noteHz2Sum += static_cast<double>(hz) * static_cast<double>(hz);
         ++noteHzCount;
      }
   }

   // The note's lifetime mean fundamental (Hz), or 0 if none recorded.
   float meanHz() const {
      if (noteHzCount == 0) return 0.0f;
      return static_cast<float>(std::sqrt(
         noteHz2Sum / static_cast<double>(noteHzCount)));
   }
};

// ============================================================================
// Transcriber implementation
// ============================================================================

Transcriber::Transcriber(uint32_t bufSize, uint32_t hopSize, float silenceDb,
                         const std::string& pitchMethod)
   : impl_(std::make_unique<Impl>()) {
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->silenceDb = silenceDb;
   impl_->pitchMethod = pitchMethod;

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

   AudioFile audioReader(inputPath);
   p->sampleRate_ = audioReader.sampleRate();
   p->onsetDetector->setSampleRate(p->sampleRate_);

   const uint32_t bufSize = p->bufSize;
   const uint32_t hopSize = p->hopSize;
   const uint32_t channels = audioReader.channels();
   // Stereo interleaved scratch (the reader downmixes in place).
   std::vector<float> rawBuf(hopSize * std::max(1u, channels));

   // [DIAG] per-hop logging to stderr (enable with MIDICAPTURE_DIAG=1).
   const bool diag = (std::getenv("MIDICAPTURE_DIAG") != nullptr);

   // [DIAG] last logged per-note state (for "notable hop" detection).
   uint8_t dbgLastPitch = 255;
   double  dbgLastLogHz = -1.0;
   double  dbgLastLogDb = -120.0;
   uint64_t dbgLastLogHop = 0;

   // Energy hysteresis thresholds, linear amplitude. The on threshold is
   // the user's silence level; the off threshold sits below it so a
   // decaying note that hovers near the on level cannot bounce on/off.
   const float onLinear = Impl::dbToLinear(p->silenceDb);
   const float offLinear = Impl::dbToLinear(
      p->silenceDb - Impl::kReleaseHysteresisDb);
   uint32_t releaseRun = 0; // consecutive hops below the off threshold.

   // Pitch hysteresis: consecutive hops agreeing on a candidate class.
   int lastCandidateClass = -1;
   uint32_t candidateRun = 0;

   // Window for the in-flight note. When a note is finalized we keep the
   // hz estimates seen *since* its onset so the final pitch (mean over
   // the lifetime) can be stamped on it.
   std::vector<float> pendingHz;
   auto flushPending = [&]() {
      if (p->isPlaying_ && !pendingHz.empty()) {
         // Resolve the note's pitch from all its valid estimates.
         int votes[12] = {0};
         for (float hz : pendingHz) {
            double mf = 0.0;
            int c = Impl::pitchClassOf(hz, mf);
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
         if (best >= 0) {
            // Re-anchor the octave: the MIDI value with the winning
            // pitch class that is closest to the current note's pitch
            // (continuity), falling back to the mean-hz octave for the
            // first note of the file.
            double base =
               (p->currentNote_.pitch >= 21)
                  ? static_cast<double>(p->currentNote_.pitch)
                  : 60.0;
            int midi = best;
            while (midi < 21) midi += 12;
            while (midi > 108) midi -= 12;
            // Move by whole octaves to the value nearest `base`.
            while (midi + 12 <= 108 && std::abs(midi + 12 - base) <
                                          std::abs(midi - base)) {
               midi += 12;
            }
            while (midi - 12 >= 21 && std::abs(midi - 12 - base) <
                                         std::abs(midi - base)) {
               midi -= 12;
            }
            p->currentNote_.pitch = static_cast<uint8_t>(midi);
         }
      }
      pendingHz.clear();
   };

   uint64_t hopIndex = 0;
   while (!audioReader.eof()) {
      uint32_t framesRead = audioReader.readMono(rawBuf.data(), hopSize);
      if (framesRead == 0) break; // EOF.

      // readMono downmixes stereo in place: after a partial read near
      // EOF the tail of the buffer still holds stale samples from an
      // earlier full read. Zero the tail before any downstream use.
      if (framesRead < hopSize) {
         std::fill(rawBuf.begin() + framesRead, rawBuf.begin() + hopSize,
                   0.0f);
      }
      std::copy(rawBuf.begin(), rawBuf.begin() + hopSize,
                 p->hopBuf.begin());

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
      const bool validPitch = (hz > 0.0f);

      // Shift the hop into the rolling window.
      p->window.erase(p->window.begin(), p->window.begin() + hopSize);
      p->window.insert(p->window.end(), p->hopBuf.begin(),
                       p->hopBuf.end());

      // Window-end timestamp: the window for hop i ends at sample
      // (i+1)*hop.
      const double windowEnd = p->sampleToTimestamp(
         (hopIndex + 1) * static_cast<uint64_t>(hopSize));

      // Onset detection over this hop (edge-detected internally).
      const bool onset = p->onsetDetector->detect(p->hopBuf.data(), hopSize);

      // Velocity from the hop RMS, scaled so a rendered piano note
      // (~0.05 peak RMS) lands mid-scale (~90).
      auto velocityFromRms = [rms]() {
         return static_cast<uint8_t>(std::max(1.0, std::min(127.0,
                                                             rms * 1800.0)));
      };

      if (onset && tonal) {
         // A new note just started.
         flushPending();
         if (p->isPlaying_) {
            p->finalizeCurrentNote(windowEnd); // replaced by the new note.
         }
         if (Impl::hzToMidiNote(hz) >= 21) {
            p->startNewNote(windowEnd, velocityFromRms());
            // Seed the accumulator with this hop's estimate; subsequent
            // hops add to it so the mean is over the note's lifetime.
            p->recordHz(hz);
            pendingHz.push_back(hz);
         }
      } else if (p->isPlaying_) {
         // Sustained note: accumulate pitch, decide whether it ends.
         if (validPitch) {
            p->recordHz(hz);
            pendingHz.push_back(hz);
         }

         // (a) Energy hysteresis: end after N consecutive quiet hops.
         releaseRun = (rms < offLinear) ? releaseRun + 1 : 0;
         if (releaseRun >= Impl::kReleaseHops) {
            flushPending();
            p->finalizeCurrentNote(windowEnd);
         }
         // (b) Pitch hysteresis: end + replace when K hops agree that
         // the pitch class has moved.
         else if (validPitch) {
            double mf = 0.0;
            int c = Impl::pitchClassOf(hz, mf);
            const bool sameNote = (Impl::hzToMidiNote(hz) ==
                                   p->currentNote_.pitch);
            if (!sameNote) {
               candidateRun = (c == lastCandidateClass) ? candidateRun + 1
                                                        : 1;
               lastCandidateClass = c;
               if (candidateRun >= Impl::kPitchStability) {
                  flushPending();
                  p->finalizeCurrentNote(windowEnd);
                  p->startNewNote(windowEnd, velocityFromRms());
                  p->recordHz(hz);
                  pendingHz.push_back(hz);
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

      ++hopIndex;
   }

   // A note still active at EOF: stamp its lifetime-mean pitch and end it
   // at the last sample read.
   if (p->isPlaying_) {
      flushPending();
      p->finalizeCurrentNote(
         p->sampleToTimestamp(static_cast<uint64_t>(hopIndex) * hopSize));
   }

   // Build and return the Score (HIR).
   Score score;
   score.notes = std::move(p->notes_);
   score.tempo = 120.0; // Default tempo (set explicitly by the CLI).
   score.title = "";
   score.composer = "";

   return score;
}

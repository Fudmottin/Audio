/**
 * @file transcriber.cpp
 * @brief Implementation of Transcriber — audio-to-MIDI transcription.
 *
 * This module implements the Transcriber class, which orchestrates
 * the full transcription pipeline:
 * 1. Open the input audio file (via AudioFile).
 * 2. Run pitch detection (YINfft) on each audio frame.
 * 3. Run onset detection (spectral flux) on each frame.
 * 4. Build a HIR Score with detected notes.
 * 5. Return the Score (HIR) for MIDI writing.
 *
 * The monophonic prototype uses a simple state machine:
 * - IDLE: No note currently active. Waiting for onset.
 * - PLAYING: A note is active. Watching for note-off.
 *
 */

#include <cmath>
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
// This makes the rest of the codebase framework-free (except for
// the transcriber.h header which only includes transcriber.h).
//
// The state machine has two states:
// - isPlaying_ = false: IDLE — waiting for onset.
// - isPlaying_ = true: PLAYING — a note is active.
//
// RAII — all resources are automatically freed when the Impl is
// destroyed.
// ============================================================================
struct Transcriber::Impl {
   // Analysis parameters.
   uint32_t bufSize;
   uint32_t hopSize;
   float confidenceThreshold;
   float silenceDb;
   std::string pitchMethod;

   // Analysis objects (Pimpl pattern).
   std::unique_ptr<PitchDetector> pitchDetector;
   std::unique_ptr<OnsetDetector> onsetDetector;

   // Current note being tracked (monophonic state machine).
   bool isPlaying_ = false;
   Note currentNote_;

   // Results.
   std::vector<Note> notes_;

   // Sample rate (set during transcribe()).
   uint32_t sampleRate_ = 48000;

   // Frame counter (for timestamp calculation).
   uint32_t frameCount_ = 0;

   // Convert aubio pitch (float) to MIDI note number (uint8_t).
   //
   // Domain context: aubio returns pitch as a float in MIDI note numbers.
   // We round to the nearest integer and clamp to the piano range (21–108).
   // This is the standard conversion used throughout the project.
   static uint8_t pitchToMidiNote(float pitch) {
      uint8_t note = static_cast<uint8_t>(std::round(pitch));
      // Clamp to piano range (A0 = 21, C8 = 108).
      return static_cast<uint8_t>(
         std::max(21u, std::min(108u, static_cast<unsigned>(note))));
   }

   // Convert dB to linear amplitude.
   //
   // Domain context: The silence threshold is in dB. We convert it to
   // linear amplitude for comparison with the RMS energy of each frame.
   // silenceDb = -40 dB → linear = 0.01 (1% of full scale).
   static float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

   // Calculate the RMS energy of a buffer.
   //
   // Domain context: RMS energy is used to determine if a frame is
   // "silent" (below the silence threshold). Frames below this
   // threshold are ignored during pitch detection.
   static float rmsEnergy(const float* buffer, uint32_t length) {
      float sum = 0.0f;
      for (uint32_t i = 0; i < length; ++i) {
         float sample = buffer[i];
         sum += sample * sample;
      }
      return std::sqrt(sum / static_cast<float>(length));
   }

   // Convert frame count to timestamp in seconds.
   //
   // Domain context: Each frame is hopSize samples apart. The timestamp
   // is calculated as: frameCount * hopSize / sampleRate.
   // This gives the time in seconds from the start of the recording.
   double frameToTimestamp(uint32_t frameCount) const {
      return static_cast<double>(frameCount) * static_cast<double>(hopSize) /
             static_cast<double>(sampleRate_);
   }

   // Finalize the current note (end it) and add it to the results.
   //
   // Domain context: When a note ends (confidence drops below threshold,
   // or a new onset is detected), we finalize the current note by
   // setting its end time and adding it to the results vector.
   void finalizeCurrentNote(double endTime) {
      if (isPlaying_ && currentNote_.startTime < endTime) {
         currentNote_.endTime = endTime;
         notes_.push_back(currentNote_);
      }
      isPlaying_ = false;
      currentNote_ = Note();
   }

   // Start a new note from pitch and onset data.
   //
   // Domain context: When pitch confidence exceeds the threshold AND
   // an onset is detected, we start a new note. The note's pitch
   // and velocity are set from the pitch detection result.
   void startNewNote(float pitchMidi, float confidence, double startTime,
                     uint8_t velocity) {
      isPlaying_ = true;
      currentNote_ = Note();
      currentNote_.startTime = startTime;
      currentNote_.pitch = pitchToMidiNote(pitchMidi);
      currentNote_.velocity = velocity;
      currentNote_.channel = 0; // Channel 0 = Acoustic Grand Piano.
      currentNote_.sustain = false;
   }
};

// ============================================================================
// Transcriber implementation
// ============================================================================

Transcriber::Transcriber(uint32_t bufSize, uint32_t hopSize,
                         float confidenceThreshold, float silenceDb,
                         const std::string& pitchMethod)
   : impl_(std::make_unique<Impl>()) {
   // Store analysis parameters.
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->confidenceThreshold = confidenceThreshold;
   impl_->silenceDb = silenceDb;
   impl_->pitchMethod = pitchMethod;

   // Create the pitch detector (YINfft by default).
   impl_->pitchDetector = std::make_unique<PitchDetector>(bufSize, 0.15f);
   impl_->pitchDetector->setMethod(pitchMethod);
   impl_->pitchDetector->setConfidenceThreshold(confidenceThreshold);
   impl_->pitchDetector->setHopSize(hopSize);

   // Create the onset detector (spectral flux by default).
   impl_->onsetDetector =
      std::make_unique<OnsetDetector>("specflux", bufSize, hopSize,
                                      48000); // Default sample rate.
}

Transcriber::~Transcriber() = default;

Score Transcriber::transcribe(const std::string& inputPath) const {
   // Transcribe an audio file to a HIR Score.
   //
   // Domain context: The transcription pipeline:
   // 1. Open the input audio file.
   // 2. Read audio frames (mono downmix).
   // 3. For each frame:
   //    a. Calculate RMS energy (is the frame "silent"?).
   //    b. If not silent, run pitch detection.
   //    c. If pitch confidence exceeds threshold, check for onset.
   //    d. If onset detected and not playing, start a new note.
   //    e. If pitch confidence drops below threshold, end the note.
   // 4. Return the Score (HIR) with all detected notes.

   // Finalize the Impl pointer for const correctness.
   auto* p = impl_.get();

   // Open the input audio file.
   AudioFile audioReader(inputPath);

   // Update the sample rate in the Impl.
   // Note: we need a non-const pointer for this. We use a workaround:
   // cast away constness (the Impl is owned by the Transcriber, not const).
   const_cast<uint32_t&>(p->sampleRate_) = audioReader.sampleRate();

   // Update the onset detector's sample rate.
   // Note: we need a non-const pointer for this.
   const_cast<std::string&>(p->pitchMethod) = p->pitchMethod; // no-op.

   // Allocate the audio buffer.
   std::vector<float> buffer(p->bufSize);

   // Process the audio frame by frame.
   while (!audioReader.eof()) {
      uint32_t framesRead = audioReader.readMono(buffer.data(), p->bufSize);
      if (framesRead == 0) break; // EOF.

      // Calculate RMS energy of the current frame.
      float rms = Impl::rmsEnergy(buffer.data(), framesRead);
      float silenceLinear = Impl::dbToLinear(p->silenceDb);

      // Skip silent frames (below silence threshold).
      if (rms < silenceLinear) {
         // If we were playing a note, end it.
         double endTime = p->frameToTimestamp(p->frameCount_);
         p->finalizeCurrentNote(endTime);
         ++p->frameCount_;
         continue;
      }

      // Run pitch detection.
      auto [pitchMidi, confidence] =
         p->pitchDetector->detect(buffer.data(), framesRead);

      // Calculate the timestamp for this frame.
      double timestamp = p->frameToTimestamp(p->frameCount_);

      // Check if we have a confident pitch detection.
      bool hasConfidentPitch = (confidence >= p->confidenceThreshold);

      if (hasConfidentPitch && pitchMidi > 0.0f) {
         // Check for onset.
         bool onsetDetected =
            p->onsetDetector->detect(buffer.data(), framesRead);

         if (onsetDetected && !p->isPlaying_) {
            // Start a new note (onset detected, not currently playing).
            // Estimate velocity from RMS energy (0–127).
            uint8_t velocity = static_cast<uint8_t>(
               std::min(127.0, rms * 254.0) + 0.5); // Scale to 0–127.
            velocity = static_cast<uint8_t>(
               std::max(1u, std::min(127u, static_cast<unsigned>(velocity))));

            p->startNewNote(pitchMidi, confidence, timestamp, velocity);
         } else if (p->isPlaying_) {
            // Update the current note's pitch (in case pitch changed).
            // For monophonic, we keep the current note going.
            // (In polyphony, this would trigger a note change.)
         }
      } else {
         // Confidence below threshold — end the current note.
         double endTime = timestamp;
         p->finalizeCurrentNote(endTime);
      }

      ++p->frameCount_;
   }

   // Build and return the Score (HIR).
   Score score;
   score.notes = std::move(p->notes_);
   score.tempo = 120.0; // Default tempo.
   score.title = "";
   score.composer = "";

   return score;
}

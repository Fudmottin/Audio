/**
 * @file notes.cpp
 * @brief Implementation of NoteDetector — note detection via aubio.
 *
 * This module wraps aubio's note detection. It combines onset detection,
 * pitch estimation, velocity analysis, and note-off detection into a
 * single interface.
 *
 * @section algorithmic-limitations Algorithmic Limitations
 *
 * These are the known limitations of aubio's note detection that
 * affect transcription quality:
 *
 * 1. **Monophonic assumption** — aubio's default note detector
 *    assumes one note at a time. For polyphonic content, use
 *    harmonic binning (spectral analysis) for multi-note separation.
 *
 * 2. **Harmonic overlap** — Piano harmonics from multiple notes blur
 *    frequency analysis. Mitigate with template matching or NMF
 *    (future enhancement).
 *
 * 3. **Pedal + polyphony** — Resonating harmonics from sustained
 *    notes blur pitch detection. Detect the pedal separately
 *    (low-frequency energy analysis) to improve note separation.
 *
 * 4. **Note overlap (legato)** — When note A is held while note B
 *    starts, determining when A ends is ambiguous. Use aubio's
 *    note-off detection (release drop level) to refine note
 *    boundaries.
 *
 * 5. **Silent passages** — No notes detected during rests. Use
 *    onset detection to mark rest boundaries, ensuring accurate
 *    timing for the final score.
 *
 * @see lode/libaudio/summary.md — Algorithmic limitations
 */

#include <libaudio/notes.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/notes/notes.h>
#include <stdexcept>

// ============================================================================
// NoteDetector::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All aubio C API calls are isolated here.
//
// RAII — aubio resources are automatically freed when
// the Impl is destroyed.
// ============================================================================
struct NoteDetector::Impl {
   aubio_notes_t* detector = nullptr;
   fvec_t* inputBuffer = nullptr;
   std::string method;
   uint32_t bufSize;
   uint32_t hopSize;
   uint32_t sampleRate;
   float silenceThreshold = -40.0f;
   double minIoIMs_ = 10.0;
   float releaseDropDb = 10.0f;
   uint32_t currentFrame = 0;

   ~Impl() {
      if (detector) del_aubio_notes(detector);
      if (inputBuffer) del_fvec(inputBuffer);
   }
};

// ============================================================================
// NoteDetector implementation
// RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

NoteDetector::NoteDetector(std::string_view method, uint32_t bufSize,
                           uint32_t hopSize, uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   // Create the aubio note detector.

   impl_->method = std::string(method);
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->sampleRate = sampleRate;

   // Create the note detector with the specified method.
   impl_->detector = new_aubio_notes(impl_->method.c_str(), bufSize,
                                     hopSize, sampleRate);

   // Allocate input buffer.
   impl_->inputBuffer = new_fvec(bufSize);

   // Set the silence threshold, minimum IoI (in ms),
   // and release drop level.
   // Aubio_notes_set_minioi_ms takes milliseconds,
   // aubio_notes_set_release_drop takes dB.
   aubio_notes_set_silence(impl_->detector, impl_->silenceThreshold);
   aubio_notes_set_minioi_ms(impl_->detector,
                             static_cast<float>(impl_->minIoIMs_));
   aubio_notes_set_release_drop(impl_->detector, impl_->releaseDropDb);
}

NoteDetector::~NoteDetector() = default;

NoteDetector::NoteDetector(NoteDetector&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

NoteDetector& NoteDetector::operator=(NoteDetector&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

std::optional<NoteEvent> NoteDetector::detect(const float* samples,
                                              uint32_t length) {
   // Detect notes in a buffer of audio samples.

   if (impl_ == nullptr || impl_->detector == nullptr) {
      return std::nullopt;
   }

   if (length != impl_->bufSize) {
      throw std::invalid_argument(
         "Sample length must equal buffer size");
   }

   // Copy samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));

   // Run note detection.
   // aubio_notes_do takes 3 args: (detector, input, output_fvec)
   // and returns void. The output fvec holds:
   //   [0] = MIDI note value (0 if no note)
   //   [1] = note velocity
   //   [2] = MIDI note-off value

   fvec_t* output = new_fvec(3);
   aubio_notes_do(impl_->detector, impl_->inputBuffer, output);

   // Extract note event data from output buffer.
   // aubio_notes_do writes [pitch (float), velocity (float), noteOff (float)].
   NoteEvent event;
   event.pitchMidi = output->data[0];
   event.velocity = output->data[1];
   event.noteOffMidi = output->data[2];

   del_fvec(output);

   if (event.pitchMidi <= 0.0f) {
      return std::nullopt;  // No note detected.
   }

   impl_->currentFrame += length;
   return event;
}

float NoteDetector::silenceThreshold() const {
   // Simple accessor.
   return impl_ ? impl_->silenceThreshold : -40.0f;
}

void NoteDetector::setSilenceThreshold(float threshold) {
   // Set the silence threshold.

   if (impl_ && impl_->detector) {
      impl_->silenceThreshold = threshold;
      aubio_notes_set_silence(impl_->detector, threshold);
   }
}

double NoteDetector::minIoIMs() const {
   // Simple accessor.
   return impl_ ? impl_->minIoIMs_ : 10.0;
}

void NoteDetector::setMinIoIMs(double ms) {
   // Set the minimum time between onsets.
   // Aubio_notes_set_minioi_ms takes milliseconds,
   // not samples — this is the correct API for millisecond-based IoI.

   if (impl_ && impl_->detector) {
      impl_->minIoIMs_ = ms;
      aubio_notes_set_minioi_ms(impl_->detector, static_cast<float>(ms));
   }
}

float NoteDetector::releaseDropDb() const {
   // Simple accessor.
   return impl_ ? impl_->releaseDropDb : 10.0f;
}

void NoteDetector::setReleaseDropDb(float db) {
   // Set the note-off release drop level.
   // Aubio_notes_set_release_drop takes dB.

   if (impl_ && impl_->detector) {
      impl_->releaseDropDb = db;
      aubio_notes_set_release_drop(impl_->detector, db);
   }
}

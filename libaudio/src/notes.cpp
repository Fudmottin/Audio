/**
 * @file notes.cpp
 * @brief Implementation of NoteDetector — note detection via aubio.
 *
 * This module wraps aubio's note detection. It combines onset detection,
 * pitch estimation, velocity analysis, and note-off detection into a
 * single interface.
 *
 * @see lode/libaudio/summary.md — Module overview and API design
 * @see lode/libaudio/decisions.md — Default parameters
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
// Core Guidelines: RAII — aubio resources are automatically freed when
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
// Core Guidelines: RAII resource management — aubio resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

NoteDetector::NoteDetector(std::string_view method, uint32_t bufSize,
                           uint32_t hopSize, uint32_t sampleRate)
   : impl_(std::make_unique<Impl>()) {
   // Core Guidelines: create the aubio note detector.

   impl_->method = std::string(method);
   impl_->bufSize = bufSize;
   impl_->hopSize = hopSize;
   impl_->sampleRate = sampleRate;

   // Core Guidelines: create the note detector with the specified method.
   impl_->detector = new_aubio_notes(impl_->method.c_str(), bufSize,
                                     hopSize, sampleRate);

   // Core Guidelines: allocate input buffer.
   impl_->inputBuffer = new_fvec(bufSize);

   // Core Guidelines: set the silence threshold, minimum IoI (in ms),
   // and release drop level.
   // Core Guidelines: aubio_notes_set_minioi_ms takes milliseconds,
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
   // Core Guidelines: detect notes in a buffer of audio samples.

   if (impl_ == nullptr || impl_->detector == nullptr) {
      return std::nullopt;
   }

   if (length != impl_->bufSize) {
      throw std::invalid_argument(
         "Sample length must equal buffer size");
   }

   // Core Guidelines: copy samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));

   // Core Guidelines: run note detection.
   // aubio_notes_do takes 3 args: (detector, input, output_fvec)
   // and returns void. The output fvec holds:
   //   [0] = MIDI note value (0 if no note)
   //   [1] = note velocity
   //   [2] = MIDI note-off value

   fvec_t* output = new_fvec(3);
   aubio_notes_do(impl_->detector, impl_->inputBuffer, output);

   // Core Guidelines: extract note event data from output buffer.
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
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->silenceThreshold : -40.0f;
}

void NoteDetector::setSilenceThreshold(float threshold) {
   // Core Guidelines: set the silence threshold.

   if (impl_ && impl_->detector) {
      impl_->silenceThreshold = threshold;
      aubio_notes_set_silence(impl_->detector, threshold);
   }
}

double NoteDetector::minIoIMs() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->minIoIMs_ : 10.0;
}

void NoteDetector::setMinIoIMs(double ms) {
   // Core Guidelines: set the minimum time between onsets.
   // Core Guidelines: aubio_notes_set_minioi_ms takes milliseconds,
   // not samples — this is the correct API for millisecond-based IoI.

   if (impl_ && impl_->detector) {
      impl_->minIoIMs_ = ms;
      aubio_notes_set_minioi_ms(impl_->detector, static_cast<float>(ms));
   }
}

float NoteDetector::releaseDropDb() const {
   // Core Guidelines: simple accessor.
   return impl_ ? impl_->releaseDropDb : 10.0f;
}

void NoteDetector::setReleaseDropDb(float db) {
   // Core Guidelines: set the note-off release drop level.
   // Core Guidelines: aubio_notes_set_release_drop takes dB.

   if (impl_ && impl_->detector) {
      impl_->releaseDropDb = db;
      aubio_notes_set_release_drop(impl_->detector, db);
   }
}

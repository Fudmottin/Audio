#ifdef LIBAUDIO_HAS_RUBBERBAND

#include <libaudio/rubberband.h>
#include <rubberband/RubberBandStretcher.h>
#include <rubberband/RubberBandLiveShifter.h>
#include <stdexcept>
#include <memory>

// ============================================================================
// RubberbandProcessor::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All rubberband C++ API calls are isolated here.
// The public interface never exposes rubberband types.
//
// Core Guidelines: RAII — rubberband resources are automatically freed
// when the Impl is destroyed.
// ============================================================================
struct RubberbandProcessor::Impl {
   std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
   std::unique_ptr<RubberBand::RubberBandLiveShifter> liveShifter;
   uint32_t sampleRate;
   uint32_t channels;
   float timeStretchFactor = 1.0f;
   float pitchShiftSemitones = 0.0f;
   float tempoBpm = 120.0f;

   ~Impl() = default;
};

// ============================================================================
// RubberbandProcessor implementation
// Core Guidelines: RAII resource management — rubberband resources are
// automatically freed when the C++ object is destroyed.
// ============================================================================

RubberbandProcessor::RubberbandProcessor(uint32_t sampleRate,
                                        uint32_t channels)
   : impl_(std::make_unique<Impl>()) {
   // Core Guidelines: create the rubberband stretcher.
   // Core Guidelines: RubberBand 4.0.0 uses OptionProcessRealTime and
   // OptionEngineFaster flags instead of quality constants.

   impl_->sampleRate = sampleRate;
   impl_->channels = channels;

   // Core Guidelines: create the RubberBandStretcher with real-time mode
   // and the Faster (R2) engine (default for compatibility).
   // Core Guidelines: RubberBand 4.0.0 constructor takes (sampleRate,
   // channels, options, initialTimeRatio, initialPitchScale).
   impl_->stretcher = std::make_unique<RubberBand::RubberBandStretcher>(
      sampleRate, static_cast<size_t>(channels),
      RubberBand::RubberBandStretcher::OptionProcessRealTime |
      RubberBand::RubberBandStretcher::OptionEngineFaster,
      1.0, 1.0);
}

RubberbandProcessor::~RubberbandProcessor() = default;

RubberbandProcessor::RubberbandProcessor(RubberbandProcessor&& other) noexcept
   : impl_(std::move(other.impl_)) {
   // Core Guidelines: move constructor. Transfer ownership of all
   // rubberband resources from the source object.
   other.impl_ = std::make_unique<Impl>();
}

RubberbandProcessor& RubberbandProcessor::operator=(
   RubberbandProcessor&& other) noexcept {
   // Core Guidelines: move assignment operator. Release current resources
   // and take ownership of the source object's resources.

   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

std::vector<float> RubberbandProcessor::process(
   const std::vector<float>& samples) {
   // Core Guidelines: process a block of audio samples through the
   // rubberband stretcher.
   // Core Guidelines: RubberBand 4.0.0 uses process() and retrieve()
   // instead of feed() and getOutput().

   if (impl_ == nullptr || impl_->stretcher == nullptr) {
      return {};
   }

   // Core Guidelines: feed samples into the stretcher using process().
   // process() takes (const float *const *input, size_t samples, bool final).
   // For mono, we pass a single-channel pointer array.
   const float* inputPtr = samples.data();
   impl_->stretcher->process(&inputPtr,
                             static_cast<size_t>(samples.size()), false);

   // Core Guidelines: get processed output using retrieve().
   // retrieve() takes (float *const *output, size_t samples).
   size_t numAvailable = impl_->stretcher->available();
   if (numAvailable == 0) {
      return {};
   }

   std::vector<float> output(numAvailable);
   float* outputPtr = output.data();
   impl_->stretcher->retrieve(&outputPtr, numAvailable);

   return output;
}

void RubberbandProcessor::setTimeStretch(float factor) {
   // Core Guidelines: set the time-stretch factor.
   // Core Guidelines: RubberBand 4.0.0 uses setTimeRatio() instead of
   // setTimeFactor().

   if (impl_) {
      impl_->timeStretchFactor = factor;
      if (impl_->stretcher) {
         impl_->stretcher->setTimeRatio(static_cast<double>(factor));
      }
   }
}

void RubberbandProcessor::setPitchShift(float semitones) {
   // Core Guidelines: set the pitch shift (in semitones).
   // Core Guidelines: RubberBand 4.0.0 uses setPitchScale() which takes
   // a frequency ratio (2^(semitones/12)) instead of semitones directly.

   if (impl_) {
      impl_->pitchShiftSemitones = semitones;
      if (impl_->stretcher) {
         impl_->stretcher->setPitchScale(
            std::pow(2.0, semitones / 12.0));
      }
   }
}

void RubberbandProcessor::setTempo(float bpm) {
   // Core Guidelines: set the desired tempo (in BPM) for time-stretching.

   if (impl_) {
      impl_->tempoBpm = bpm;
   }
}

std::vector<float> RubberbandProcessor::flush() {
   // Core Guidelines: flush any remaining samples from the stretcher.
   // Core Guidelines: RubberBand 4.0.0 uses process() with final=true,
   // then retrieve() to get remaining output.

   if (impl_ == nullptr || impl_->stretcher == nullptr) {
      return {};
   }

   // Core Guidelines: signal end of input with final=true.
   const float* dummy = nullptr;
   impl_->stretcher->process(&dummy, 0, true);

   // Core Guidelines: retrieve any remaining output.
   size_t numAvailable = impl_->stretcher->available();
   if (numAvailable == 0) {
      return {};
   }

   std::vector<float> output(numAvailable);
   float* outputPtr = output.data();
   impl_->stretcher->retrieve(&outputPtr, numAvailable);

   return output;
}

bool RubberbandProcessor::hasRemaining() const {
   // Core Guidelines: check if there are remaining samples to process.
   // Core Guidelines: RubberBand 4.0.0 uses available() instead of
   // haveActiveStretcher().

   if (impl_ == nullptr || impl_->stretcher == nullptr) {
      return false;
   }

   return impl_->stretcher->available() > 0;
}

#endif // LIBAUDIO_HAS_RUBBERBAND
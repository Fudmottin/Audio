// modelDescriptor.cpp — model contracts + fail-fast I/O validation.

#include "libaudio/modelDescriptor.h"

#include <algorithm>
#include <stdexcept>

namespace libaudio {

namespace {

// Human-readable rendering of a shape for validation error messages.
std::string describeShape(const std::vector<int64_t>& dims) {
   std::string s = "[";
   for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) {
         s += ",";
      }
      s += std::to_string(dims[i]);
   }
   s += "]";
   return s;
}

} // namespace

void ModelDescriptor::validateInput(const Tensor& input) const {
   // basic-pitch and friends take a 3-D tensor: (batch, windowSamples,
   // channels).
   if (input.dims.size() != 3) {
      throw std::invalid_argument(
         id + ": expected a 3-D input " +
         describeShape({1, windowSamples, channels}) + ", got rank " +
         std::to_string(input.dims.size()) + " " + describeShape(input.dims));
   }
   if (input.dims[1] != windowSamples) {
      throw std::invalid_argument(
         id + ": input window length " + std::to_string(input.dims[1]) +
         " != expected " + std::to_string(windowSamples));
   }
   if (input.dims[2] != static_cast<int64_t>(channels)) {
      throw std::invalid_argument(id + ": input channels " +
                                  std::to_string(input.dims[2]) +
                                  " != expected " + std::to_string(channels));
   }
}

void ModelDescriptor::validateOutputs(
   const std::vector<Tensor>& outputs) const {
   if (outputs.size() != outputNames.size()) {
      throw std::invalid_argument(
         id + ": got " + std::to_string(outputs.size()) +
         " outputs, expected " + std::to_string(outputNames.size()));
   }

   // Check each output is a map with a width we recognize (note bins or contour
   // bins). The model emits a leading batch dim — (1, frames, width) — but we
   // also tolerate an already-squeezed (frames, width) form. We validate
   // *widths* (the last dimension) rather than positions, so the check stays
   // correct even if the runtime returns the outputs in a different order.
   size_t noteLike = 0;
   size_t contourLike = 0;
   for (const Tensor& out : outputs) {
      int64_t width = -1;
      if (out.dims.size() == 3 && out.dims[0] == 1) {
         width = out.dims[2]; // (1, frames, width)
      } else if (out.dims.size() == 2) {
         width = out.dims[1]; // (frames, width)
      } else {
         throw std::invalid_argument(id +
                                     ": expected outputs of shape (1, frames, "
                                     "width) or (frames, width), got " +
                                     describeShape(out.dims));
      }
      if (width == nNoteBins) {
         ++noteLike;
      } else if (nContourBins > 0 && width == nContourBins) {
         ++contourLike;
      } else {
         throw std::invalid_argument(id + ": unexpected output width " +
                                     std::to_string(width) + " (expected " +
                                     std::to_string(nNoteBins) + " or " +
                                     std::to_string(nContourBins) + ")");
      }
   }
   if (noteLike + contourLike != outputs.size()) {
      throw std::invalid_argument(
         id + ": output widths do not match the descriptor contract");
   }
}

const ModelDescriptor& basicPitchDescriptor() {
   static const ModelDescriptor descriptor = [] {
      ModelDescriptor d;
      d.id = "basic-pitch";
      d.version = "0.4.0";
      d.opset = 0; // informational; read from the model at runtime if needed

      // Audio input contract (constants.py): mono @ 22050 Hz.
      d.sampleRate = 22050;
      d.channels = 1;
      d.windowSamples = 43844;     // AUDIO_N_SAMPLES = 22050*2 - 256
      d.frontPadSamples = 3840;    // (overlapFrames * FFT_HOP) / 2 = (30*256)/2
      d.hopSamples = 43844 - 7680; // windowSamples - overlapFrames*FFT_HOP
      d.frameRate = 86;            // 22050 // 256

      // Output contract (inference.py): note, onset, contour.
      d.outputNames = {"StatefulPartitionedCall:1",  // note
                       "StatefulPartitionedCall:2",  // onset
                       "StatefulPartitionedCall:0"}; // contour
      d.outputSemantics = OutputSemantics::NOTE_ACTIVATIONS;
      d.nNoteBins = 88;     // MIDI 21..108 (88 piano keys)
      d.nContourBins = 264; // 3 per semitone (fine pitch / pitch-bend)
      d.midiOffset = 21;    // A0
      d.overlapFrames = 30; // DEFAULT_OVERLAPPING_FRAMES

      // Post-processing defaults (inference.py DEFAULT_*).
      d.onsetThreshold = 0.5f;
      d.frameThreshold = 0.3f;
      d.minNoteLenFrames = 11; // ~127.7 ms at 86 fps
      d.velocityScale = 127;
      return d;
   }();
   return descriptor;
}

} // namespace libaudio

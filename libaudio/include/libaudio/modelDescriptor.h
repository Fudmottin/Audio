/**
 * @file modelDescriptor.h
 * @brief The in-code declaration of what a Tier-2 model expects and produces.
 *
 * A `ModelDescriptor` is a self-describing contract for one neural model:
 * its identity, its audio I/O contract, the *meaning* of its outputs, and its
 * tunable post-processing knobs. It is the thing that lets a generic
 * `OnnxSession` (which only knows raw tensors) be driven correctly by a
 * model-specific adapter, and lets the test harness validate a model's I/O
 * fail-fast instead of silently producing wrong numbers.
 *
 * @section model-descriptor-role Role in the architecture
 *
 * The adapter for a model (e.g. `BasicPitch`) is driven by its descriptor:
 * the descriptor tells the adapter the sample rate, the window/hop sizes, how
 * to lay the outputs out, and — via `outputSemantics` — *which* post-processor
 * to select. Swapping basic-pitch for TF-MAGS later is a matter of providing a
 * different descriptor + adapter, not changing the plumbing.
 *
 * @section model-descriptor-tier2 Tier-2 grouping
 *
 * Part of libaudio's Tier-2 layer; gated behind `LIBAUDIO_ENABLE_TIER2`.
 */

#ifndef LIBAUDIO_MODEL_DESCRIPTOR_H
#define LIBAUDIO_MODEL_DESCRIPTOR_H

#include <cstdint>
#include <string>
#include <vector>

#include "onnxTensor.h"

namespace libaudio {

// ============================================================================
// OutputSemantics — what a model's output tensors *mean*.
//
// This is the discriminator that selects the correct post-processor. Different
// families of transcribers emit fundamentally different outputs, and the same
// "threshold a 2-D activation map" code that works for basic-pitch is wrong
// for a source-separator or a Onsets&Frames model. Naming the semantics keeps
// that choice explicit and data-driven.
// ============================================================================
enum class OutputSemantics {
   /// basic-pitch style: per-pitch *note* and *onset* activation maps (plus an
   /// optional fine-pitch *contour* map). Post-processed by `pianoRoll`.
   NOTE_ACTIVATIONS,

   /// (Reserved for later) TF-MAGS / Onsets&Frames style.
   ONSETS_FRAMES,

   /// (Reserved for later) source-separator style: model emits audio stems,
   /// not note maps; handled by the `Separator` port, not a post-processor.
   SEPARATED_STEMS,
};

// ============================================================================
// ModelDescriptor — the in-code contract for one Tier-2 model.
//
// Domain context: a model is a black box with an I/O shape and a *meaning*.
// The descriptor captures both so that (a) an adapter can window and resample
// audio correctly, (b) a post-processor can be selected, and (c) the I/O can
// be validated fail-fast against what the loaded model actually reports.
//
// This is an aggregate value type (copyable). Concrete descriptors are provided
// by the `*Descriptor()` functions below as shared singletons.
// ============================================================================
struct ModelDescriptor {
   // --- Identity ----------------------------------------------------------
   /** Short machine id, e.g. "basic-pitch". */
   std::string id;
   /** Upstream version the descriptor was written against, e.g. "0.4.0". */
   std::string version;
   /** ONNX opset the model was exported with (informational). 0 = unknown. */
   uint32_t opset = 0;

   // --- Audio input contract ---------------------------------------------
   /** Target sample rate; the front-end resamples audio to this. */
   uint32_t sampleRate = 22050;
   /** Number of audio channels the model expects (basic-pitch: 1, mono). */
   uint32_t channels = 1;
   /** Samples per window fed to the model (basic-pitch: 43844 = 2 s at 22050 −
    * 256). */
   int64_t windowSamples = 0;
   /** Sample hop between successive (overlapping) windows. */
   int64_t hopSamples = 0;
   /** Zero samples prepended to the front of the signal before windowing. */
   int64_t frontPadSamples = 0;
   /** Output frames per window / effective global frame rate (basic-pitch: 86).
    */
   int64_t frameRate = 0;

   // --- Output contract ---------------------------------------------------
   /** The model's output tensor names, in the model's declared order. */
   std::vector<std::string> outputNames;
   /** What the outputs mean — selects the post-processor. */
   OutputSemantics outputSemantics = OutputSemantics::NOTE_ACTIVATIONS;
   /** Number of note/pitch bins in the activation map (basic-pitch: 88). */
   int64_t nNoteBins = 0;
   /** Number of fine-pitch contour bins (basic-pitch: 264; 0 = none). */
   int64_t nContourBins = 0;
   /** MIDI number of the lowest pitch bin (basic-pitch: 21 = A0). */
   int64_t midiOffset = 21;
   /** Output frames trimmed per window during overlap-stitching (basic-pitch:
    * 30). */
   int64_t overlapFrames = 0;

   // --- Post-processing knobs (defaults from the upstream tool) -----------
   /** Onset activation threshold (basic-pitch: 0.5). */
   float onsetThreshold = 0.5f;
   /** Frame (sustain) activation threshold (basic-pitch: 0.3). */
   float frameThreshold = 0.3f;
   /** Minimum note length in frames (basic-pitch: 11 ≈ 127.7 ms at 86 fps). */
   int64_t minNoteLenFrames = 11;
   /** Velocity scale: velocity = round(scale * maxAmplitude) (basic-pitch:
    * 127). */
   uint8_t velocityScale = 127;

   // --- Fail-fast validation ---------------------------------------------
   /**
    * Verify an input tensor matches this descriptor's input contract.
    * @throws std::invalid_argument on any mismatch (rank, window, channels).
    */
   void validateInput(const Tensor& input) const;

   /**
    * Verify a set of output tensors matches this descriptor's output contract
    * (names present, correct number of pitch bins).
    *
    * Each output may be a `(1, frames, width)` map — the raw model tensor with
    * its leading batch dimension — or an already-squeezed `(frames, width)`
    * map; the width is read from the *last* dimension and must be a note or
    * contour bin count. (basic-pitch always emits a batch dim of 1.)
    * @throws std::invalid_argument on any mismatch.
    */
   void validateOutputs(const std::vector<Tensor>& outputs) const;
};

// ============================================================================
// Registered descriptors — one per model. These are the single source of truth
// for each model's contract; adapters and the harness read from here.
// ============================================================================

/**
 * The basic-pitch descriptor (Spotify, ICAASP 2022 "nmp" model).
 *
 * Encodes the verified I/O contract of `basic_pitch/saved_models/icassp_2022/
 * nmp.onnx`: mono @ 22050 Hz, 43844-sample (2 s) windows, 30-frame overlap,
 * 86 fps, 88 note-bins (MIDI 21–108), 264 contour-bins. The CQT is computed
 * *inside* the model, so the front-end only resamples and windows.
 */
const ModelDescriptor& basicPitchDescriptor();

} // namespace libaudio

#endif // LIBAUDIO_MODEL_DESCRIPTOR_H

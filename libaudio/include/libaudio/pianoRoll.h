/**
 * @file pianoRoll.h
 * @brief Post-processor for basic-pitch style note/onset activation maps.
 *
 * Ports `basic_pitch/note_creation.py::output_to_notes_polyphonic`: onset
 * thresholding → peak detection → note segmentation (onset walk + melodia
 * trick) → velocity inference → frame-to-time conversion. The output is a
 * list of HIR `Note`s with times in seconds and velocities in 0–127.
 *
 * This is a small, stateful unit: it holds the post-processing knobs from
 * the `ModelDescriptor` and the timing constants needed to convert model
 * frames to seconds. The `BasicPitch` adapter stays thin: it does the
 * front-end (resample + window + overlap-stitch) and then calls
 * `PianoRoll::process` to get notes.
 *
 * Pitch-bends (the contour map) are **not** handled here — they are deferred
 * to a later enhancement.
 *
 * @section piano-roll-tier2 Tier-2 grouping
 *
 * Part of libaudio's Tier-2 layer; gated behind `LIBAUDIO_HAS_TIER2`.
 */

#ifndef LIBAUDIO_PIANOROLL_H
#define LIBAUDIO_PIANOROLL_H

#include <cstdint>
#include <libaudio/hir.h>
#include <libaudio/modelDescriptor.h>
#include <libaudio/onnxTensor.h>
#include <memory>
#include <vector>

namespace libaudio {

// ============================================================================
// PianoRoll — decode raw note/onset activation maps into HIR Notes.
//
// Domain context: the "piano roll" is the time-frequency display of a
// piano performance; this class turns the model's per-pitch activation maps
// into discrete note events. It is the C++ analogue of
// `basic_pitch/note_creation.py::output_to_notes_polyphonic`.
//
// State: holds the post-processing knobs (thresholds, min length, velocity
// scale) and the timing constants (sample rate, window length) from the
// `ModelDescriptor`. No external resources — the Pimpl exists for
// encapsulation only.
//
// Movable, non-copyable (house style for all libaudio classes).
// ============================================================================
class PianoRoll {
 public:
   explicit PianoRoll(const ModelDescriptor& desc);

   ~PianoRoll();

   PianoRoll(const PianoRoll&) = delete;
   PianoRoll& operator=(const PianoRoll&) = delete;

   PianoRoll(PianoRoll&& other) noexcept;
   PianoRoll& operator=(PianoRoll&& other) noexcept;

   /**
    * Process stitched note + onset activation maps into HIR Notes.
    *
    * The input maps are the *stitched* (global) maps produced by the
    * `BasicPitch` adapter's overlap-stitch: each is a 2-D `(n_frames, 88)`
    * tensor (or a 3-D `(1, n_frames, 88)` with a batch dim of 1, which is
    * squeezed).
    *
    * @param frames        The stitched note-activation map.
    * @param onsets        The stitched onset-activation map.
    * @param annotNFrames  Per-window output frame count (172 for basic-pitch).
    *                      Used by the frame-to-time conversion; it is the
    *                      *untrimmed* frame count, not the stitched count.
    * @return Detected notes, with times in seconds and velocities 1–127.
    */
   [[nodiscard]] std::vector<Note> process(const Tensor& frames,
                                           const Tensor& onsets,
                                           int64_t annotNFrames) const;

 private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_PIANOROLL_H

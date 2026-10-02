/**
 * @file pianoRoll.h
 * @brief Post-processor for basic-pitch style note/onset activation maps.
 *
 * Ports `basic_pitch/note_creation.py::output_to_notes_polyphonic`: onset
 * thresholding → peak detection → note segmentation (onset walk + melodia
 * trick) → velocity inference → frame-to-time conversion. The output is a
 * list of HIR `Note`s with times in seconds and velocities in 0–127.
 *
 * This is a small, stateful unit: it holds the *model-fixed* timing constants
 * from the `ModelDescriptor`; the *tunable* post-processing knobs arrive per
 * call via `BasicPitchOptions`. It also converts model frames to seconds. The
 * `BasicPitch` adapter stays thin: it does the front-end (resample + window +
 * overlap-stitch) and then calls `PianoRoll::process` to get notes.
 *
 * When the fine-pitch *contour* map is supplied (the 4-arg `process` overload),
 * the port of `basic_pitch/note_creation.py::get_pitch_bends` runs as well: for
 * each detected note it traces the contour's peak (a Gaussian-weighted argmax
 * over a 51-bin window around the note's pitch) and converts the per-frame
 * deviation to MIDI pitch-bend ticks, stored in `Note.pitchBends`.
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

// The tunable knobs that drive the decode (note-detection thresholds, the
// frequency band, the infer-onsets / melodia gates, the output tempo, and the
// bend policy). Forward-declared here to avoid a header cycle with
// `basicPitch.h` (which includes this one); the full type is pulled in by
// `pianoRoll.cpp`.
struct BasicPitchOptions;

// ============================================================================
// PianoRoll — decode raw note/onset activation maps into HIR Notes.
//
// Domain context: the "piano roll" is the time-frequency display of a
// piano performance; this class turns the model's per-pitch activation maps
// into discrete note events. It is the C++ analogue of
// `basic_pitch/note_creation.py::output_to_notes_polyphonic`.
//
// State: holds only the *model-fixed* timing constants (MIDI offset, sample
// rate, window length) from the `ModelDescriptor`. The *tunable* post-
// processing knobs arrive per call via `BasicPitchOptions` (see `process`),
// keeping the descriptor a pure I/O contract. No external resources — the
// Pimpl exists for encapsulation only.
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
    * @param options       The tunable knobs (thresholds, frequency band, the
    *                      infer-onsets / melodia gates, velocity scale).
    * @return Detected notes, with times in seconds and velocities 1–127 and
    *         **no** pitch bends (`Note.pitchBends` left empty).
    */
   [[nodiscard]] std::vector<Note>
   process(const Tensor& frames, const Tensor& onsets, int64_t annotNFrames,
           const BasicPitchOptions& options) const;

   /**
    * Process stitched note + onset + *contour* activation maps into HIR Notes,
    * **including pitch bends**.
    *
    * This is the 3-arg overload plus the fine-pitch contour map, which the
    * port of `get_pitch_bends` consumes. The contour map is a 2-D
    * `(n_frames, 264)` tensor (3 bins per semitone) on the *same* frame grid as
    * `frames`/`onsets`; a note whose `pitchBends` come out all-zero is left
    * non-empty here (this is the faithful reference port — any deadbanding of
    * near-flat notes is the adapter's job, not the post-processor's).
    *
    * @param frames        The stitched note-activation map.
    * @param onsets        The stitched onset-activation map.
    * @param contour       The stitched fine-pitch contour map `(n_frames,
    * 264)`.
    * @param annotNFrames  Per-window output frame count (172 for basic-pitch).
    * @param options       The tunable knobs (thresholds, frequency band, the
    *                      infer-onsets / melodia gates, velocity scale).
    * @return Detected notes with `pitchBends` filled (MIDI ticks, one per frame
    *         of the note).
    */
   [[nodiscard]] std::vector<Note>
   process(const Tensor& frames, const Tensor& onsets, const Tensor& contour,
           int64_t annotNFrames, const BasicPitchOptions& options) const;

 private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_PIANOROLL_H

/**
 * @file rawMap.h
 * @brief The model's raw activation maps + a small binary file format for them.
 *
 * `RawPredictions` is the result of running the basic-pitch model once over an
 * audio file: the three *stitched, global* activation maps the model produces
 * (note / onset / fine-pitch contour) plus the untrimmed per-window frame count
 * (`annotNFrames`, 172 for basic-pitch) that the frame-to-time conversion
 * needs. It is exactly the input the post-processor (`PianoRoll`) consumes, so
 * it doubles as the *cached* artifact a knob-sweep reads to re-decode notes
 * **without** re-running the model: the maps are the expensive part (the model
 * run), whereas the resampled audio they were built from is cheap and is
 * deliberately *not* stored here — it is regenerated on demand from the source
 * file when a path actually needs it.
 *
 * A `RawPredictions` is a plain value type (three `Tensor`s + two scalars):
 * cheap to move, no hidden state, no ONNX dependency.
 *
 * `writeRawPredictions` / `readRawPredictions` serialize a `RawPredictions` to
 * a small, dependency-free binary file (no JSON — the ONNX-free adapters stay
 * free of extra dependencies). The layout is little-endian and is aimed at fast
 * same-machine experiments, not cross-machine exchange; the magic + version
 * guard make a wrong or drifting file fail fast on read.
 *
 * @section raw-map-tier2 Tier-2 grouping
 *
 * Part of libaudio's Tier-2 layer. This header is included only from the
 * Tier-2-guarded umbrella (`libaudio.h`), so it compiles only when Tier-2 is
 * enabled.
 */

#ifndef LIBAUDIO_RAW_MAP_H
#define LIBAUDIO_RAW_MAP_H

#include <cstdint>
#include <libaudio/onnxTensor.h>
#include <string>

namespace libaudio {

// ============================================================================
// RawPredictions — the three stitched activation maps + the timing constant.
//
// `noteMap` / `onsetMap` are `(n_frames, 88)`; `contourMap` is
// `(n_frames, 264)` when `haveContour` and empty otherwise. `annotNFrames` is
// the *untrimmed* per-window frame count (the model's own output rows, 172)
// that the frame-to-time math divides by — not the stitched frame count.
// ============================================================================
struct RawPredictions {
   /** Stitched note-activation map, `(n_frames, n_note_bins)`. */
   Tensor noteMap;

   /** Stitched onset-activation map, `(n_frames, n_note_bins)`. */
   Tensor onsetMap;

   /** Stitched fine-pitch contour map, `(n_frames, n_contour_bins)`; empty when
    * `haveContour` is false. */
   Tensor contourMap;

   /** Untrimmed per-window output frame count (172 for basic-pitch). */
   int64_t annotNFrames = 0;

   /** True when `contourMap` was accumulated (the model emitted a contour). */
   bool haveContour = false;
};

// ============================================================================
// writeRawPredictions — serialize `pred` to `path`.
//
// The file is a small little-endian block: a magic + version header, the two
// scalars (`annotNFrames`, `haveContour`), then one `(rows, cols, data)`
// record per map. float32 is written byte-for-byte, so a round-trip
// (write → read) reproduces the maps exactly.
//
// @throws std::runtime_error if the file cannot be created or the write fails.
// ============================================================================
void writeRawPredictions(const RawPredictions& pred, const std::string& path);

// ============================================================================
// readRawPredictions — the inverse of `writeRawPredictions`.
//
// @throws std::runtime_error on a missing/unreadable file, a bad magic, a
// version this build does not understand, or a truncated body.
// ============================================================================
[[nodiscard]] RawPredictions readRawPredictions(const std::string& path);

} // namespace libaudio

#endif // LIBAUDIO_RAW_MAP_H

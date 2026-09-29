// pianoRoll.cpp — decode basic-pitch style note/onset activation maps into
// HIR `Note`s.
//
// Derived from Spotify's basic-pitch (https://github.com/spotify/basic-pitch).
// Copyright 2022 Spotify AB. Licensed under the Apache License, Version 2.0
// (code + model weights).
//
// The model (nmp.onnx) was developed by Spotify's Audio Intelligence Lab and
// published at ICASSP 2022:
//   "A Lightweight Instrument-Agnostic Model for Polyphonic Note Transcription
//    and Multipitch Estimation" — Bittner, Bosch, Rubinstein, Meseguer-Brocal,
//    Ewert. Proceedings of the IEEE International Conference on Acoustics,
//    Speech, and Signal Processing (ICASSP), Singapore, 2022.
//
// The model file is shipped by Spotify as basic_pitch/saved_models/icassp_2022/
// nmp.onnx (230,444 bytes, exported by tf2onnx 1.15.1).
//
// This is the C++ port of
// `basic_pitch/note_creation.py::output_to_notes_polyphonic` (plus its
// `get_infered_onsets` helper and `model_frames_to_time`), the post-processing
// step that turns a model's per-pitch *activation maps* into discrete note
// events. Pitch-bends (the `contour` map) are deliberately NOT handled here;
// they are a later enhancement.
//
// @section piano-roll-algorithm The algorithm (faithful to the reference)
//
// The decoder receives the *stitched, global* note + onset maps
// (each `(n_frames, 88)`) and the untrimmed per-window frame count
// (`annotNFrames`, 172 for basic-pitch) needed to convert frames to seconds:
//
//   1. **Infer onsets** from large changes in the note (frame) activations —
//      `get_infered_onsets`. This adds onsets the model missed: a sharp rise in
//      a note's energy is treated as an attack.
//   2. **Detect onset peaks**: a frame/pitch cell is a relative maximum
//   (strictly
//      greater than its temporal neighbours) with activation above the onset
//      threshold. Walk them in reverse time order.
//   3. **Segment notes from each onset**: extend forward through frames whose
//      *remaining* energy stays above the frame threshold (up to a small
//      tolerance gap). Zero that run (and its adjacent pitches) so it is not
//      re-detected. The note's amplitude is the *mean* of the note column.
//   4. **Melodia trick**: while any remaining energy exceeds the frame
//      threshold, take the global maximum as a new note centre and grow it
//      forward and backward the same way. This recovers notes that have no
//      onset peak (e.g. a sustained note still ringing when the window ends).
//   5. **Frame → time** and **velocity**: convert the (start, end) frame
//      indices to seconds and map the mean amplitude to a MIDI velocity.
//
// The reference operates on NumPy arrays; here `Map2D` is a small row-major
// 2-D float matrix (the shape a stitched map has after squeezing the batch
// dimension) with just the helpers the algorithm needs. The `Impl` holds the
// tunable knobs + timing constants and owns the decode logic, so the public
// `PianoRoll` stays a thin, movable, Pimpl unit.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <libaudio/pianoRoll.h>
#include <utility>
#include <vector>

namespace libaudio {

namespace {

// ============================================================================
// Map2D — a row-major 2-D float matrix with the operations the decoder needs.
//
// This is a file-local detail, not part of the public API. It is the C++
// stand-in for a NumPy 2-D array and is exactly the shape of a stitched
// activation map (n_frames rows, 88 pitch columns). Row-major layout means a
// flat index is `row * cols + col`.
// ============================================================================
struct Map2D {
   int64_t rows = 0;
   int64_t cols = 0;
   std::vector<float> data; // size == rows * cols, row-major

   [[nodiscard]] float at(int64_t r, int64_t c) const {
      return data[static_cast<size_t>(r) * static_cast<size_t>(cols) +
                  static_cast<size_t>(c)];
   }

   void set(int64_t r, int64_t c, float v) {
      data[static_cast<size_t>(r) * static_cast<size_t>(cols) +
           static_cast<size_t>(c)] = v;
   }

   // The global maximum and the (row, col) of its *first* occurrence in
   // row-major order — the exact semantics of `np.argmax` / `np.max` over the
   // whole matrix. For an empty map, `row` is -1 and `value` is 0.
   struct Argmax {
      float value = 0.0f;
      int64_t row = -1;
      int64_t col = -1;
   };
   [[nodiscard]] Argmax maxWithIndex() const {
      Argmax best;
      const int64_t total =
         static_cast<int64_t>(rows) * static_cast<int64_t>(cols);
      for (int64_t i = 0; i < total; ++i) {
         // Strict '>' keeps the *first* cell attaining the running maximum,
         // matching numpy's argmax (which is first-occurrence on ties).
         if (best.row < 0 || data[static_cast<size_t>(i)] > best.value) {
            best.value = data[static_cast<size_t>(i)];
            best.row = i / cols;
            best.col = i % cols;
         }
      }
      return best;
   }

   [[nodiscard]] float globalMax() const { return maxWithIndex().value; }

   // Mean of column `c` over the half-open row range [r0, r1) — `np.mean(
   // matrix[r0:r1, c])`. Returns 0 for an empty range.
   [[nodiscard]] float columnMean(int64_t r0, int64_t r1, int64_t c) const {
      if (r1 <= r0) {
         return 0.0f;
      }
      double sum = 0.0;
      for (int64_t r = r0; r < r1; ++r) {
         sum += static_cast<double>(at(r, c));
      }
      return static_cast<float>(sum / static_cast<double>(r1 - r0));
   }

   void zeroColumnRange(int64_t r0, int64_t r1, int64_t c) {
      for (int64_t r = r0; r < r1; ++r) {
         set(r, c, 0.0f);
      }
   }
};

// Build a Map2D from a Tensor, squeezing a leading batch-1 dimension. A
// stitched map is usually already 2-D; a raw per-window output is 3-D
// (1, frames, width). Either is handled; anything else yields an empty map.
Map2D mapFromTensor(const Tensor& t) {
   Map2D m;
   if (t.dims.size() == 3 && t.dims[0] == 1) {
      m.rows = t.dims[1];
      m.cols = t.dims[2];
      m.data = t.data; // batch 1: the row-major layout is unchanged
   } else if (t.dims.size() == 2) {
      m.rows = t.dims[0];
      m.cols = t.dims[1];
      m.data = t.data;
   }
   return m;
}

// A note as detected before time/velocity conversion: (startFrame, endFrame,
// pitchBin, amplitude). `endFrame` is the index of the first frame *after* the
// note; the note's end *time* is `times[endFrame]` (matching the reference,
// whose amplitude mean covers the half-open [start, end) rows).
struct RawNote {
   int64_t startFrame = 0;
   int64_t endFrame = 0;
   int64_t pitchBin = 0;
   float amplitude = 0.0f;
};

} // namespace

// ============================================================================
// PianoRoll::Impl — the decode logic + the knobs/timing it depends on.
//
// The tunable thresholds and lengths come from the ModelDescriptor (the single
// source of truth for the model's contract). A few constants are specific to
// the reference's timing math and are not in the descriptor, so they live here
// as named constants (mirroring basic_pitch/constants.py and note_creation.py).
// ============================================================================
struct PianoRoll::Impl {
   // --- Post-processing knobs (from the ModelDescriptor) -------------------
   float onsetThreshold = 0.5f;   // min onset activation to be considered
   float frameThreshold = 0.3f;   // min frame activation to keep a note alive
   int64_t minNoteLenFrames = 11; // discard notes shorter than this (frames)
   int64_t midiOffset = 21;       // MIDI number of pitch bin 0 (A0)
   uint8_t velocityScale = 127;   // velocity = clamp(round(scale * amp))

   // --- Sample-rate (from the descriptor); the 22050 model rate -------------
   uint32_t sampleRate = 22050;

   // --- Reference timing constants (not in the descriptor) ------------------
   // The model's internal FFT hop in samples (constants.py FFT_HOP = 256).
   // frameHopSec is derived from it. It is 256/22050, NOT 1/frameRate, so the
   // time grid is sample-accurate rather than an integer-fps approximation.
   static constexpr int64_t fftHop = 256;
   // A constant added to the per-window time offset so detected onsets line up
   // with the true attacks (constants.py MAGIC_ALIGNMENT_OFFSET).
   static constexpr double magicAlignmentOffset = 0.0018;
   // How many consecutive sub-threshold frames the note-walk tolerates before
   // it stops (note_creation.py ENERGY_TOLERANCE = 11). This is a *walk
   // tolerance*, distinct from minNoteLenFrames (a *minimum length*): they
   // happen to be equal (11) but mean different things, so they are separate.
   static constexpr int64_t energyTolerance = 11;

   explicit Impl(const ModelDescriptor& d)
      : onsetThreshold(d.onsetThreshold)
      , frameThreshold(d.frameThreshold)
      , minNoteLenFrames(d.minNoteLenFrames)
      , midiOffset(d.midiOffset)
      , velocityScale(d.velocityScale)
      , sampleRate(d.sampleRate)
      , windowSamples(d.windowSamples) {}

   // Port of `output_to_notes_polyphonic` (+ `get_infered_onsets`). `frames`
   // is the stitched note-activation map (kept intact for amplitude + the onset
   // difference); `onsets` is the stitched onset map, *modified* by the
   // inferred-onset step. Returns notes with times in seconds and velocities
   // clamped to [1, 127].
   [[nodiscard]] std::vector<Note> decode(const Map2D& frames, Map2D onsets,
                                          int64_t annotNFrames) const {
      std::vector<Note> notes;
      if (frames.rows <= 0 || frames.cols <= 0) {
         return notes; // nothing to decode
      }

      const int64_t nFrames = frames.rows;
      const int64_t nFreqs = frames.cols;
      const int64_t maxFreqIdx = nFreqs - 1; // highest valid pitch bin

      // The reference's constrain_frequency() zeroes activations outside a
      // [min_freq, max_freq] band, but both are None (no band requested), so
      // the whole 88-bin range is kept and the step is a no-op. We skip it
      // rather than zero nothing.

      // --- 1. Infer onsets from sharp rises in the note (frame) activations.
      getInferredOnsets(onsets, frames, /*nDiff=*/2);

      // --- 2. Detect onset peaks and walk them in reverse time order. ---
      // A cell is a peak when it is strictly greater than its temporal
      // neighbours (boundaries count as peaks — this mirrors the reference's
      // relative-maximum test) and exceeds the onset threshold. Collected in
      // row-major (time-ascending) order, then reversed, exactly as the
      // reference reverses its (row, col) coordinate list.
      std::vector<std::pair<int64_t, int64_t>> peaks;
      for (int64_t t = 0; t < nFrames; ++t) {
         for (int64_t f = 0; f < nFreqs; ++f) {
            const float v = onsets.at(t, f);
            const bool higherThanPrev = (t == 0) || (v > onsets.at(t - 1, f));
            const bool higherThanNext =
               (t == nFrames - 1) || (v > onsets.at(t + 1, f));
            if (higherThanPrev && higherThanNext && v >= onsetThreshold) {
               peaks.emplace_back(t, f);
            }
         }
      }
      std::reverse(peaks.begin(), peaks.end());

      // Remaining energy: a scratch copy of the note map that the segmentation
      // drains, so an already-claimed region is not re-detected.
      Map2D remaining;
      remaining.rows = nFrames;
      remaining.cols = nFreqs;
      remaining.data = frames.data;

      std::vector<RawNote> raw;

      // --- 3. Segment a note from each onset peak. ---
      for (const auto& [s, f] : peaks) {
         // Too close to the very end to grow forward — skip.
         if (s >= nFrames - 1) {
            continue;
         }
         int64_t i = s + 1;
         int64_t k = 0; // consecutive sub-threshold frames since the last hit
         while (i < nFrames - 1 && k < energyTolerance) {
            if (remaining.at(i, f) < frameThreshold) {
               ++k;
            } else {
               k = 0;
            }
            ++i;
         }
         i -= k; // step back to the last frame at/above threshold
         // Too short — discard (the onset run collapsed into the gap).
         if (i - s <= minNoteLenFrames) {
            continue;
         }
         // Consume this run and its adjacent pitches so it is not revisited.
         remaining.zeroColumnRange(s, i, f);
         if (f < maxFreqIdx) {
            remaining.zeroColumnRange(s, i, f + 1);
         }
         if (f > 0) {
            remaining.zeroColumnRange(s, i, f - 1);
         }
         RawNote rn;
         rn.startFrame = s;
         rn.endFrame = i;
         rn.pitchBin = f;
         rn.amplitude = frames.columnMean(s, i, f);
         raw.push_back(rn);
      }

      // --- 4. Melodia trick: claim notes that have no onset peak. ---
      // While any remaining energy exceeds the frame threshold, take the global
      // maximum as a note centre and grow it in both time directions. Each pass
      // zeroes at least the centre cell, so the count of above-threshold cells
      // strictly decreases and the loop terminates.
      while (true) {
         const Map2D::Argmax mw = remaining.maxWithIndex();
         if (mw.row < 0 || mw.value <= frameThreshold) {
            break;
         }
         const int64_t iMid = mw.row;
         const int64_t f = mw.col;
         remaining.set(iMid, f, 0.0f);

         // Forward pass (toward the end of the audio).
         int64_t i = iMid + 1;
         int64_t k = 0;
         while (i < nFrames - 1 && k < energyTolerance) {
            if (remaining.at(i, f) < frameThreshold) {
               ++k;
            } else {
               k = 0;
            }
            remaining.set(i, f, 0.0f);
            if (f < maxFreqIdx) {
               remaining.set(i, f + 1, 0.0f);
            }
            if (f > 0) {
               remaining.set(i, f - 1, 0.0f);
            }
            ++i;
         }
         const int64_t iEnd = i - 1 - k;

         // Backward pass (toward the start of the audio).
         i = iMid - 1;
         k = 0;
         while (i > 0 && k < energyTolerance) {
            if (remaining.at(i, f) < frameThreshold) {
               ++k;
            } else {
               k = 0;
            }
            remaining.set(i, f, 0.0f);
            if (f < maxFreqIdx) {
               remaining.set(i, f + 1, 0.0f);
            }
            if (f > 0) {
               remaining.set(i, f - 1, 0.0f);
            }
            --i;
         }
         const int64_t iStart = i + 1 + k;

         // Too short — discard (but the region is already drained).
         if (iEnd - iStart <= minNoteLenFrames) {
            continue;
         }
         RawNote rn;
         rn.startFrame = iStart;
         rn.endFrame = iEnd;
         rn.pitchBin = f;
         rn.amplitude = frames.columnMean(iStart, iEnd, f);
         raw.push_back(rn);
      }

      // --- 5. Frames → time, amplitude → velocity; build the HIR Notes. ---
      const std::vector<double> times = frameTimes(nFrames, annotNFrames);
      notes.reserve(raw.size());
      for (const RawNote& rn : raw) {
         if (rn.startFrame < 0 || rn.endFrame > nFrames) {
            continue; // defensive: indices outside the map
         }
         Note n;
         n.startTime = times[static_cast<size_t>(rn.startFrame)];
         n.endTime = times[static_cast<size_t>(
            rn.endFrame >= nFrames ? nFrames - 1 : rn.endFrame)];
         n.pitch = static_cast<uint8_t>(rn.pitchBin + midiOffset);
         // velocity = clamp(round(velocityScale * amplitude), 1, 127).
         int vel =
            static_cast<int>(std::llround(static_cast<double>(velocityScale) *
                                          static_cast<double>(rn.amplitude)));
         if (vel < 1) {
            vel = 1;
         }
         if (vel > 127) {
            vel = 127;
         }
         n.velocity = static_cast<uint8_t>(vel);
         n.channel = 0;
         n.sustain = false;
         notes.push_back(n);
      }
      return notes;
   }

 private:
   // Port of `get_infered_onsets`: add onsets the model missed by detecting
   // sharp rises in the note activations, then take the elementwise maximum
   // with the predicted onsets. `n_diff` is the number of back-differences
   // considered (the reference uses 2). `onsets` is written in place.
   void getInferredOnsets(Map2D& onsets, const Map2D& frames, int nDiff) const {
      const int64_t nFrames = frames.rows;
      const int64_t nFreqs = frames.cols;

      // frame_diff[r, c] = min over n in [1, nDiff] of
      //    frames[r, c] - (r < n ? 0 : frames[r - n, c])
      // i.e. the smallest (most positive) of the successive differences.
      Map2D diff;
      diff.rows = nFrames;
      diff.cols = nFreqs;
      diff.data.assign(static_cast<size_t>(nFrames) *
                          static_cast<size_t>(nFreqs),
                       0.0f);
      for (int n = 1; n <= nDiff; ++n) {
         for (int64_t r = 0; r < nFrames; ++r) {
            for (int64_t c = 0; c < nFreqs; ++c) {
               const float base = frames.at(r, c);
               const float ref = (r < n) ? 0.0f : frames.at(r - n, c);
               const float d = base - ref;
               if (n == 1) {
                  diff.set(r, c, d);
               } else {
                  diff.set(r, c, std::min(diff.at(r, c), d));
               }
            }
         }
      }
      // Clip negative differences to zero and zero the first nDiff rows (the
      // reference sets frame_diff[:n_diff] = 0 to avoid edge artefacts).
      for (int64_t r = 0; r < nFrames; ++r) {
         for (int64_t c = 0; c < nFreqs; ++c) {
            float v = diff.at(r, c);
            if (v < 0.0f) {
               v = 0.0f;
            }
            if (r < nDiff) {
               v = 0.0f;
            }
            diff.set(r, c, v);
         }
      }
      // Rescale the differences to share the onsets' peak magnitude, but only
      // when there is a non-zero maximum to divide by (avoid 0/0 = NaN when
      // the frames are flat). This mirrors the reference's intent.
      const float maxOnsets = onsets.globalMax();
      const float maxFrameDiff = diff.globalMax();
      if (maxFrameDiff > 0.0f) {
         for (float& v : diff.data) {
            v = maxOnsets * v / maxFrameDiff;
         }
      }
      // onsets = max(onsets, diff), elementwise.
      for (int64_t r = 0; r < nFrames; ++r) {
         for (int64_t c = 0; c < nFreqs; ++c) {
            onsets.set(r, c, std::max(onsets.at(r, c), diff.at(r, c)));
         }
      }
   }

   // Port of `model_frames_to_time`: convert global frame indices to seconds.
   //
   // The reference computes `times[j] = j * (FFT_HOP / SR) - windowOffset *
   // floor(j / ANNOT_N_FRAMES)`, where `windowOffset = (FFT_HOP / SR) *
   // (ANNOT_N_FRAMES - AUDIO_N_SAMPLES / FFT_HOP) + MAGIC_ALIGNMENT_OFFSET`.
   //
   // NOTE: `ANNOT_N_FRAMES` (172) is the *untrimmed* per-window frame count,
   // but the stitched map carries only 142 frames per window (172 - 30
   // overlap). The reference divides by 172 regardless — we replicate that
   // quirk (do not "fix" it): it is what makes the model's own output align
   // with real onset times.
   [[nodiscard]] std::vector<double> frameTimes(int64_t nFrames,
                                                int64_t annotNFrames) const {
      std::vector<double> times(
         static_cast<size_t>(std::max<int64_t>(nFrames, 0)));
      if (annotNFrames <= 0) {
         return times;
      }
      const double frameHopSec =
         static_cast<double>(fftHop) / static_cast<double>(sampleRate);
      const double audioNSamples = static_cast<double>(windowSamples); // 43844
      const double windowOffset =
         frameHopSec * (static_cast<double>(annotNFrames) -
                        audioNSamples / static_cast<double>(fftHop)) +
         magicAlignmentOffset;
      for (int64_t j = 0; j < nFrames; ++j) {
         const double original = static_cast<double>(j) * frameHopSec;
         // floor(j / annotNFrames) for non-negative j is plain integer
         // division.
         const int64_t windowNumber = j / annotNFrames;
         times[static_cast<size_t>(j)] =
            original - windowOffset * static_cast<double>(windowNumber);
      }
      return times;
   }

   // The model's window length in samples (constants.py AUDIO_N_SAMPLES);
   // set from the descriptor and used by the frame→time math.
   int64_t windowSamples = 43844;
};

// --- PianoRoll: public API -------------------------------------------------

PianoRoll::PianoRoll(const ModelDescriptor& desc)
   : impl_(std::make_unique<Impl>(desc)) {}

PianoRoll::~PianoRoll() = default;

PianoRoll::PianoRoll(PianoRoll&& other) noexcept = default;
PianoRoll& PianoRoll::operator=(PianoRoll&& other) noexcept = default;

std::vector<Note> PianoRoll::process(const Tensor& frames, const Tensor& onsets,
                                     int64_t annotNFrames) const {
   Map2D f = mapFromTensor(frames);
   Map2D o = mapFromTensor(onsets);
   return impl_->decode(f, o, annotNFrames);
}

} // namespace libaudio

// basicPitch.cpp — the basic-pitch transcriber adapter.
//
// This is the thin C++ front-end for the basic-pitch model. The CQT lives
// *inside* the model, so all the front-end does is: resample the audio to the
// model's 22050 Hz mono rate, pad and cut it into the model's overlapping
// windows, run each window, and stitch the per-window note/onset maps back into
// one global map. The `PianoRoll` then decodes that map into HIR notes.
//
// The windowing and stitching constants come from `basicPitchDescriptor()` and
// the math mirrors the reference `inference.py`:
//   - `get_audio_input`:  resample → prepend `frontPadSamples` zeros → hop by
//     `hopSamples`, emitting `windowSamples`-sample windows (the last one
//     zero-padded short).
//   - `unwrap_output`:    trim `overlapFrames/2` frames from each window's
//     start and end, concatenate, then truncate to the expected global frame
//     count `int(origLen / hopSamples * (annotNFrames - overlapFrames))`.

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <libaudio/audioDecode.h>
#include <libaudio/audioFile.h>
#include <libaudio/basicPitch.h>
#include <libaudio/scoreBuilder.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nmp_onnx_data.h"

namespace libaudio {

// ============================================================================
// Impl — owns the loaded model session + the post-processor + decode settings.
//
// The `ModelDescriptor` is held as a pointer to the shared singleton (it is a
// static with program lifetime, so the pointer is always valid); storing a
// pointer avoids copying its name strings on every move. `session` is
// `mutable` because `OnnxSession::run` is non-const while a `BasicPitch` is
// logically const (one session transcribes many files).
// ============================================================================
struct BasicPitch::Impl {
   // Resolve all numeric constants from the descriptor once.
   const ModelDescriptor* desc = &basicPitchDescriptor();
   mutable OnnxSession
      session; // mutable: run() is non-const but logically const
   PianoRoll roll;
   std::string ffmpegPath;

   int64_t sampleRate = 0;    // 22050
   int64_t windowSamples = 0; // 43844
   int64_t frontPad = 0;      // 3840
   int64_t hop = 0;           // 36164
   int64_t overlapFrames = 0; // 30
   int64_t nNoteBins = 0;     // 88

   explicit Impl(const ModelDescriptor& d)
      : desc(&d)
      , roll(d)
      , sampleRate(d.sampleRate)
      , windowSamples(d.windowSamples)
      , frontPad(d.frontPadSamples)
      , hop(d.hopSamples)
      , overlapFrames(d.overlapFrames)
      , nNoteBins(d.nNoteBins) {}
};

// ============================================================================
// accumulate — trim one window's activation map and append the surviving frames
// to a growing flat accumulator.
//
// `out` is a single window's output, either `(1, frames, width)` (the raw model
// tensor with its batch dim) or an already-squeezed `(frames, width)`; the row
// count and column count are read from the last two dims. The reference
// `unwrap_output` keeps rows `[overlapFrames/2, frames - overlapFrames/2)` and
// appends them; that is what this does (appending the kept rows' worth of the
// flat, row-major buffer).
// ============================================================================
static void accumulateWindow(const Tensor& out, int64_t overlapFrames,
                             std::vector<float>& acc) {
   int64_t rows = 0;
   int64_t cols = 0;
   if (out.dims.size() == 3 && out.dims[0] == 1) {
      rows = out.dims[1];
      cols = out.dims[2];
   } else if (out.dims.size() == 2) {
      rows = out.dims[0];
      cols = out.dims[1];
   } else {
      return; // Unexpected rank; validateOutputs() would already have thrown.
   }

   const int64_t nOlap = overlapFrames / 2;
   if (rows <= 2 * nOlap) {
      return; // Not enough frames to overlap-trim; nothing survives.
   }
   const int64_t lo = nOlap;
   const int64_t hi = rows - nOlap; // e.g. rows=172 → [15,157) = 142 frames
   acc.reserve(acc.size() +
               static_cast<size_t>(hi - lo) * static_cast<size_t>(cols));
   for (int64_t r = lo; r < hi; ++r) {
      const float* rowSrc =
         out.data.data() + (static_cast<size_t>(r) * static_cast<size_t>(cols));
      acc.insert(acc.end(), rowSrc, rowSrc + static_cast<size_t>(cols));
   }
}

// ============================================================================
// readAllMono — drain a reader's frames into one vector of floats.
//
// `AudioFileReader` normalizes to [-1, 1] float regardless of the on-disk
// format and downmixes to mono, so the result is exactly the mono signal the
// model wants. A 4096-frame block keeps the per-call buffer bounded.
// ============================================================================
static std::vector<float> readAllMono(AudioFileReader& reader) {
   std::vector<float> mono;
   if (reader.totalFrames() > 0) {
      mono.reserve(reader.totalFrames());
      std::vector<float> block(4096);
      uint32_t got = 0;
      while ((got = reader.readMono(block.data(),
                                    static_cast<uint32_t>(block.size()))) > 0) {
         mono.insert(mono.end(), block.begin(),
                     block.begin() + static_cast<std::ptrdiff_t>(got));
      }
   }
   return mono;
}

// ============================================================================
// BasicPitch — public API implementation.
// ============================================================================

BasicPitch::BasicPitch(const std::string& ffmpegPath)
   : impl_(std::make_unique<Impl>(basicPitchDescriptor())) {
   impl_->ffmpegPath = ffmpegPath;
   // Load the model from the embedded blob (weights + CQT are compiled into the
   // binary — nothing to resolve on disk) with the Core ML EP requested; a CPU
   // fallback is not fatal (coreMlActive() reports the actual path).
   impl_->session.loadFromMemory(nmp_onnx, nmp_onnx_len, /*useCoreMl=*/true);
}

BasicPitch::~BasicPitch() = default;

BasicPitch::BasicPitch(BasicPitch&& other) noexcept
   : impl_(std::move(other.impl_)) {}

BasicPitch& BasicPitch::operator=(BasicPitch&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
   }
   return *this;
}

std::string BasicPitch::name() const { return "basic-pitch"; }

bool BasicPitch::coreMlActive() const {
   return impl_ ? impl_->session.coreMlActive() : false;
}

Score BasicPitch::transcribe(std::string_view path) const {
   const ModelDescriptor& desc = *impl_->desc;
   const uint32_t modelRate = static_cast<uint32_t>(desc.sampleRate);

   // --- 1. Resolve a path the reader can open, and get mono at the model rate.
   AudioSource src = AudioSource::open(path, impl_->ffmpegPath);

   // Probe the source rate (the probe leaves the reader at frame 0, so it is
   // still readable afterwards). When the file is not at the model's 22050 Hz
   // rate, resample by having ffmpeg decode the source straight into a
   // 22050 Hz mono float32 WAV and reading that, *rather than* the in-process
   // aubio resampler: the installed aubio is not built with libsamplerate, so
   // `TemporalProcessor::resample` would silently return silence for a 48 kHz
   // file. ffmpeg is already required by this front-end, and its decoder does a
   // proper anti-aliased multirate resample.
   AudioFileReader probe(src.path());
   const uint32_t fileRate = probe.sampleRate();

   std::vector<float> mono;
   if (fileRate != 0 && fileRate != modelRate) {
      AudioSource resampled =
         AudioSource::decodeToRate(src.path(), modelRate, impl_->ffmpegPath);
      AudioFileReader reader(
         resampled.path()); // `resampled` lives for this read
      mono = readAllMono(reader);
   } else {
      // At the model's rate already (or the rate is unknown — read as-is).
      mono = readAllMono(probe);
   }

   // `origLen` is the (resampled) length *before* the front pad — it drives the
   // final stitching truncation, exactly as the reference's `original_length`.
   const int64_t origLen = static_cast<int64_t>(mono.size());

   // --- 3. Front pad, then cut into overlapping windows and run the model. --
   std::vector<float> padded(static_cast<size_t>(desc.frontPadSamples) +
                                mono.size(),
                             0.0f);
   std::copy(mono.begin(), mono.end(),
             padded.begin() +
                static_cast<std::ptrdiff_t>(desc.frontPadSamples));
   const int64_t paddedLen = static_cast<int64_t>(padded.size());

   // Number of windows: the reference iterates `range(0, len, hop)`, i.e.
   // ceil(len / hop). A positive hop is guaranteed by the descriptor.
   const int64_t nWin = (paddedLen + impl_->hop - 1) / impl_->hop;

   // Locate the note/onset outputs by *name* (never position): the runtime may
   // return the model's outputs in any order. `desc.outputNames` is the
   // semantic order {note, onset, contour}.
   const auto outNames = impl_->session.outputNames();
   int64_t noteIdx = -1;
   int64_t onsetIdx = -1;
   for (size_t i = 0; i < outNames.size(); ++i) {
      if (desc.outputNames.size() > 0 && outNames[i] == desc.outputNames[0]) {
         noteIdx = static_cast<int64_t>(i);
      } else if (desc.outputNames.size() > 1 &&
                 outNames[i] == desc.outputNames[1]) {
         onsetIdx = static_cast<int64_t>(i);
      }
   }
   if (noteIdx < 0 || onsetIdx < 0) {
      throw std::runtime_error(
         "basic-pitch: could not locate the note/onset outputs by name");
   }

   std::vector<float> accNote;
   std::vector<float> accOnset;
   int64_t annotNFrames = 0;
   bool haveAnnot = false;
   bool validatedInput = false;
   bool validatedOutputs = false;

   for (int64_t w = 0; w < nWin; ++w) {
      const int64_t start = w * impl_->hop;

      // One zero-filled window; fill the in-bounds samples, zero-pad the tail.
      Tensor input({1, impl_->windowSamples, 1},
                   static_cast<size_t>(impl_->windowSamples));
      for (int64_t k = 0; k < impl_->windowSamples; ++k) {
         const int64_t idx = start + k;
         if (idx >= 0 && idx < paddedLen) {
            input.data[static_cast<size_t>(k)] =
               padded[static_cast<size_t>(idx)];
         }
      }
      if (!validatedInput) {
         desc.validateInput(input);
         validatedInput = true;
      }

      std::vector<Tensor> outputs = impl_->session.run(input);
      if (!validatedOutputs) {
         desc.validateOutputs(outputs);
         validatedOutputs = true;
      }
      if (!haveAnnot) {
         const Tensor& noteOut = outputs[static_cast<size_t>(noteIdx)];
         annotNFrames =
            (noteOut.dims.size() == 3) ? noteOut.dims[1] : noteOut.dims[0];
         haveAnnot = true;
      }
      accumulateWindow(outputs[static_cast<size_t>(noteIdx)],
                       impl_->overlapFrames, accNote);
      accumulateWindow(outputs[static_cast<size_t>(onsetIdx)],
                       impl_->overlapFrames, accOnset);
   }

   // --- 4. Truncate to the expected global length and build the maps. -------
   // The reference trims to int(origLen / hop * (annotNFrames -
   // overlapFrames)).
   const int64_t accumRows =
      static_cast<int64_t>(accNote.size()) / impl_->nNoteBins;
   const int64_t framesPerWindow = annotNFrames - impl_->overlapFrames;
   const int64_t targetFrames = static_cast<int64_t>(
      static_cast<double>(origLen) / static_cast<double>(impl_->hop) *
      static_cast<double>(framesPerWindow));
   int64_t totalFrames = std::min(accumRows, targetFrames);
   if (totalFrames < 0) {
      totalFrames = 0;
   }

   const size_t mapCount =
      static_cast<size_t>(totalFrames) * static_cast<size_t>(impl_->nNoteBins);
   Tensor noteMap({totalFrames, impl_->nNoteBins}, mapCount);
   std::copy(accNote.begin(),
             accNote.begin() + static_cast<ptrdiff_t>(mapCount),
             noteMap.data.begin());
   Tensor onsetMap({totalFrames, impl_->nNoteBins}, mapCount);
   std::copy(accOnset.begin(),
             accOnset.begin() + static_cast<ptrdiff_t>(mapCount),
             onsetMap.data.begin());

   // --- 5. Decode the global maps into notes and assemble the Score. --------
   std::vector<Note> notes =
      impl_->roll.process(noteMap, onsetMap, annotNFrames);

   ScoreBuilder builder;
   for (Note& n : notes) {
      builder.addNote(std::move(n));
   }
   builder.setTempo(120.0);
   builder.setTitle(std::filesystem::path(std::string(path)).stem().string());
   return builder.build();
}

} // namespace libaudio

// tier2_smoke.cpp — end-to-end proof of the C++ / ONNX Runtime / Core ML path.
//
// Exercises the real Spotify basic-pitch model
// (https://github.com/spotify/basic-pitch). Copyright 2022 Spotify AB.
// Licensed under the Apache License, Version 2.0 (code + model weights).
//
// The model (nmp.onnx) was developed by Spotify's Audio Intelligence Lab and
// published at ICASSP 2022:
//   "A Lightweight Instrument-Agnostic Model for Polyphonic Note Transcription
//    and Multipitch Estimation" — Bittner, Bosch, Rubinstein, Meseguer-Brocal,
//    Ewert. Proceedings of the IEEE International Conference on Acoustics,
//    Speech, and Signal Processing (ICASSP), Singapore, 2022.
//
// The model file is shipped by Spotify as basic_pitch/saved_models/icassp_2022/
// nmp.onnx (230,444 bytes, exported by tf2onnx 1.15.1). Its SHA-256 is
// verified at build time against manifests/basic-pitch.txt.
//
// This is a deliberate, isolated smoke test for the Tier-2 foundation. It does
// three things, in increasing strength:
//
//   1. Loads the real basic-pitch model (nmp.onnx, referenced in place from the
//      external/basic-pitch submodule) through libaudio::OnnxSession.
//   2. Feeds it a *known* synthetic signal (a sustained 440 Hz / A4 sine) and
//      confirms the model responds sensibly: the per-pitch "note" activation
//      map lights up strongly near A4. This proves the model actually computes
//      (CQT-in-model → activations), not that it returns zeros.
//   3. Validates the returned tensors against the ModelDescriptor (fail-fast
//   I/O
//      contract) and confirms the output shapes are the expected note / onset /
//      contour maps.
//
// It intentionally does NOT assert exact transcription accuracy — that is what
// the full analyzer-agnostic harness (the render_test_suite port) is for, in a
// later increment. Here the bar is: the path loads, runs, and behaves.
//
// Exit code: 0 on success, non-zero on failure (so CTest tracks it).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <libaudio/libaudio.h>
#include <libaudio/rawMap.h>
#include <string>
#include <vector>

#if !defined(LIBAUDIO_HAS_TIER2)

int main() {
   std::printf(
      "tier2_smoke: Tier-2 is not enabled in this build; nothing to do.\n");
   return 0;
}

#else // LIBAUDIO_HAS_TIER2

#include "nmp_onnx_data.h"

namespace {

int g_failures = 0;

// Report a check; counts failures instead of aborting, so we see the full
// picture.
bool check(bool condition, const std::string& what) {
   std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", what.c_str());
   if (!condition) {
      ++g_failures;
   }
   return condition;
}

std::string shapeStr(const std::vector<int64_t>& dims) {
   std::string s = "(";
   for (size_t i = 0; i < dims.size(); ++i) {
      if (i > 0) {
         s += ", ";
      }
      s += std::to_string(dims[i]);
   }
   s += ")";
   return s;
}

// Maximum value over the whole tensor.
float maxAbs(const libaudio::Tensor& t) {
   float m = 0.0f;
   for (float v : t.data) {
      m = std::max(m, std::fabs(v));
   }
   return m;
}

// --- Pure post-processing test: the get_pitch_bends port, no model --------
//
// Exercises `PianoRoll::process(frames, onsets, contour, annotNFrames)` on a
// synthetic activation/contour grid and checks the bend math directly, without
// running the model. This is the model-free core of the pitch-bend feature:
//   * a note whose contour peak sits exactly on its base pitch reads 0 ticks;
//   * a note whose contour peak is offset by k contour bins reads a known
//     non-zero 14-bit tick count (k bins is k/3 semitones = round(k*4096/3));
//   * the 3-arg overload (no contour) yields a note with no bends at all.
//
// One note is engineered deterministically: a constant 0.8 activation in a
// single pitch column with a single onset peak, well above the descriptor's
// frame/onset thresholds and longer than the minimum note length, so exactly
// one note is decoded (the melodia step then finds no leftover energy).
void testBendPostProcessing() {
   using namespace libaudio;
   std::printf("\n--- get_pitch_bends post-processing (no model) ---\n");
   const ModelDescriptor& desc = basicPitchDescriptor();
   PianoRoll roll(desc);

   const int64_t nFrames = 40;
   const int64_t nNote = desc.nNoteBins;       // 88
   const int64_t nContour = desc.nContourBins; // 264
   const int64_t pitchBin = 39; // MIDI 60 (C4) = bin 39 (offset 21)
   const int64_t s = 5;
   const int64_t e = 26; // half-open [5, 26): 21 frames > minNoteLen (11)
   // The note's centre in the contour: 3 * pitchBin = 117 (mid-range, so the
   // 51-bin window is unclamped and the peak/shift land on the window centre).
   const int64_t centerCol = 3 * pitchBin; // 117
   const int64_t annotNFrames = 172;

   auto makeFramesOnsets = [&]() {
      Tensor frames({nFrames, nNote},
                    static_cast<size_t>(nFrames) * static_cast<size_t>(nNote));
      Tensor onsets({nFrames, nNote},
                    static_cast<size_t>(nFrames) * static_cast<size_t>(nNote));
      for (int64_t r = s; r < e; ++r) {
         frames.data[static_cast<size_t>(r) * static_cast<size_t>(nNote) +
                     static_cast<size_t>(pitchBin)] = 0.8f;
      }
      onsets.data[static_cast<size_t>(s) * static_cast<size_t>(nNote) +
                  static_cast<size_t>(pitchBin)] = 0.9f; // onset peak
      return std::make_pair(std::move(frames), std::move(onsets));
   };

   auto makeContour = [&](int64_t peakCol) {
      Tensor c({nFrames, nContour},
               static_cast<size_t>(nFrames) * static_cast<size_t>(nContour));
      for (int64_t r = s; r < e; ++r) {
         c.data[static_cast<size_t>(r) * static_cast<size_t>(nContour) +
                static_cast<size_t>(peakCol)] = 1.0f;
      }
      return c;
   };

   // Case A: the contour peak is exactly on the base pitch → flat, 0 ticks.
   {
      auto fo = makeFramesOnsets();
      auto notes = roll.process(fo.first, fo.second, makeContour(centerCol),
                                annotNFrames, BasicPitchOptions{});
      check(notes.size() == 1, "one note decoded from the synthetic maps");
      if (!notes.empty()) {
         const int64_t frames = e - s;
         check(static_cast<int64_t>(notes[0].pitchBends.size()) == frames,
               "flat note: one bend value per frame");
         bool allZero = !notes[0].pitchBends.empty();
         for (int16_t t : notes[0].pitchBends) {
            if (t != 0) {
               allZero = false;
               break;
            }
         }
         check(allZero,
               "flat note: every bend value is 0 (peak on base pitch)");
      }
   }

   // Case B: the contour peak is offset by +3 bins (1 semitone) → the known
   // non-zero tick count round(3 * 4096 / 3) = 4096 on every frame.
   {
      auto fo = makeFramesOnsets();
      auto notes = roll.process(fo.first, fo.second, makeContour(centerCol + 3),
                                annotNFrames, BasicPitchOptions{});
      check(notes.size() == 1, "offset note decoded (one note)");
      if (!notes.empty()) {
         const int64_t frames = e - s;
         check(static_cast<int64_t>(notes[0].pitchBends.size()) == frames,
               "offset note: one bend value per frame");
         const int16_t expected =
            static_cast<int16_t>(std::llround(3.0 * 4096.0 / 3.0)); // 4096
         bool allExpected = !notes[0].pitchBends.empty();
         for (int16_t t : notes[0].pitchBends) {
            if (t != expected) {
               allExpected = false;
               break;
            }
         }
         check(allExpected,
               "offset note: every bend == round(3 bins * 4096/3) = 4096");
      }
   }

   // Case C: the no-contour overload never fills bends.
   {
      auto fo = makeFramesOnsets();
      auto notes =
         roll.process(fo.first, fo.second, annotNFrames, BasicPitchOptions{});
      check(notes.size() == 1 && notes[0].pitchBends.empty(),
            "no-contour process yields a note with no bends");
   }
}

// --- Raw-map write/read round-trip: the binary format, no model ----------
//
// Exercises the dependency-free raw-map file format (rawMap.{h,cpp}) without
// running the model: build small synthetic note/onset/contour maps, assemble a
// `RawPredictions`, write it to a temp file, read it back, and check that
//   (a) every field round-trips exactly (float32 is written by byte, so the
//       maps are bit-identical; the scalar `annotNFrames` / `haveContour`
//       match), and
//   (b) decoding the *read-back* maps with `PianoRoll` yields the same notes
//       as decoding the in-memory originals — i.e. the format is lossless for
//       the downstream post-processor.
//
// One flat note is engineered (constant 0.8 in a single column, a single onset
// peak, a flat contour on the base pitch) so the decode comparison is
// unambiguous: exactly one note, no bends.
void testRawMapRoundTrip() {
   using namespace libaudio;
   std::printf("\n--- raw-map write/read round-trip (no model) ---\n");
   const ModelDescriptor& desc = basicPitchDescriptor();
   PianoRoll roll(desc);

   const int64_t nFrames = 40;
   const int64_t nNote = desc.nNoteBins;       // 88
   const int64_t nContour = desc.nContourBins; // 264
   const int64_t pitchBin = 39; // MIDI 60 (C4) = bin 39 (offset 21)
   const int64_t s = 5;
   const int64_t e = 26; // half-open [5, 26): 21 frames > minNoteLen (11)
   const int64_t centerCol = 3 * pitchBin; // 117 (mid-range, unclamped window)
   const int64_t annotNFrames = 172;

   Tensor noteMap({nFrames, nNote},
                  static_cast<size_t>(nFrames) * static_cast<size_t>(nNote));
   Tensor onsetMap({nFrames, nNote},
                   static_cast<size_t>(nFrames) * static_cast<size_t>(nNote));
   for (int64_t r = s; r < e; ++r) {
      noteMap.data[static_cast<size_t>(r) * static_cast<size_t>(nNote) +
                   static_cast<size_t>(pitchBin)] = 0.8f;
   }
   onsetMap.data[static_cast<size_t>(s) * static_cast<size_t>(nNote) +
                 static_cast<size_t>(pitchBin)] = 0.9f; // onset peak
   Tensor contourMap({nFrames, nContour}, static_cast<size_t>(nFrames) *
                                             static_cast<size_t>(nContour));
   for (int64_t r = s; r < e; ++r) {
      contourMap.data[static_cast<size_t>(r) * static_cast<size_t>(nContour) +
                      static_cast<size_t>(centerCol)] = 1.0f;
   }

   RawPredictions pred;
   pred.noteMap = noteMap;
   pred.onsetMap = onsetMap;
   pred.contourMap = contourMap;
   pred.annotNFrames = annotNFrames;
   pred.haveContour = true;

   // A unique temp file; it is removed at the end regardless of outcome.
   const std::string path = (std::filesystem::temp_directory_path() /
                             "libaudio_tier2_rawmap_roundtrip.rawmap")
                               .string();

   bool ioOk = true;
   try {
      writeRawPredictions(pred, path);
   } catch (const std::exception& ex) {
      std::printf("  [FAIL] writeRawPredictions threw: %s\n", ex.what());
      ioOk = false;
   }
   check(ioOk, "writeRawPredictions succeeds");

   bool rtOk = true;
   RawPredictions back;
   try {
      back = readRawPredictions(path);
   } catch (const std::exception& ex) {
      std::printf("  [FAIL] readRawPredictions threw: %s\n", ex.what());
      rtOk = false;
   }
   check(rtOk, "readRawPredictions succeeds");

   if (rtOk) {
      // Scalar fields.
      check(back.annotNFrames == annotNFrames, "annotNFrames round-trips");
      check(back.haveContour == pred.haveContour, "haveContour round-trips");
      // Map shapes.
      check(back.noteMap.dims == noteMap.dims, "noteMap shape round-trips");
      check(back.onsetMap.dims == onsetMap.dims, "onsetMap shape round-trips");
      check(back.contourMap.dims == contourMap.dims,
            "contourMap shape round-trips");
      // float32 is written by byte, so the element buffers are bit-identical.
      check(back.noteMap.data == noteMap.data,
            "noteMap data is byte-identical");
      check(back.onsetMap.data == onsetMap.data,
            "onsetMap data is byte-identical");
      check(back.contourMap.data == contourMap.data,
            "contourMap data is byte-identical");

      // The decode must agree: re-decoding the read-back maps yields the same
      // notes as decoding the in-memory originals (lossless for the post-proc).
      const std::vector<Note> a =
         roll.process(pred.noteMap, pred.onsetMap, pred.contourMap,
                      annotNFrames, BasicPitchOptions{});
      const std::vector<Note> b =
         roll.process(back.noteMap, back.onsetMap, back.contourMap,
                      annotNFrames, BasicPitchOptions{});
      check(a.size() == b.size(), "decoded note count matches");
      bool same = a.size() == b.size();
      for (size_t i = 0; same && i < a.size(); ++i) {
         if (a[i].pitch != b[i].pitch || a[i].startTime != b[i].startTime ||
             a[i].endTime != b[i].endTime || a[i].velocity != b[i].velocity ||
             a[i].channel != b[i].channel ||
             a[i].pitchBends != b[i].pitchBends) {
            same = false;
         }
      }
      check(same, "decoded notes match the originals (lossless)");
   }

   std::filesystem::remove(path); // best-effort cleanup
}

} // namespace

int main() {
   using namespace libaudio;

   std::printf("=== libaudio Tier-2 smoke test (basic-pitch / ONNX) ===\n");

   // Pure post-processing (model-free): the get_pitch_bends port. Runs first
   // so it is exercised even in an environment where the model can't load.
   testBendPostProcessing();

   // Pure serialization (model-free): the raw-map write/read round-trip.
   testRawMapRoundTrip();

   // --- 1. Load the model ---------------------------------------------------
   // The model is embedded into the binary (nmp_onnx_data.h); there is no file
   // path to resolve. This exercises the loadFromMemory path the shipping
   // binary uses.
   std::printf("Model: embedded nmp.onnx (%zu bytes)\n", nmp_onnx_len);

   OnnxSession session;
   bool loaded = false;
   try {
      session.loadFromMemory(nmp_onnx, nmp_onnx_len, /*useCoreMl=*/true);
      loaded = true;
   } catch (const std::exception& e) {
      std::printf("  [FAIL] load threw: %s\n", e.what());
   }
   check(session.isLoaded() && loaded, "OnnxSession loads embedded nmp.onnx");
   if (!session.isLoaded()) {
      std::printf("tier2_smoke: FAILED (could not load model)\n");
      return 1;
   }

   std::printf("  Core ML EP active: %s\n",
               session.coreMlActive() ? "yes" : "no (CPU fallback)");
   std::printf("  Inputs : %u  [%s]\n", session.numInputs(),
               shapeStr(session.inputShape()).c_str());
   for (const auto& n : session.inputNames()) {
      std::printf("           in: '%s'\n", n.c_str());
   }
   std::printf("  Outputs: %u\n", session.numOutputs());
   for (const auto& n : session.outputNames()) {
      std::printf("           out: '%s'\n", n.c_str());
   }

   // --- 2. Build a known synthetic input: sustained A4 (440 Hz) sine --------
   const ModelDescriptor& desc = basicPitchDescriptor();
   const int64_t window = desc.windowSamples;                    // 43844
   const int64_t channels = static_cast<int64_t>(desc.channels); // 1
   const double sr = static_cast<double>(desc.sampleRate);       // 22050
   const double freq = 440.0;                                    // A4

   // Input tensor shape: (1, windowSamples, channels) = (1, 43844, 1).
   Tensor input({1, window, channels},
                static_cast<size_t>(1 * window * channels));
   for (int64_t i = 0; i < window; ++i) {
      // A steady 0.5-amplitude sine. The first sample is an "attack"; the rest
      // is a sustained tone, which is what drives the per-frame note
      // activation.
      const double t = static_cast<double>(i) / sr;
      const double s = 0.5 * std::sin(2.0 * M_PI * freq * t);
      input.data[static_cast<size_t>(i * channels)] = static_cast<float>(s);
   }

   // Fail-fast: the input must match the descriptor's input contract.
   bool inputValid = true;
   try {
      desc.validateInput(input);
   } catch (const std::exception& e) {
      inputValid = false;
      std::printf("  [FAIL] validateInput: %s\n", e.what());
   }
   check(inputValid, "input tensor matches the descriptor input contract");

   // --- 3. Run + verify -----------------------------------------------------
   std::vector<Tensor> outputs;
   bool ran = false;
   try {
      outputs = session.run(input);
      ran = true;
   } catch (const std::exception& e) {
      std::printf("  [FAIL] run threw: %s\n", e.what());
   }
   check(ran, "model run() succeeds");
   if (!ran) {
      std::printf("tier2_smoke: FAILED (%d check(s) failed)\n", g_failures);
      return 1;
   }

   check(outputs.size() == session.numOutputs(),
         "run() returns one tensor per declared output");
   check(session.numOutputs() == 3, "model has the expected 3 outputs");

   for (size_t i = 0; i < outputs.size(); ++i) {
      std::printf("  output[%zu] '%s' shape %s  maxAbs %.4f\n", i,
                  (i < session.outputNames().size()
                      ? session.outputNames()[i].c_str()
                      : "?"),
                  shapeStr(outputs[i].dims).c_str(), maxAbs(outputs[i]));
   }

   // The descriptor validation is the fail-fast contract check.
   bool outputsValid = true;
   try {
      desc.validateOutputs(outputs);
   } catch (const std::exception& e) {
      outputsValid = false;
      std::printf("  [FAIL] validateOutputs: %s\n", e.what());
   }
   check(outputsValid, "outputs match the descriptor output contract");

   // --- 4. Does the model actually *see* the A4? ----------------------------
   // Find the *note* output by its declared name (the descriptor lists it
   // first); fall back to the first 88-wide map if names are unavailable. The
   // runtime may return outputs in any order, so we never assume a position.
   const auto outNames = session.outputNames();
   const Tensor* noteOut = nullptr;
   for (size_t i = 0; i < outputs.size() && i < outNames.size(); ++i) {
      if (!desc.outputNames.empty() &&
          outNames[i] == desc.outputNames.front()) {
         noteOut = &outputs[i];
         break;
      }
   }
   if (noteOut == nullptr) {
      for (const Tensor& o : outputs) {
         const int64_t width = o.dims.empty() ? -1 : o.dims.back();
         if (width == desc.nNoteBins) {
            noteOut = &o; // first 88-wide map is the note activation map
            break;
         }
      }
   }
   check(noteOut != nullptr, "found the note-activation output");
   if (noteOut != nullptr) {
      // Squeeze the leading batch dim (always 1) into a logical (frames, 88)
      // map so the 2-D Tensor helpers (rows / argmaxLastAxisPerRow) apply.
      Tensor note;
      if (noteOut->dims.size() == 3 && noteOut->dims[0] == 1) {
         note.dims = {noteOut->dims[1], noteOut->dims[2]};
         note.data =
            noteOut->data; // batch=1: the flat row-major layout is unchanged
      } else {
         note = *noteOut;
      }
      const float peak = maxAbs(note);
      std::printf("  note-activation peak across window: %.4f\n", peak);
      check(peak > 0.10f,
            "model responds to the A4 tone (note activation is non-trivial)");

      // Per-frame argmax → MIDI, sampled across the sustained middle of the
      // window.
      const auto argmax = note.argmaxLastAxisPerRow();
      const int64_t midFrame = note.rows() / 2;
      const int64_t detectedMidi =
         static_cast<int64_t>(argmax[static_cast<size_t>(midFrame)]) +
         desc.midiOffset;
      std::printf("  argmax pitch at mid-window frame %lld: MIDI %lld (%s)\n",
                  static_cast<long long>(midFrame),
                  static_cast<long long>(detectedMidi),
                  detectedMidi == 69 ? "== A4, the input" : "(informational)");
   }

   std::printf("=== tier2_smoke: %s (%d check(s) failed) ===\n",
               g_failures == 0 ? "PASSED" : "FAILED", g_failures);
   return g_failures == 0 ? 0 : 1;
}

#endif // LIBAUDIO_HAS_TIER2

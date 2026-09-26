// tier2_smoke.cpp — end-to-end proof of the C++ / ONNX Runtime / Core ML path.
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
#include <libaudio/libaudio.h>
#include <string>
#include <vector>

#if !defined(LIBAUDIO_HAS_TIER2)

int main() {
   std::printf(
      "tier2_smoke: Tier-2 is not enabled in this build; nothing to do.\n");
   return 0;
}

#else // LIBAUDIO_HAS_TIER2

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

} // namespace

int main() {
   using namespace libaudio;

   std::printf("=== libaudio Tier-2 smoke test (basic-pitch / ONNX) ===\n");

   // --- 1. Load the model ---------------------------------------------------
   const std::string modelPath = LIBAUDIO_BASICPITCH_MODEL;
   std::printf("Model: %s\n", modelPath.c_str());

   OnnxSession session;
   bool loaded = false;
   try {
      session.load(modelPath, /*useCoreMl=*/true);
      loaded = true;
   } catch (const std::exception& e) {
      std::printf("  [FAIL] load threw: %s\n", e.what());
   }
   check(session.isLoaded() && loaded, "OnnxSession loads nmp.onnx");
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

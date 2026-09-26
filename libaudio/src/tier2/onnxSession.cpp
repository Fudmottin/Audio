// onnxSession.cpp — the single libaudio translation unit that includes ONNX
// Runtime headers. Everything else in the Tier-2 layer (and every consuming
// module) depends only on the clean `onnxSession.h` surface, never on Ort
// types.

#include "libaudio/onnxSession.h"

// --- Third-party (ONNX Runtime). -------------------------------------------
// Included after the local header and wrapped in clang-format off/on so the
// project's include sorting cannot reorder the C++ API against its own
// expectations (a third-party C++ API comes after the local header, after the
// std headers — the same include-ordering convention the aubio wrappers use).
//
// The ONNX headers (notably onnxruntime_float16.h) contain their own
// -Wconversion / -Wsign-conversion triggers that we cannot fix from here.
// Scope those two warnings out for the includes only, so the project's
// -Werror keeps policing *our* code while the third-party header cannot break
// the build. (Both GCC and Clang honor `#pragma GCC diagnostic`.)
// clang-format off
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#include <onnxruntime/coreml_provider_factory.h>
#include <onnxruntime/onnxruntime_cxx_api.h>
#pragma GCC diagnostic pop
// clang-format on

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace libaudio {

// ============================================================================
// Impl — isolates all ONNX Runtime (Ort::*) state and calls.
//
// Owns the runtime environment and the loaded session, plus a cached copy of
// the model's I/O contract (names + input shape) so that per-call accessors
// and the fail-fast validators never have to re-introspect the model.
// ============================================================================
struct OnnxSession::Impl {
   // One environment per session is fine for a single-model tool. (If we ever
   // run many sessions concurrently, promote this to a process-wide singleton.)
   Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "libaudio-onnx"};

   std::unique_ptr<Ort::Session> session;

   std::vector<std::string> inputNames;
   std::vector<std::string> outputNames;
   std::vector<int64_t> inputShape;

   // RunOptions reused across runs; left at defaults (synchronous, no flags).
   Ort::RunOptions runOptions;

   bool coreMlActive = false;
   bool loaded = false;
};

OnnxSession::OnnxSession()
   : impl_(std::make_unique<Impl>()) {}

OnnxSession::~OnnxSession() = default;

// Move the implementation pointer; this avoids relying on individual Ort
// members being movable.
OnnxSession::OnnxSession(OnnxSession&& other) noexcept
   : impl_(std::move(other.impl_)) {}

OnnxSession& OnnxSession::operator=(OnnxSession&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
   }
   return *this;
}

void OnnxSession::load(std::string_view path, bool useCoreMl) {
   // The ONNX C++ API's path type is ORTCHAR_T: `wchar_t` on Windows and `char`
   // on macOS/Linux. Typing the path as basic_string<ORTCHAR_T> keeps this one
   // line correct on both. All our model paths are ASCII, so the char→wchar_t
   // conversion on Windows is safe.
   std::basic_string<ORTCHAR_T> ortPath(path.begin(), path.end());

   Ort::SessionOptions opts;
   if (useCoreMl) {
      // Ask for the Core ML execution provider (Apple GPU / Neural Engine).
      //
      // In ORT 1.30 this is a standalone C function
      // (coreml_provider_factory.h), NOT a member of OrtApi. It takes the raw
      // OrtSessionOptions* — the C++ wrapper converts to that implicitly — and
      // returns an OrtStatus* that we wrap in the RAII Ort::Status.
      // `COREML_FLAG_USE_NONE` (0) means "any Apple device". If this model
      // cannot run on Core ML the append returns a non-OK status but the
      // session is still created and runs on CPU, so we record the outcome
      // rather than failing; coreMlActive() reports it.
      impl_->coreMlActive =
         Ort::Status(OrtSessionOptionsAppendExecutionProvider_CoreML(
                        opts, static_cast<int>(COREML_FLAG_USE_NONE)))
            .IsOK();
   }

   try {
      impl_->session =
         std::make_unique<Ort::Session>(impl_->env, ortPath.c_str(), opts);
   } catch (const Ort::Exception& e) {
      throw std::runtime_error(std::string("Failed to load ONNX model '") +
                               std::string(path) + "': " + e.what());
   }

   // Cache the I/O contract for cheap per-call access + fail-fast validation.
   // ORT 1.30: GetInputNames()/GetOutputNames() return
   // std::vector<std::string>, and the count accessors *return* a size_t (the
   // out-param overloads are gone).
   impl_->inputNames = impl_->session->GetInputNames();
   impl_->outputNames = impl_->session->GetOutputNames();

   impl_->inputShape.clear();
   if (impl_->session->GetInputCount() > 0) {
      // ORT 1.30:
      // Session::GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape()
      // (the older Session::GetInputTypeAndShape() was removed).
      impl_->inputShape = impl_->session->GetInputTypeInfo(0)
                             .GetTensorTypeAndShapeInfo()
                             .GetShape();
   }

   impl_->loaded = true;
}

bool OnnxSession::isLoaded() const {
   return impl_->loaded && impl_->session != nullptr;
}

std::vector<Tensor> OnnxSession::run(const Tensor& input) {
   if (!impl_->loaded || !impl_->session) {
      throw std::runtime_error(
         "OnnxSession::run() called before a successful load()");
   }

   // The model input is handed to ORT by pointer (no copy). We never mutate it
   // during the run, so the const_cast is safe.
   //
   // ORT 1.30: the CPU memory-info factory is MemoryInfo::CreateCpu(...) — the
   // old MemoryInfo::Create(name, memType) overload was removed. Arena
   // allocator
   // + CPU output is the standard choice for a host tensor feeding the session.
   Ort::MemoryInfo memInfo =
      Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator,
                                 OrtMemType::OrtMemTypeCPU);
   Ort::Value ortInput =
      Ort::Value::CreateTensor<float>(memInfo,
                                      const_cast<float*>(input.data.data()),
                                      input.data.size(), input.dims.data(),
                                      input.dims.size());

   // The C++ Run() overload wants `const char* const*` for the name arrays.
   // We have exactly one input; `&inputName` is the pointer to that single name
   // (a null when the model declared none, which ORT tolerates).
   const char* inputName =
      impl_->inputNames.empty() ? nullptr : impl_->inputNames.front().c_str();
   std::vector<const char*> outputNames;
   outputNames.reserve(impl_->outputNames.size());
   for (const std::string& name : impl_->outputNames) {
      outputNames.push_back(name.c_str());
   }

   try {
      auto outputs =
         impl_->session->Run(impl_->runOptions, &inputName, &ortInput, 1,
                             outputNames.empty() ? nullptr : outputNames.data(),
                             outputNames.size());

      // ORT 1.30's GetTensorData<float>() returns a `const float*` into the
      // tensor's buffer (no destination argument); copy it into our Tensor.
      std::vector<Tensor> result;
      result.reserve(outputs.size());
      for (const Ort::Value& out : outputs) {
         const Ort::TensorTypeAndShapeInfo info =
            out.GetTensorTypeAndShapeInfo();
         const std::vector<int64_t> dims = info.GetShape();
         const size_t count = info.GetElementCount();
         Tensor tensor(dims, count);
         if (count > 0) {
            const float* src = out.GetTensorData<float>();
            std::copy(src, src + count, tensor.data.begin());
         }
         result.push_back(std::move(tensor));
      }
      return result;
   } catch (const Ort::Exception& e) {
      throw std::runtime_error(
         std::string("ONNX Runtime failed to run the model: ") + e.what());
   }
}

uint32_t OnnxSession::numInputs() const {
   return static_cast<uint32_t>(impl_->inputNames.size());
}

uint32_t OnnxSession::numOutputs() const {
   return static_cast<uint32_t>(impl_->outputNames.size());
}

std::vector<std::string> OnnxSession::inputNames() const {
   return impl_->inputNames;
}

std::vector<std::string> OnnxSession::outputNames() const {
   return impl_->outputNames;
}

std::vector<int64_t> OnnxSession::inputShape() const {
   return impl_->inputShape;
}

bool OnnxSession::coreMlActive() const { return impl_->coreMlActive; }

} // namespace libaudio

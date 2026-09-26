/**
 * @file onnxSession.h
 * @brief A Pimpl wrapper around an ONNX Runtime session — the single point
 *        where ONNX Runtime headers are allowed to appear.
 *
 * This is the one class in libaudio that knows about ONNX Runtime. It follows
 * the same `std::unique_ptr<Impl>` Pimpl pattern as every other libaudio
 * module (see `pitch.h`): the public header includes **only** standard C++ and
 * `onnxTensor.h`, and every `Ort::*` call is isolated inside `Impl`. The rest
 * of the codebase (adapters, post-processing, tests) depends on this clean
 * surface and never sees ONNX types.
 *
 * @section onnx-session-contract Contract
 *
 * - `load(path, useCoreMl)` opens a model file and, if requested, asks for the
 *   **Core ML** execution provider (Apple GPU / Neural Engine). If the model
 *   cannot run on Core ML, the session transparently falls back to CPU rather
 *   than failing — `coreMlActive()` reports which path was actually taken.
 * - `run(input)` executes the model once and returns **all** output tensors,
 *   in the model's declared output order, as `libaudio::Tensor`s.
 * - Introspection accessors (`inputNames()`, `outputNames()`, `inputShape()`,
 *   `numInputs()`, `numOutputs()`) expose the model's I/O contract so a
 *   `ModelDescriptor` can be validated fail-fast (see `modelDescriptor.h`).
 *
 * @section onnx-session-env Environment
 *
 * Each `OnnxSession` owns its own `Ort::Env` inside its `Impl`. For a
 * single-model tool this is fine; if we ever run many sessions we can promote
 * the env to a process-wide singleton.
 */

#ifndef LIBAUDIO_ONNX_SESSION_H
#define LIBAUDIO_ONNX_SESSION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "onnxTensor.h"

namespace libaudio {

// ============================================================================
// OnnxSession — load and run a single ONNX model.
//
// Domain context: the runtime home for a Tier-2 neural model (basic-pitch, and
// later TF-MAGS / Demucs). One `OnnxSession` hosts one `.onnx` file. Model-
// specific knowledge (what the outputs *mean*, how to window the audio) lives
// in the per-model adapters, NOT here — this class is model-agnostic.
//
// RAII: the ONNX Runtime session is released automatically when the object is
// destroyed. Non-copyable (the underlying session is not), movable.
// ============================================================================
class OnnxSession {
 public:
   // Create an empty, not-yet-loaded session.
   OnnxSession();

   // Destructor. Releases the ONNX Runtime session (if loaded).
   ~OnnxSession();

   // Non-copyable (an Ort session is not copyable).
   OnnxSession(const OnnxSession&) = delete;
   OnnxSession& operator=(const OnnxSession&) = delete;

   // Movable.
   OnnxSession(OnnxSession&& other) noexcept;
   OnnxSession& operator=(OnnxSession&& other) noexcept;

   /**
    * Load a model file and prepare it for inference.
    *
    * @param path       Path to a serialized `.onnx` model.
    * @param useCoreMl  If true, request the Core ML execution provider for
    *                   Apple GPU/ANE acceleration. If the model does not run
    *                   on Core ML, the session falls back to CPU and still
    *                   succeeds; `coreMlActive()` reports the actual path.
    *                   Default: true.
    *
    * @throws std::runtime_error if the model file cannot be loaded.
    */
   void load(std::string_view path, bool useCoreMl = true);

   /**
    * Whether a model is currently loaded and ready to run.
    */
   [[nodiscard]] bool isLoaded() const;

   /**
    * Run the model once with a single input tensor.
    *
    * @param input  The model's single input, as a `libaudio::Tensor` (must
    *               match the model's declared input shape).
    * @return Every output tensor, in the model's declared output order.
    *
    * @throws std::runtime_error if no model is loaded or the run fails.
    */
   std::vector<Tensor> run(const Tensor& input);

   /** Number of model inputs. */
   [[nodiscard]] uint32_t numInputs() const;

   /** Number of model outputs. */
   [[nodiscard]] uint32_t numOutputs() const;

   /** The model's input tensor names (in declared order). */
   [[nodiscard]] std::vector<std::string> inputNames() const;

   /** The model's output tensor names (in declared order). */
   [[nodiscard]] std::vector<std::string> outputNames() const;

   /** The model's declared input shape; a dimension of -1 is dynamic. */
   [[nodiscard]] std::vector<int64_t> inputShape() const;

   /**
    * Whether the Core ML execution provider is actually active on this session.
    * False means the run falls to CPU (still correct — just not accelerated).
    */
   [[nodiscard]] bool coreMlActive() const;

 private:
   // Private implementation — all ONNX Runtime (Ort::*) usage is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_ONNX_SESSION_H

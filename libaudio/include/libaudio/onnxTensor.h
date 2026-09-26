/**
 * @file onnxTensor.h
 * @brief A minimal, dependency-free tensor (shape + contiguous float32 data).
 *
 * This is the neutral value type that flows between libaudio's ONNX layer and
 * the per-model adapters. It deliberately has **no** dependency on ONNX
 * Runtime (or aubio, or any DSP library) so it can be used, tested, and passed
 * around freely, and so the ONNX headers stay confined to the single
 * `onnxSession.cpp` translation unit.
 *
 * @section onnx-tensor-layout Layout
 *
 * A `Tensor` is a one-dimensional `std::vector<float>` interpreted through a
 * `dims` vector (its shape, in "C" / row-major order). The element count must
 * equal the product of `dims`. For a model output such as basic-pitch's
 * `(1, 172, 88)` note-activation map, `dims = {1, 172, 88}` and
 * `data.size() == 1 * 172 * 88`.
 *
 * This is a value type: it is cheap to copy/move (the data vector dominates)
 * and has no hidden state.
 *
 * @section onnx-tensor-tier2 Tier-2 grouping
 *
 * `onnxTensor`, `onnxSession`, `modelDescriptor`, `transcriber`,
 * `basicPitch`, and `pianoRoll` together form libaudio's **Tier-2** layer
 * (neural transcription on ONNX Runtime). All of them are gated behind
 * `LIBAUDIO_ENABLE_TIER2` and are no-ops when it is OFF.
 */

#ifndef LIBAUDIO_ONNX_TENSOR_H
#define LIBAUDIO_ONNX_TENSOR_H

#include <cstdint>
#include <vector>

namespace libaudio {

// ============================================================================
// Tensor — a shape plus a contiguous block of float32 elements.
//
// Domain context: this is the C++ analogue of a single ONNX Runtime output
// tensor. It is what `OnnxSession::run()` returns and what the per-model
// adapters (e.g. basic-pitch) consume. Keeping it free of ONNX types is what
// lets the rest of the codebase reason about model results without ever seeing
// an `Ort::Value`.
//
// Aggregate value type; default copy/move semantics are what we want.
// ============================================================================
struct Tensor {
   /**
    * The tensor shape, in row-major order. Example: `{1, 172, 88}` for a
    * single-window, 172-frame, 88-pitch-bins note-activation map.
    */
   std::vector<int64_t> dims;

   /**
    * The element data, stored contiguously in row-major order.
    * Invariant: `data.size() == product(dims)`.
    */
   std::vector<float> data;

   Tensor() = default;

   /**
    * Construct a zero-filled tensor of the given shape.
    *
    * @param shape  The tensor shape (number of elements per dimension).
    * @param count  Explicit element count. Must equal `product(shape)`; it is
    *               taken as authoritative for the `data` allocation.
    */
   Tensor(std::vector<int64_t> shape, size_t count)
      : dims(std::move(shape))
      , data(count, 0.0f) {}

   // Default copy/move semantics are fine for this value type.
   Tensor(const Tensor&) = default;
   Tensor(Tensor&&) = default;
   Tensor& operator=(const Tensor&) = default;
   Tensor& operator=(Tensor&&) = default;

   /**
    * The number of dimensions (rank). Example: a `(1,172,88)` map has rank 3.
    */
   [[nodiscard]] int64_t rank() const;

   /**
    * The total number of elements, computed as the product of `dims`.
    * (May differ from `data.size()` if the tensor was constructed
    * inconsistently; the `count` argument of the constructor is authoritative
    * for `data`.)
    */
   [[nodiscard]] int64_t numElements() const;

   /**
    * Number of rows — the first dimension. Valid for rank >= 1; 0 otherwise.
    */
   [[nodiscard]] int64_t rows() const;

   /**
    * Number of columns — the last dimension. Valid for rank >= 2; 0 otherwise.
    */
   [[nodiscard]] int64_t cols() const;

   /**
    * Overwrite every element with a single value.
    */
   void fill(float value);

   /**
    * Read a single element of a 2D tensor by (row, column).
    * @throws std::out_of_range if the index is outside the tensor.
    */
   float at2(int64_t row, int64_t col) const;

   /**
    * For each row of a 2D tensor, return the column index of the maximum
    * element. This is the workhorse of "which pitch is strongest in this
    * frame" post-processing.
    *
    * @return A vector with one entry per row (length == `rows()`).
    */
   [[nodiscard]] std::vector<int64_t> argmaxLastAxisPerRow() const;
};

} // namespace libaudio

#endif // LIBAUDIO_ONNX_TENSOR_H

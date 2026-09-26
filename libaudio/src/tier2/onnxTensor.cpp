// onnxTensor.cpp — implementation of the dependency-free Tensor value type.

#include "libaudio/onnxTensor.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace libaudio {

int64_t Tensor::rank() const { return static_cast<int64_t>(dims.size()); }

int64_t Tensor::numElements() const {
   if (dims.empty()) {
      // No shape recorded; fall back to the actual data size.
      return static_cast<int64_t>(data.size());
   }
   int64_t n = 1;
   for (const int64_t d : dims) {
      n *= d;
   }
   return n;
}

int64_t Tensor::rows() const { return dims.size() >= 1 ? dims[0] : 0; }

int64_t Tensor::cols() const {
   return dims.size() >= 2 ? dims[dims.size() - 1] : 0;
}

void Tensor::fill(float value) { std::fill(data.begin(), data.end(), value); }

float Tensor::at2(int64_t row, int64_t col) const {
   if (dims.size() != 2) {
      throw std::invalid_argument("Tensor::at2 requires a 2D tensor (rank 2)");
   }
   if (row < 0 || row >= dims[0] || col < 0 || col >= dims[1]) {
      throw std::out_of_range("Tensor::at2 index out of range");
   }
   const size_t idx = static_cast<size_t>(row) * static_cast<size_t>(dims[1]) +
                      static_cast<size_t>(col);
   return data[idx];
}

std::vector<int64_t> Tensor::argmaxLastAxisPerRow() const {
   if (dims.size() != 2) {
      throw std::invalid_argument(
         "Tensor::argmaxLastAxisPerRow requires a 2D tensor (rank 2)");
   }
   const int64_t nRows = dims[0];
   const int64_t nCols = dims[1];
   std::vector<int64_t> argmax(static_cast<size_t>(nRows), 0);
   for (int64_t r = 0; r < nRows; ++r) {
      int64_t best = 0;
      float bestVal = -std::numeric_limits<float>::infinity();
      const size_t base = static_cast<size_t>(r) * static_cast<size_t>(nCols);
      for (int64_t c = 0; c < nCols; ++c) {
         const float v = data[base + static_cast<size_t>(c)];
         if (v > bestVal) {
            bestVal = v;
            best = c;
         }
      }
      argmax[static_cast<size_t>(r)] = best;
   }
   return argmax;
}

} // namespace libaudio

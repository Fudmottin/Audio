// rawMap.cpp — the small binary file format for `RawPredictions`.
//
// This is the on-disk home of the three stitched activation maps the model
// produces (see `rawMap.h`). It is deliberately dependency-free (plain
// `std::fstream`, no JSON) and little-endian: the goal is a fast, same-machine
// cache a knob-sweep can read to re-decode notes without re-running the model.
// (The resampled audio the maps were built from is *not* stored — it is cheap
// and regenerable from the source file on demand.)

#include "libaudio/rawMap.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace libaudio {

namespace {

// The file's leading signature and the one version this build reads/writes. A
// reader that finds a different magic or version fails fast instead of
// mis-reading arbitrary bytes as a map.
constexpr char kMagic[4] = {'L', 'R', 'M', '1'};
constexpr uint32_t kVersion = 1;

// Write one map as a `(rows, cols)` record followed by its elements. A map
// with a degenerate shape (an empty contour, or zero frames) is written as
// `0,0` with no data, so it round-trips as an empty tensor.
void writeTensor(std::ofstream& out, const Tensor& t) {
   int64_t rows = 0;
   int64_t cols = 0;
   if (t.dims.size() >= 2) {
      rows = t.dims[0];
      cols = t.dims[1];
   }
   out.write(reinterpret_cast<const char*>(&rows), sizeof(rows));
   out.write(reinterpret_cast<const char*>(&cols), sizeof(cols));
   if (rows > 0 && cols > 0) {
      const size_t n = static_cast<size_t>(rows) * static_cast<size_t>(cols);
      if (t.data.size() < n) {
         throw std::runtime_error("rawMap: map holds fewer elements than its"
                                  " shape; refusing to write a short tail");
      }
      out.write(reinterpret_cast<const char*>(t.data.data()),
                static_cast<std::streamsize>(n * sizeof(float)));
   }
   if (!out) {
      throw std::runtime_error("rawMap: failed writing a map body");
   }
}

// Read one `(rows, cols, data)` record back into a `Tensor` (2-D, row-major).
Tensor readTensor(std::ifstream& in) {
   int64_t rows = 0;
   int64_t cols = 0;
   in.read(reinterpret_cast<char*>(&rows), sizeof(rows));
   in.read(reinterpret_cast<char*>(&cols), sizeof(cols));
   if (!in) {
      throw std::runtime_error("rawMap: truncated map header");
   }
   Tensor t;
   if (rows > 0 && cols > 0) {
      const size_t n = static_cast<size_t>(rows) * static_cast<size_t>(cols);
      t.dims = {rows, cols};
      t.data.resize(n, 0.0f);
      in.read(reinterpret_cast<char*>(t.data.data()),
              static_cast<std::streamsize>(n * sizeof(float)));
      if (!in) {
         throw std::runtime_error("rawMap: truncated map data");
      }
   } else {
      t.dims = {0, 0}; // a degenerate map stays empty
   }
   return t;
}

} // namespace

void writeRawPredictions(const RawPredictions& pred, const std::string& path) {
   std::ofstream out(path, std::ios::binary | std::ios::trunc);
   if (!out) {
      throw std::runtime_error("rawMap: cannot open '" + path +
                               "' for writing");
   }
   out.write(kMagic, sizeof(kMagic));
   const uint32_t version = kVersion;
   out.write(reinterpret_cast<const char*>(&version), sizeof(version));
   const int64_t annotNFrames = pred.annotNFrames;
   out.write(reinterpret_cast<const char*>(&annotNFrames),
             sizeof(annotNFrames));
   const uint8_t haveContour = pred.haveContour ? 1 : 0;
   out.write(reinterpret_cast<const char*>(&haveContour), sizeof(haveContour));

   writeTensor(out, pred.noteMap);
   writeTensor(out, pred.onsetMap);
   writeTensor(out, pred.contourMap);
   out.close();
   if (!out) {
      throw std::runtime_error("rawMap: failed writing '" + path + "'");
   }
}

RawPredictions readRawPredictions(const std::string& path) {
   std::ifstream in(path, std::ios::binary);
   if (!in) {
      throw std::runtime_error("rawMap: cannot open '" + path +
                               "' for reading");
   }
   char magic[4];
   in.read(magic, sizeof(magic));
   if (!in || std::memcmp(magic, kMagic, sizeof(magic)) != 0) {
      throw std::runtime_error("rawMap: '" + path +
                               "' is not a raw-map file (bad magic)");
   }
   uint32_t version = 0;
   in.read(reinterpret_cast<char*>(&version), sizeof(version));
   if (!in) {
      throw std::runtime_error("rawMap: truncated header in '" + path + "'");
   }
   if (version != kVersion) {
      throw std::runtime_error("rawMap: unsupported version " +
                               std::to_string(version) + " in '" + path + "'");
   }
   RawPredictions pred;
   in.read(reinterpret_cast<char*>(&pred.annotNFrames),
           sizeof(pred.annotNFrames));
   uint8_t haveContour = 0;
   in.read(reinterpret_cast<char*>(&haveContour), sizeof(haveContour));
   if (!in) {
      throw std::runtime_error("rawMap: truncated header in '" + path + "'");
   }
   pred.haveContour = (haveContour != 0);

   pred.noteMap = readTensor(in);
   pred.onsetMap = readTensor(in);
   pred.contourMap = readTensor(in);
   if (!in) {
      throw std::runtime_error("rawMap: truncated file '" + path + "'");
   }
   return pred;
}

} // namespace libaudio

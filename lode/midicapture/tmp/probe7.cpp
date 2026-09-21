// Dump raw per-frame specflux descriptor (no peak-picking) via pvoc+specdesc.
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/cvec.h>
#include <aubio/spectral/specdesc.h>
#include <aubio/spectral/phasevoc.h>
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
   const char* path = argv[1];
   int window = argc > 2 ? std::atoi(argv[2]) : 2048;
   int hop = argc > 3 ? std::atoi(argv[3]) : 512;
   SF_INFO info{};
   SNDFILE* f = sf_open(path, SFM_READ, &info);
   if (!f) return 1;
   std::vector<float> raw((size_t)window * 2), mono(window);
   fvec_t* in = new_fvec(window);
   cvec_t* spectrum = new_cvec(window / 2 + 1);
   aubio_pvoc_t* pv = new_aubio_pvoc(window, hop);
   aubio_specdesc_t* d = new_aubio_specdesc("specflux", window);
   fvec_t* out = new_fvec(1);
   long idx = 0;
   while (true) {
      long n = sf_readf_float(f, raw.data(), window);
      if (n <= 0) break;
      for (long i = 0; i < n; ++i)
         mono[i] = (raw[i * 2] + raw[i * 2 + 1]) / 2.0f;
      if ((size_t)n < window) std::fill(mono.begin() + n, mono.end(), 0.0f);
      std::copy(mono.begin(), mono.end(), in->data);
      aubio_pvoc_do(pv, in, spectrum);
      aubio_specdesc_do(d, spectrum, out);
      double sum = 0;
      for (auto s : mono) sum += (double)s * s;
      float rms = (float)std::sqrt(sum / window);
      std::printf("idx=%4ld t=%.4f rms=%.5f specflux=%.4f\n", idx,
                  idx * hop / (double)info.samplerate, rms, out->data[0]);
      idx++;
   }
   sf_close(f);
   del_aubio_specdesc(d); del_aubio_pvoc(pv);
   del_fvec(in); del_cvec(spectrum); del_fvec(out);
   return 0;
}

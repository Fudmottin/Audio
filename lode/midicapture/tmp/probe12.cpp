// Onset probe, aubioonset-CLI style: pvoc(win, hop) filled with
// hop-sized frames, then the specflux descriptor is dumped per hop.
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
   std::vector<float> raw((size_t)hop * 2), mono(hop);
   fvec_t* in = new_fvec(hop);
   cvec_t* spectrum = new_cvec(window / 2 + 1);
   aubio_pvoc_t* pv = new_aubio_pvoc(window, hop);
   aubio_specdesc_t* d = new_aubio_specdesc("specflux", window);
   fvec_t* desc = new_fvec(1);
   long idx = 0;
   double maxDesc = 0;
   while (true) {
      long n = sf_readf_float(f, raw.data(), hop);
      if (n <= 0) break;
      for (long i = 0; i < n; ++i)
         mono[i] = (raw[i * 2] + raw[i * 2 + 1]) / 2.0f;
      if ((size_t)n < hop) std::fill(mono.begin() + n, mono.end(), 0.0f);
      std::copy(mono.begin(), mono.end(), in->data);
      aubio_pvoc_do(pv, in, spectrum);
      aubio_specdesc_do(d, spectrum, desc);
      double rms2 = 0;
      for (auto s : mono) rms2 += (double)s * s;
      float rms = (float)std::sqrt(rms2 / hop);
      maxDesc = std::max(maxDesc, (double)desc->data[0]);
      if (idx % 2 == 0 || desc->data[0] > 60.0)
         std::printf("idx=%4ld t=%.4f rms=%.5f specflux=%9.3f\n", idx,
                     idx * hop / (double)info.samplerate, rms, desc->data[0]);
      idx++;
   }
   std::printf("max specflux over file: %.3f (idx=%ld)\n", maxDesc, idx);
   sf_close(f);
   del_aubio_specdesc(d); del_aubio_pvoc(pv);
   del_fvec(in); del_cvec(spectrum); del_fvec(desc);
   return 0;
}

// Onset probe using pvoc with hop-sized input frames (like aubioonset CLI).
// Usage: probe9 <file> [window] [hop] [threshold] [minIoI_sec]
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/onset/onset.h>
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
   const char* path = argv[1];
   int window = argc > 2 ? std::atoi(argv[2]) : 2048;
   int hop = argc > 3 ? std::atoi(argv[3]) : 512;
   float onsetThr = argc > 4 ? std::atof(argv[4]) : 0.2f;
   float minIoI = argc > 5 ? std::atof(argv[5]) : -1.0f;
   SF_INFO info{};
   SNDFILE* f = sf_open(path, SFM_READ, &info);
   if (!f) return 1;
   std::vector<float> raw((size_t)hop * 2), mono(hop);
   fvec_t* in = new_fvec(hop); // pvoc expects hop-sized frames
   aubio_onset_t* o = new_aubio_onset("specflux", window, hop, info.samplerate);
   aubio_onset_set_threshold(o, onsetThr);
   if (minIoI >= 0.0f) aubio_onset_set_minioi_s(o, minIoI);
   fvec_t* out = new_fvec(1);
   long count = 0, idx = 0;
   double lastT = 0.0;
   bool first = true;
   while (true) {
      long n = sf_readf_float(f, raw.data(), hop);
      if (n <= 0) break;
      for (long i = 0; i < n; ++i)
         mono[i] = (raw[i * 2] + raw[i * 2 + 1]) / 2.0f;
      if ((size_t)n < hop) std::fill(mono.begin() + n, mono.end(), 0.0f);
      std::copy(mono.begin(), mono.end(), in->data);
      aubio_onset_do(o, in, out);
      double t = aubio_onset_get_last_s(o);
      if (first) {
         if (aubio_onset_get_last(o) == 0) continue;
         first = false;
      }
      if (t > lastT) {
         count++;
         lastT = t;
         if (count <= 30)
            std::printf("onset %2ld at t=%.4f (idx=%ld, out=%.3f)\n", count, t,
                        idx, out->data[0]);
      }
      idx++;
   }
   std::printf("TOTAL onsets: %ld over %ld hop-frames\n", count, idx);
   sf_close(f);
   del_aubio_onset(o); del_fvec(in); del_fvec(out);
   return 0;
}

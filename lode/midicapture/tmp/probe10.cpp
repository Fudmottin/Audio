// Pitch probe: rolling window of last 2048 samples, YINfft, per hop.
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/pitch/pitchyinfft.h>
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
int main(int argc, char** argv) {
   const char* path = argv[1];
   int window = argc > 2 ? std::atoi(argv[2]) : 2048;
   int hop = argc > 3 ? std::atoi(argv[3]) : 512;
   float confThr = argc > 4 ? std::atof(argv[4]) : 0.5f;
   SF_INFO info{};
   SNDFILE* f = sf_open(path, SFM_READ, &info);
   if (!f) return 1;
   std::vector<float> raw((size_t)hop * 2), mono(hop);
   std::vector<float> win(window, 0.0f);
   fvec_t* in = new_fvec(window);
   fvec_t* cands = new_fvec(window);
   aubio_pitchyinfft_t* p = new_aubio_pitchyinfft(info.samplerate, window);
   aubio_pitchyinfft_set_tolerance(p, 0.15f);
   long idx = 0;
   while (true) {
      long n = sf_readf_float(f, raw.data(), hop);
      if (n <= 0) break;
      for (long i = 0; i < n; ++i)
         mono[i] = (raw[i * 2] + raw[i * 2 + 1]) / 2.0f;
      if ((size_t)n < hop) std::fill(mono.begin() + n, mono.end(), 0.0f);
      win.erase(win.begin(), win.begin() + hop);
      win.insert(win.end(), mono.begin(), mono.end());
      std::copy(win.begin(), win.end(), in->data);
      aubio_pitchyinfft_do(p, in, cands);
      float hz = cands->data[0];
      float conf = aubio_pitchyinfft_get_confidence(p);
      double midi = hz > 0 ? 69.0 + 12.0 * std::log2(hz / 440.0) : -1.0;
      std::printf("idx=%4ld t=%.4f hz=%9.3f conf=%.3f midi=%7.3f confid=%d\n",
                  idx, idx * hop / (double)info.samplerate, hz, conf, midi,
                  (int)(conf >= confThr && hz > 0));
      idx++;
   }
   sf_close(f);
   del_aubio_pitchyinfft(p); del_fvec(in); del_fvec(cands);
   return 0;
}

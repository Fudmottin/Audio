// Onset probe with proper sequential hop reading.
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
   SF_INFO info{};
   SNDFILE* f = sf_open(path, SFM_READ, &info);
   if (!f) return 1;
   std::printf("sr=%d ch=%d frames=%ld thr=%.3f  (sequential hop=%d)\n",
               (int)info.samplerate, (int)info.channels, (long)info.frames,
               onsetThr, hop);
   std::vector<float> raw((size_t)window * 2), mono(window);
   fvec_t* in = new_fvec(window);
   fvec_t* out = new_fvec(1);
   aubio_onset_t* o = new_aubio_onset("specflux", window, hop, info.samplerate);
   aubio_onset_set_threshold(o, onsetThr);
   long idx = 0;
   while (true) {
      long n = sf_readf_float(f, raw.data(), window);
      if (n <= 0) break;
      for (long i = 0; i < n; ++i)
         mono[i] = (raw[i * 2] + raw[i * 2 + 1]) / 2.0f;
      if ((size_t)n < window) std::fill(mono.begin() + n, mono.end(), 0.0f);
      std::copy(mono.begin(), mono.end(), in->data);
      double sum = 0;
      for (auto s : mono) sum += (double)s * s;
      float rms = (float)std::sqrt(sum / window);
      aubio_onset_do(o, in, out);
      float des = aubio_onset_get_descriptor(o);
      float thdes = aubio_onset_get_thresholded_descriptor(o);
      bool fire = thdes != 0.0f;
      double t = idx * hop / (double)info.samplerate;
      if (fire || rms > 0.001f)
         std::printf("idx=%4ld t=%.3f rms=%.5f desc=%10.3f thr=%5.3f fire=%d last=%.3f\n",
                     idx, t, rms, des, aubio_onset_get_threshold(o),
                     (int)fire, aubio_onset_get_last_s(o));
      idx++;
      if (idx > 200) break;
   }
   sf_close(f);
   del_aubio_onset(o); del_fvec(in); del_fvec(out);
   return 0;
}

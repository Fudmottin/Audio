// Scratch debug probe: raw signal stats + raw aubio detector behavior.
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/onset/onset.h>
#include <aubio/pitch/pitchyinfft.h>
#include <sndfile.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
   const char* path = argv[1];
   int window = (argc > 2) ? std::atoi(argv[2]) : 2048;
   int hop = (argc > 3) ? std::atoi(argv[3]) : 512;
   float confThr = (argc > 4) ? std::atof(argv[4]) : 0.5f;
   float onsetThr = (argc > 5) ? std::atof(argv[5]) : 0.2f;
   float silenceDb = (argc > 6) ? std::atof(argv[6]) : -40.0f;

   SF_INFO info{};
   SNDFILE* f = sf_open(path, SFM_READ, &info);
   if (!f) { std::printf("open failed\n"); return 1; }
   std::printf("sr=%d ch=%d frames=%ld\n", (int)info.samplerate,
                (int)info.channels, (long)info.frames);

   std::vector<float> raw((size_t)window * 2), mono(window);
   aubio_pitchyinfft_t* pitch = new_aubio_pitchyinfft(info.samplerate, window);
   aubio_onset_t* onset =
      new_aubio_onset("specflux", window, hop, info.samplerate);
   aubio_onset_set_threshold(onset, onsetThr);
   fvec_t* in = new_fvec(window);
   fvec_t* out = new_fvec(window);
   float silLinear = std::pow(10.0f, silenceDb / 20.0f);

   long frame = 0;
   while (!sf_seek(f, frame, SEEK_SET)) {
      long n = sf_readf_float(f, raw.data(), window);
      if (n <= 0) break;
      for (long i = 0; i < n; ++i)
         mono[i] = (raw[i * 2] + raw[i * 2 + 1]) / 2.0f;
      if ((size_t)n < window)
         std::fill(mono.begin() + n, mono.end(), 0.0f);
      double t0 = (double)frame / info.samplerate;
      // copy to fvec
      std::copy(mono.begin(), mono.end(), in->data);
      // RMS
      double sum = 0;
      for (auto s : mono) sum += (double)s * s;
      float rms = (float)std::sqrt(sum / window);
      // aubio silent-frame check: does aubio consider this frame silent?
      aubio_onset_do(onset, in, out);
      bool onsetFired = out->data[0] != 0.0f;
      aubio_pitchyinfft_do(pitch, in, out);
      float pHz = out->data[0];
      float conf = aubio_pitchyinfft_get_confidence(pitch);
      double midi = 69.0 + 12.0 * std::log2(pHz / 440.0);
      bool silent = rms < silLinear;
      bool show =
         onsetFired || (conf >= confThr && pHz > 0 && !silent) || silent;
      if (show)
         std::printf("f=%ld t=%.3f rms=%.4f silent=%d pHz=%.2f conf=%.3f "
                     "midi=%.2f onset=%d thrDes=%.3f\n",
                     frame, t0, rms, (int)silent, pHz, conf,
                     pHz > 0 ? midi : -1.0, (int)onsetFired,
                     aubio_onset_get_thresholded_descriptor(onset));
      frame += hop;
      if (frame > info.frames) break;
   }
   sf_close(f);
   del_aubio_onset(onset);
   del_aubio_pitchyinfft(pitch);
   del_fvec(in);
   del_fvec(out);
   return 0;
}

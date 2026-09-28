// audioDecode.cpp — resolve an input audio path to one libsndfile can open.
//
// Two related guarantees live here, both built on the same probe-first design:
//   - `open()`:       a path libsndfile can read (native rate; the consumer
//                     resamples). Most sources open directly; only an
//                     unreadable container is ffmpeg-decoded to a throwaway 48
//                     kHz mono WAV.
//   - `decodeToRate()`: a path libsndfile can read *at a specific rate* — the
//                     source is ffmpeg-decoded + resampled into a throwaway
//                     mono float32 WAV. The `BasicPitch` adapter uses this to
//                     hand its 22050 Hz model correctly-rate mono audio.
//
// The probe-first choice is deliberate: the corpus and both adapters must read
// the *same* bytes through the *same* path, so a file libsndfile reads natively
// is used as-is (no re-encode, no resampling surprise — it is read at its own
// rate). Only a file libsndfile genuinely cannot open is ffmpeg-decoded, and
// the resulting temp file is unlinked when the `AudioSource` is destroyed.

#include <cstdlib>
#include <filesystem>
#include <libaudio/audioDecode.h>
#include <libaudio/audioFile.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace libaudio {

namespace {

// Single-quote a string for safe embedding in a POSIX shell command. The only
// special character in a single-quoted shell token is the single quote itself,
// which is escaped by closing, adding a literal quote, and re-opening. Every
// other byte (spaces, etc.) is literal inside single quotes.
std::string shellQuote(const std::string& s) {
   std::string out = "'";
   for (char c : s) {
      if (c == '\'') {
         out += "'\\''";
      } else {
         out += c;
      }
   }
   out += "'";
   return out;
}

// Run ffmpeg to decode `inputPath` into a throwaway mono WAV at `targetRate`
// Hz in `codec` (e.g. "pcm_s16le" / "pcm_f32le"), and return the temp path.
//
// The temp file lives next to the input, named from its stem plus `tag`, so two
// different calls on one input (a native decode vs. a specific-rate resample)
// never collide. The caller owns the returned path (it must be deleted — the
// `AudioSource` that wraps it does so on destruction). `tag` is the suffix used
// to distinguish the output files.
//
// @throws std::runtime_error if ffmpeg exits non-zero or produces no file.
std::string ffmpegDecodeToTemp(std::string_view inputPath, uint32_t targetRate,
                               const std::string& codec, const std::string& tag,
                               const std::string& ffmpegPath) {
   std::string in(inputPath);
   std::filesystem::path p(in);
   std::filesystem::path dir = p.parent_path();
   if (dir.empty()) {
      dir = ".";
   }
   const std::string tempPath =
      (dir / (p.stem().string() + "." + tag + ".wav")).string();

   const std::string cmd = std::string(ffmpegPath) + " -y -loglevel error -i " +
                           shellQuote(in) + " -ac 1 -ar " +
                           std::to_string(targetRate) + " -c:a " + codec + " " +
                           shellQuote(tempPath);

   // A non-zero exit or a missing/empty output both mean the decode failed.
   const int status = std::system(cmd.c_str());
   std::error_code ec;
   const bool produced = status == 0 && std::filesystem::exists(tempPath, ec) &&
                         !std::filesystem::is_empty(tempPath, ec);
   if (!produced) {
      // Clean up a partial file, if any.
      std::filesystem::remove(std::filesystem::path(tempPath), ec);
      throw std::runtime_error(
         "ffmpeg decode of '" + in + "' to " + std::to_string(targetRate) +
         " Hz failed (exit " + std::to_string(status) + ").");
   }
   return tempPath;
}

} // namespace

// ============================================================================
// Impl — holds the resolved path and whether a temp file must be deleted.
// ============================================================================
struct AudioSource::Impl {
   std::string path;
   bool owned = false;

   // Unlink the temp file, if we made one. Failures are ignored: a stale temp
   // is harmless (a fresh decode overwrites with ffmpeg -y).
   ~Impl() {
      if (owned) {
         std::error_code ec;
         std::filesystem::remove(std::filesystem::path(path), ec);
      }
   }
};

AudioSource::AudioSource()
   : impl_(std::make_unique<Impl>()) {}

AudioSource::AudioSource(std::string path, bool owned)
   : impl_(std::make_unique<Impl>()) {
   impl_->path = std::move(path);
   impl_->owned = owned;
}

AudioSource::~AudioSource() = default;

AudioSource::AudioSource(AudioSource&& other) noexcept
   : impl_(std::move(other.impl_)) {}

AudioSource& AudioSource::operator=(AudioSource&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
   }
   return *this;
}

const std::string& AudioSource::path() const { return impl_->path; }

bool AudioSource::owned() const { return impl_->owned; }

AudioSource AudioSource::open(std::string_view inputPath,
                              const std::string& ffmpegPath) {
   std::string path(inputPath);

   // 1. Probe: can libsndfile open this directly? If so, use it as-is (native
   //    rate — the consumer resamples). Nothing is written to disk.
   try {
      AudioFileReader probe(path); // throws if it cannot open the file
      (void)probe; // header was read; we only needed the probe to succeed
      return AudioSource(path, /*owned=*/false);
   } catch (const std::exception&) {
      // Fall through to the ffmpeg decode below.
   }

   // 2. Fallback: ffmpeg-decode to a temporary 48 kHz mono PCM16 WAV. We keep a
   //    flat 48 kHz (not the source rate) so the decode is uniform and simply
   //    "readable" — the consumer resamples from there.
   const std::string temp =
      ffmpegDecodeToTemp(path, 48000, "pcm_s16le", "decoded", ffmpegPath);
   return AudioSource(std::move(temp), /*owned=*/true);
}

AudioSource AudioSource::decodeToRate(std::string_view inputPath,
                                      uint32_t targetRate,
                                      const std::string& ffmpegPath) {
   // Decode + resample the source straight into a `targetRate` Hz mono float32
   // WAV and read that. Float32 output keeps the model's sample precision (no
   // 16-bit round trip) and is exactly what the reader hands back as floats.
   const std::string tag = "decoded" + std::to_string(targetRate);
   const std::string temp =
      ffmpegDecodeToTemp(inputPath, targetRate, "pcm_f32le", tag, ffmpegPath);
   return AudioSource(std::move(temp), /*owned=*/true);
}

} // namespace libaudio

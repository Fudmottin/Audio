/**
 * @file audioDecode.h
 * @brief Open an audio file, transparently decoding it to a format our reader
 *        (libsndfile) can actually read.
 *
 * `AudioSource` is a small RAII value that resolves *one* input audio path to
 * a path that an `AudioFileReader` (libsndfile) can open:
 *
 *   1. **Probe** the input with `AudioFileReader` first. Most of our sources
 *      (WAV, AIFF, FLAC, and — on a libsndfile built with MP3 support — MP3)
 *      open directly; in that case the *original* path is used and nothing is
 *      written to disk.
 *   2. **Fallback**: if libsndfile cannot open the input, decode it to a
 *      temporary 48 kHz / mono / PCM16 WAV with `ffmpeg` and use that. The
 *      temp file is deleted when the `AudioSource` is destroyed.
 *
 * This is the single place in libaudio that shells out to ffmpeg *for reading*.
 * It is shared by the Tier-2 `BasicPitch` adapter and (via the
 * `AubioTranscriber` in midicapture) the Tier-1 path, so every analyzer decodes
 * the same way and one file never has two different decode strategies.
 *
 * Two entry points:
 *   - `open()` resolves a path libsndfile can read at its *native* rate (the
 *     consumer resamples); it only touches disk for a container libsndfile
 *     cannot read.
 *   - `decodeToRate()` additionally resamples to a specific rate, for a model
 *     that wants a fixed input rate (`BasicPitch` → 22050 Hz mono). This is the
 *     robust in-front-end substitute for an aubio `TemporalProcessor` resample,
 *     which requires `libsamplerate` and the installed aubio is not built with
 *     it (its resampler would otherwise silently return silence).
 *
 * @section audio-decode-tier2 Tier-2 grouping
 *
 * Part of libaudio's Tier-2 layer; gated behind `LIBAUDIO_HAS_TIER2`. (The
 * helper is a plain value with no ONNX dependency — it only needs the audio
 * reader and a path to ffmpeg.)
 */

#ifndef LIBAUDIO_AUDIODECODE_H
#define LIBAUDIO_AUDIODECODE_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace libaudio {

// ============================================================================
// AudioSource — resolves an input audio path to a reader-usable path.
//
// Domain context: not every audio container we are handed (e.g. an MP3 whose
// build lacks the decoder) opens under libsndfile. Rather than make each
// analyzer re-implement "try libsndfile, else ffmpeg-decode to a temp WAV",
// this one RAII value does it and cleans up after itself.
//
// The object is a *view over a file*, not a reader: it holds the resolved path
// plus a flag for whether we created a temp file that must be deleted. It is
// movable, non-copyable (house style), and the destructor unlinks the temp.
// ============================================================================
class AudioSource {
 public:
   // Default state: nothing resolved, nothing owned. Not directly useful; use
   // `open()`. Present so the type is movable into a vector / option slot.
   AudioSource();

   // Resolve `inputPath` to a path an AudioFileReader can open.
   //
   // Tries libsndfile (via a probe AudioFileReader) first; on failure decodes
   // to a temporary 48 kHz mono PCM16 WAV with ffmpeg and owns (and later
   // deletes) that file.
   //
   // @param inputPath   The audio file to open (any container).
   // @param ffmpegPath  Path to the ffmpeg executable for the fallback decode.
   //
   // @return A resolved AudioSource (either the original path or a temp WAV).
   // @throws std::runtime_error if neither libsndfile nor ffmpeg can read it.
   static AudioSource open(std::string_view inputPath,
                           const std::string& ffmpegPath);

   // Decode `inputPath` to a temporary mono float32 WAV at `targetRate` Hz
   // (resampled with ffmpeg) and return an AudioSource owning that file.
   //
   // Use this when an analyzer wants the audio at a fixed sample rate rather
   // than the file's native rate (the `BasicPitch` adapter feeds a 22050 Hz
   // model, so it calls this with 22050). ffmpeg's decoder does a proper
   // anti-aliased multirate resample, so the result is high quality.
   //
   // @param inputPath   The audio file to decode + resample (any container).
   // @param targetRate  Desired output sample rate in Hz (e.g. 22050).
   // @param ffmpegPath  Path to the ffmpeg executable.
   //
   // @return An AudioSource pointing at the `targetRate` Hz mono WAV.
   // @throws std::runtime_error if ffmpeg cannot decode the input.
   static AudioSource decodeToRate(std::string_view inputPath,
                                   uint32_t targetRate,
                                   const std::string& ffmpegPath);

   ~AudioSource();

   AudioSource(const AudioSource&) = delete;
   AudioSource& operator=(const AudioSource&) = delete;

   AudioSource(AudioSource&& other) noexcept;
   AudioSource& operator=(AudioSource&& other) noexcept;

   // The resolved path to read with an AudioFileReader.
   [[nodiscard]] const std::string& path() const;

   // Whether we created a temp file that the destructor must delete.
   [[nodiscard]] bool owned() const;

 private:
   // Build a source from an already-resolved path + ownership flag.
   AudioSource(std::string path, bool owned);

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_AUDIODECODE_H

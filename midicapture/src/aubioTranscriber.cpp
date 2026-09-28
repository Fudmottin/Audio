/**
 * @file aubioTranscriber.cpp
 * @brief Implementation of AubioTranscriber — the aubio engine behind the
 *        `libaudio::Transcriber` port.
 *
 * The whole implementation is Tier-2: it depends on `libaudio::AudioSource`
 * (to turn an arbitrary input path into one libsndfile can open) and on the
 * `libaudio::Transcriber` port (the base class it implements). When the build
 * has no Tier-2, nothing here is compiled.
 */

#ifdef LIBAUDIO_HAS_TIER2

#include <libaudio/audioDecode.h>
#include <libaudio/hir.h>
#include <libaudio/transcriber.h>
#include <midicapture/aubioTranscriber.h>
#include <midicapture/transcriber.h>
#include <stdexcept>
#include <utility>

// ============================================================================
// Impl — just the ffmpeg path.
//
// The adapter deliberately holds *no* engine. The Tier-1 engine is the
// *global* `::Transcriber` (midicapture/transcriber.h), a distinct type from
// the `libaudio::Transcriber` port it is adapted to (they share a name, so
// every reference is qualified: `::` for the engine, `libaudio::` for the
// port). Standard Tier-1 parameters, matching midicapture's documented
// defaults: 2048-sample FFT window, 512 hop, -40 dB silence gate, yinfft,
// 120 BPM. (An `AudioSource` is a plain value held only transiently in
// `transcribe()`, never a member.)
// ============================================================================
struct AubioTranscriber::Impl {
   std::string ffmpegPath;

   explicit Impl(const std::string& ffmpegPath)
      : ffmpegPath(ffmpegPath) {}
};

AubioTranscriber::AubioTranscriber(const std::string& ffmpegPath)
   : impl_(std::make_unique<Impl>(ffmpegPath)) {}

AubioTranscriber::~AubioTranscriber() = default;

AubioTranscriber::AubioTranscriber(AubioTranscriber&& other) noexcept
   : impl_(std::move(other.impl_)) {}

AubioTranscriber&
AubioTranscriber::operator=(AubioTranscriber&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
   }
   return *this;
}

std::string AubioTranscriber::name() const { return "aubio"; }

libaudio::Score AubioTranscriber::transcribe(std::string_view path) const {
   // Resolve the input to a path libsndfile can open (the corpus files are
   // MP3, which libsndfile may or may not read natively depending on the
   // build). `src` owns any temporary decode file and outlives the engine call
   // below, so the engine can open the resolved path safely.
   libaudio::AudioSource src =
      libaudio::AudioSource::open(std::string(path), impl_->ffmpegPath);

   // Build a *fresh* engine for this call rather than reusing one.
   //
   // The engine's internal `Impl` keeps per-file tracking state (the growing
   // note list, the current-note buffers, and the pitch/onset detectors' own
   // buffers) that is *not* reset at the start of `transcribe()`. The engine
   // was designed as one-instance-per-process (midicapture opens a file, runs
   // once, exits), so reusing it across a corpus would leak notes from every
   // prior file into the next. The reference Python harness sidesteps this by
   // spawning a fresh midicapture process per file; we match that exactly by
   // constructing a throwaway engine here. Construction is cheap (14 of them
   // for the corpus) and this keeps the committed Tier-1 engine untouched.
   ::Transcriber engine(2048u, 512u, -40.0f, "yinfft", 120.0);
   // The engine opens the resolved path itself and runs the Tier-1 pipeline.
   return engine.transcribe(src.path());
}

#endif // LIBAUDIO_HAS_TIER2

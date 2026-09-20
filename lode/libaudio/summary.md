# libaudio — Design Document

> A C++20 wrapper around **aubio** for audio-to-MIDI transcription, with optional use of **libsndfile** for file I/O and **rubberband** for time-stretching. This library is the DSP foundation for the `midicapture` module.

---

## 1. Purpose

libaudio provides the **signal processing pipeline** that converts raw PCM audio samples into analysis results (pitch, onsets, beats, note events). It is a thin, modern C++ wrapper around the C library **aubio**, providing:

- RAII resource management (no manual `new_`/`del_` calls)
- A clean C++ interface (no C-style function pointers)
- Integration with the project's **HIR (High-level Instrumentation Representation)** — see `hir.md`
- Optional use of **libsndfile** for audio file I/O
- Optional use of **rubberband** for time-stretching/pitch-shifting

The output of libaudio feeds into the HIR, which then produces both MIDI files and (optionally) LilyPond source.

---

## 2. Dependencies

| Library | Purpose | Homebrew Package | License |
|---|---|---|---|
| **aubio** (0.4.9) | Core DSP: FFT, pitch detection (YIN variants), onset detection, note segmentation, beat tracking, spectral analysis | `aubio` | GPL-3.0 |
| **libsndfile** (1.2.2) | Audio file I/O (AIFF, WAV, FLAC, etc.) | `libsndfile` | LGPL-2.1+ |
| **rubberband** (4.0.0) | Time-stretching and pitch-shifting (optional, for normalization) | `rubberband` | GPL-3.0 + commercial |

### Why aubio?

aubio is the single most relevant open-source library for audio-to-MIDI transcription. It provides:

- **Pitch detection** — YIN, YINfast, YINfft, Schmitt trigger, harmonic comb filters (fcomb, mcomb)
- **Onset detection** — spectral flux, energy, HPS, phase vocoder
- **Note segmentation** — groups onsets into note events with velocity and note-off information
- **Beat tracking** — tempo estimation and beat location
- **Spectral analysis** — FFT, Mel filterbank, MFCC, chroma features, phase vocoder
- **Audio I/O** — reads AIFF, WAV, FLAC, and many other formats (via source/sink abstraction)

aubio uses a predictable C API pattern:
```
new_aubio_foo() → aubio_foo_do() → del_aubio_foo()
```
with optional `get_param()` and `set_param()` for configuration.

This maps naturally to a C++ wrapper with constructors, methods, and properties.

### Why libsndfile?

libsndfile handles audio file I/O (reading and writing). It supports AIFF, WAV, FLAC, OGG, MP3, and many other formats. It's already installed and well-integrated.

### Why rubberband?

rubberband provides high-quality time-stretching and pitch-shifting. Use cases:
- Normalizing recordings to a consistent sample rate
- Pitch-shifting for analysis (e.g., comparing to piano templates)
- Optional: adjusting tempo for beat tracking

### Detailed decisions

See `decisions.md` for the full rationale behind each library choice, the wrapper pattern, and the default parameters.

---

## 3. Architecture

```
Audio/
├── aiffcapture/          # Phase 1: BlackHole → AIFF (COMPLETE)
├── libaudio/             # Phase 0: DSP library (THIS DOCUMENT)
│   ├── include/
│   │   └── libaudio/
│   │       ├── libaudio.h          // Public API (summary header)
│   │       ├── audioFile.h         // Audio file reading (libsndfile)
│   │       ├── fft.h               // FFT via aubio
│   │       ├── pitch.h             // Pitch detection via aubio
│   │       ├── onset.h             // Onset detection via aubio
│   │       ├── beat.h              // Beat tracking via aubio
│   │       ├── notes.h             // Note detection via aubio
│   │       ├── spectral.h          // Spectral analysis (FFT, MFCC, chroma)
│   │       ├── temporal.h          // Resampling, filtering (aubio)
│   │       ├── rubberband.h        // Time-stretching (rubberband)
│   │       └── hir.h               // High-level Instrumentation Representation
│   └── src/
│       ├── audioFile.cpp
│       ├── fft.cpp
│       ├── pitch.cpp
│       ├── onset.cpp
│       ├── beat.cpp
│       ├── notes.cpp
│       ├── spectral.cpp
│       ├── temporal.cpp
│       ├── rubberband.cpp
│       └── hir.cpp
├── midicapture/          # Phase 2: Audio → MIDI (PLANNED)
│   └── src/
│       └── transcriber.cpp  # Orchestrates libaudio analysis → HIR
├── midisheet/            # Phase 3: MIDI → sheet music
├── sheetmidi/            # Phase 4: Sheet music → MIDI
└── lode/                 # Documentation
```

---

## 4. The HIR (High-level Instrumentation Representation)

The HIR is the **intermediate language** between audio analysis and both MIDI and LilyPond output. It's a pure C++ data structure with no dependencies on aubio, Core Audio, or any file format.

See `hir.md` for the complete HIR specification, including:
- `struct Note` — pitch, velocity, timing, channel, sustain
- `struct ControlEvent` — pedals, tempo changes
- `struct Score` — complete score (notes + controls + metadata)
- From HIR to MIDI (Type 1, 480 ticks per quarter note)
- From HIR to LilyPond (notation export)
- Example usage (monophonic piano transcription)

---

## 5. Wrapper Design: C++20 over aubio's C API

aubio uses a C API with a predictable pattern:
```c
aubio_pitchyin_t *o = new_aubio_pitchyin(buf_size);
// ...
void aubio_pitchyin_do(o, input, output);
// ...
void del_aubio_pitchyin(o);
```

The C++ wrapper converts this to RAII with a clean interface:

```cpp
// pitch.h — Pitch detection wrapper

#ifndef LIBAUDIO_PITCH_H
#define LIBAUDIO_PITCH_H

#include <aubio/pitch/pitchyin.h>
#include <memory>
#include <string>

// Forward declarations
struct fvec_t;
struct cvec_t;

class PitchDetector {
public:
   // Create a pitch detector using the YIN algorithm.
   //
   // @param bufSize   FFT window size (e.g., 1024, 2048, 4096)
   // @param tolerance Tolerance parameter for minima selection [default 0.15]
   //                  Lower = more sensitive, higher = less sensitive
   PitchDetector(uint32_t bufSize, float tolerance = 0.15f);

   // Detect pitch from a buffer of audio samples.
   //
   // @param samples  Input audio samples (length must equal bufSize)
   // @return The detected pitch in MIDI note numbers (float),
   //         or 0.0 if no pitch was detected.
   //         Confidence is in [0.0, 1.0].
   std::pair<float, float> detect(const float* samples, uint32_t length);

   // Configure the detection method.
   //
   // Available methods: "yinfft", "yinfast", "fcomb", "schmitt",
   // "default". (Note: "yin" and "mcomb" are cvec-based and not
   // supported by this wrapper — they require pre-computed complex spectra.)
   // See aubio documentation for details.
   void setMethod(std::string_view method);

   // Get the current confidence of the last detection.
   float confidence() const;

   // Get the current confidence threshold.
   float confidenceThreshold() const;

   // Set the confidence threshold (notes below this are ignored).
   void setConfidenceThreshold(float threshold);

   // Get the current method name.
   std::string method() const;

   // Get the window size.
   uint32_t bufSize() const;

   // Get the step size (hop size).
   uint32_t hopSize() const;

   // Set the step size (hop size).
   void setHopSize(uint32_t hopSize);

private:
   // Private implementation — all aubio C API calls are isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

#endif
```

The `Impl` struct hides all aubio C API details:

```cpp
// pitch.cpp

#include <aubio/pitch/pitchyin.h>
#include <aubio/pitch/pitchyinfft.h>
#include <aubio/pitch/pitchfcomb.h>
#include <aubio/pitch/pitchmcomb.h>
#include <aubio/pitch/pitchschmitt.h>
#include <aubio/types.h>
#include <aubio/fvec.h>
#include <aubio/cvec.h>
#include <stdexcept>
#include <unordered_map>

struct PitchDetector::Impl {
   aubio_pitchyin_t* yin = nullptr;
   aubio_pitchyinfft_t* yinfft = nullptr;
   aubio_pitchfcomb_t* fcomb = nullptr;
   aubio_pitchmcomb_t* mcomb = nullptr;
   aubio_pitchschmitt_t* schmitt = nullptr;
   void* active = nullptr;
   std::string currentMethod;
   uint32_t bufSize;
   uint32_t hopSize;
   float confidenceThreshold = 0.0f;
   float lastConfidence = 0.0f;
   fvec_t* inputBuffer = nullptr;
   fvec_t* candsBuffer = nullptr;
};

PitchDetector::PitchDetector(uint32_t bufSize, float tolerance)
   : impl_(std::make_unique<Impl>()) {
   impl_->bufSize = bufSize;
   impl_->hopSize = bufSize / 4;  // Default: 75% overlap
   impl_->currentMethod = "yinfft";

   // Create the default (yinfft) detector.
   // Core Guidelines: aubio 0.4.9 signature: (samplerate, bufSize)
   impl_->yinfft = new_aubio_pitchyinfft(48000, bufSize);
   impl_->active = impl_->yinfft;

   // Allocate buffers.
   impl_->inputBuffer = new_fvec(bufSize);
   impl_->candsBuffer = new_fvec(bufSize);

   // Set tolerance.
   aubio_pitchyinfft_set_tolerance(impl_->yinfft, tolerance);
}

PitchDetector::~PitchDetector() {
   if (impl_->yinfft) del_aubio_pitchyinfft(impl_->yinfft);
   if (impl_->yin) del_aubio_pitchyin(impl_->yin);
   if (impl_->fcomb) del_aubio_pitchfcomb(impl_->fcomb);
   if (impl_->mcomb) del_aubio_pitchmcomb(impl_->mcomb);
   if (impl_->schmitt) del_aubio_pitchschmitt(impl_->schmitt);
   if (impl_->inputBuffer) del_fvec(impl_->inputBuffer);
   if (impl_->candsBuffer) del_fvec(impl_->candsBuffer);
}

std::pair<float, float> PitchDetector::detect(const float* samples, uint32_t length) {
   if (length != impl_->bufSize) {
      throw std::invalid_argument("Sample length must equal buffer size");
   }

   // Copy samples into aubio's fvec_t.
   std::memcpy(impl_->inputBuffer->data, samples,
               impl_->bufSize * sizeof(float));

   // Run detection.
   aubio_pitchyinfft_do(impl_->yinfft, impl_->inputBuffer, impl_->candsBuffer);

   // Extract results.
   float pitch = impl_->candsBuffer->data[0];  // MIDI note (float)
   float confidence = aubio_pitchyinfft_get_confidence(impl_->yinfft);

   impl_->lastConfidence = confidence;

   // Return 0.0 pitch if confidence is below threshold.
   if (confidence < impl_->confidenceThreshold) {
      return {0.0f, confidence};
   }

   return {pitch, confidence};
}

void PitchDetector::setMethod(std::string_view method) {
   // Delete current detector.
   if (impl_->yinfft) del_aubio_pitchyinfft(impl_->yinfft);
   if (impl_->yin) del_aubio_pitchyin(impl_->yin);
   if (impl_->fcomb) del_aubio_pitchfcomb(impl_->fcomb);
   if (impl_->mcomb) del_aubio_pitchmcomb(impl_->mcomb);
   if (impl_->schmitt) del_aubio_pitchschmitt(impl_->schmitt);

   impl_->currentMethod = std::string(method);

   // Create new detector.
   if (method == "yin") {
      impl_->yin = new_aubio_pitchyin(impl_->bufSize);
      impl_->active = impl_->yin;
   } else if (method == "yinfft") {
      impl_->yinfft = new_aubio_pitchyinfft(impl_->bufSize, impl_->hopSize, 0);
      impl_->active = impl_->yinfft;
   } else if (method == "yinfast") {
      impl_->yinfast = new_aubio_pitchyinfast(impl_->bufSize);
      impl_->active = impl_->yinfast;
   } else if (method == "fcomb") {
      impl_->fcomb = new_aubio_pitchfcomb(impl_->bufSize, impl_->hopSize);
      impl_->active = impl_->fcomb;
   } else if (method == "schmitt") {
      impl_->schmitt = new_aubio_pitchschmitt(impl_->bufSize);
      impl_->active = impl_->schmitt;
   } else {
      // Default: YINfft.
      impl_->yinfft = new_aubio_pitchyinfft(48000, impl_->bufSize);
      impl_->active = impl_->yinfft;
   }
}
```

This pattern — `unique_ptr<Impl>` with aubio C API calls inside — is the core design for all libaudio modules. It provides:

1. **RAII** — aubio resources are automatically freed when the C++ object is destroyed
2. **Encapsulation** — the rest of the codebase never sees aubio C types
3. **Swappability** — if aubio's API changes, only the `Impl` needs updating
4. **Testability** — the C++ interface is clean and mockable

### Why Pimpl?

| Benefit | Explanation |
|---|---|
| **RAII** | aubio resources are automatically freed when the C++ object is destroyed (no manual `new_`/`del_` calls) |
| **Encapsulation** | The rest of the codebase never sees aubio C types (no `aubio_pitchyin_t*`, `fvec_t*`, etc.) |
| **Swappability** | If aubio's API changes, only the `Impl` struct needs updating (not every caller) |
| **Testability** | The C++ interface is clean and mockable (no aubio dependencies in tests) |
| **Compile-time** | Header files don't need aubio includes (faster compilation, fewer dependencies) |

### Why Not a Direct C++ Wrapper?

| Approach | Why Not? |
|---|---|
| **Direct C++ wrapper** (no Pimpl) | Header files expose aubio types, breaking encapsulation. Every change to aubio requires recompiling all callers. |
| **Smart pointers to aubio objects** | Exposes aubio types in the public API. Callers need to know about aubio internals. |
| **Function pointers** | Loss of type safety, harder to debug, harder to maintain. |

See `decisions.md` for the full rationale.

---

## 6. Module-by-Module Design

### 6.1 Audio File I/O (`audioFile.h`)

Wraps libsndfile for reading audio files:

```cpp
class AudioFileReader {
public:
   AudioFileReader(std::string_view path);
   ~AudioFileReader();

   // Get file metadata.
   uint32_t sampleRate() const;
   uint32_t channels() const;
   uint32_t totalFrames() const;

   // Read a block of samples (monophonic).
   // @param[out] buffer Output buffer (must be at least hopSize elements).
   // @return Number of frames actually read (may be less than hopSize at EOF).
   uint32_t read(float* buffer, uint32_t hopSize);

   // Read a block of samples (stereo).
   // @param[out] leftOutput Output buffer for left channel.
   // @param[out] rightOutput Output buffer for right channel.
   // @return Number of frames actually read.
   uint32_t readStereo(float* leftOutput, float* rightOutput, uint32_t hopSize);

   // Downmix stereo to mono (average of L and R).
   // @param[out] monoOutput Output buffer.
   // @return Number of frames actually read.
   uint32_t readMono(float* monoOutput, uint32_t hopSize);

   // Seek to a specific frame position.
   void seek(uint32_t frame);

   // Reset to the beginning of the file.
   void reset();

   // Check if the file is still readable (not at EOF).
   bool eof() const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

**aubio alternative**: aubio also has `aubio_source_t` for file reading. However, libsndfile is more general-purpose, better documented, and already installed. We use libsndfile for file I/O and aubio for DSP.

### 6.2 FFT (`fft.h`)

Wraps aubio's FFT for spectral analysis:

```cpp
class FFT {
public:
   // Create an FFT with the given window size (must be a power of 2).
   FFT(uint32_t windowSize);
   ~FFT();

   // Forward FFT: time-domain samples → frequency-domain complex values.
   // @param timeDomain Input buffer (length must equal windowSize).
   // @return std::pair of (magnitude, phase) vectors.
   std::pair<std::vector<float>, std::vector<float>> forward(const float* timeDomain);

   // Inverse FFT: frequency-domain complex values → time-domain samples.
   // @param magnitudes Magnitude spectrum.
   // @param phases Phase spectrum.
   // @return Reconstructed time-domain samples.
   std::vector<float> inverse(const std::vector<float>& magnitudes,
                              const std::vector<float>& phases);

   // Get the FFT window size.
   uint32_t windowSize() const;

   // Get the number of frequency bins (windowSize / 2 + 1).
   uint32_t numBins() const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

### 6.3 Pitch Detection (`pitch.h`)

Already designed above (Section 5). Key features:

- **Multiple algorithms**: YIN, YINfft, YINfast, fcomb, mcomb, Schmitt
- **Confidence scoring**: Each detection returns a confidence value [0.0, 1.0]
- **Configurable tolerance**: Adjust sensitivity of minima selection
- **Returns MIDI pitch**: Pitch is returned as a float in MIDI note numbers (e.g., 60.0 = middle C)
- **Threshold filtering**: Notes below confidence threshold return 0.0 (no pitch)

**Default**: YINfft (fast, accurate, good for piano). See `decisions.md` for the full rationale.

### 6.4 Onset Detection (`onset.h`)

Wraps aubio's onset detection:

```cpp
class OnsetDetector {
public:
   // Create an onset detector.
   //
   // @param method    Detection method: "specflux" (spectral flux, default),
   //                  "energy", "hpsst", "phase", "combs".
   // @param bufSize   FFT window size (e.g., 1024, 2048, 4096).
   // @param hopSize   Step size between frames (hop).
   // @param sampleRate Sample rate of the input signal.
   OnsetDetector(std::string_view method, uint32_t bufSize,
                 uint32_t hopSize, uint32_t sampleRate);

   ~OnsetDetector();

   // Detect onsets in a buffer of audio samples.
   //
   // @param samples Input audio samples (length must equal bufSize).
   // @return true if an onset was detected at this frame, false otherwise.
   bool detect(const float* samples, uint32_t length);

   // Get the timestamp of the last detected onset (in seconds).
   // Returns std::nullopt if no onset was detected.
   std::optional<double> lastOnsetTime() const;

   // Get the confidence of the last onset detection.
   float lastConfidence() const;

   // Set the peak picking threshold (higher = fewer onsets).
   void setThreshold(float threshold);

   // Get the current threshold.
   float threshold() const;

   // Set the minimum time between onsets (in seconds).
   void setMinIoI(double minIoI);

   // Get the minimum time between onsets.
   double minIoI() const;

   // Get the current method name.
   std::string method() const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

**Key insight for piano**: Piano notes have a very characteristic attack — a fast transient followed by exponential decay. Spectral flux (change in spectral envelope between frames) is the most reliable onset signal for piano. The default method is "specflux".

### 6.5 Beat Tracking (`beat.h`)

Wraps aubio's beat tracking:

```cpp
class BeatTracker {
public:
   // Create a beat tracker.
   //
   // @param bufSize    FFT window size.
   // @param hopSize    Step size between frames.
   // @param sampleRate Sample rate of the input signal.
   BeatTracker(uint32_t bufSize, uint32_t hopSize, uint32_t sampleRate);

   ~BeatTracker();

   // Analyze a buffer for beat events.
   //
   // @param samples Input audio samples (length must equal bufSize).
   // @return true if a beat was detected at this frame, false otherwise.
   bool detect(const float* samples, uint32_t length);

   // Get the timestamp of the last detected beat (in seconds).
   std::optional<double> lastBeatTime() const;

   // Get the estimated tempo in BPM.
   double estimatedTempo() const;

   // Get the current tempo estimate (in BPM).
   double currentTempo() const;

   // Get the number of beats detected so far.
   uint32_t beatCount() const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

### 6.6 Note Detection (`notes.h`)

Wraps aubio's note detection (combines onset + pitch + velocity):

```cpp
class NoteDetector {
public:
   // Create a note detector.
   //
   // @param method     Detection method (e.g., "default", "specflux").
   // @param bufSize    FFT window size.
   // @param hopSize    Step size between frames.
   // @param sampleRate Sample rate of the input signal.
   NoteDetector(std::string_view method, uint32_t bufSize,
                uint32_t hopSize, uint32_t sampleRate);

   ~NoteDetector();

   // Detect notes in a buffer of audio samples.
   //
   // @param samples Input audio samples (length must equal bufSize).
   // @return std::optional<NoteEvent> with pitch, velocity, and note-off info,
   //         or std::nullopt if no note was detected.
   std::optional<NoteEvent> detect(const float* samples, uint32_t length);

   // Get the current silence threshold (below this, no note is detected).
   float silenceThreshold() const;

   // Set the silence threshold.
   void setSilenceThreshold(float threshold);

   // Get the minimum time between onsets (in milliseconds).
   double minIoIMs() const;

   // Set the minimum time between onsets (in milliseconds).
   void setMinIoIMs(double ms);

   // Get the note-off release drop level (in dB).
   float releaseDropDb() const;

   // Set the note-off release drop level (in dB).
   // When a new note is found, the current level in dB is measured.
   // If the measured level drops under that initial level - release_drop_level,
   // then a note-off will be emitted.
   void setReleaseDropDb(float db);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

// A note event detected by the NoteDetector.
struct NoteEvent {
   float pitchMidi;    // MIDI note (float), or 0 if no note
   float velocity;     // 0–127 (from aubio's internal analysis)
   float noteOffMidi;  // MIDI note to turn off (from aubio), or 0
};
```

**This is the simplest path to a working transcription system.** aubio's `aubio_notes_t` does onset + pitch + velocity + note-off all in one call. For monophonic piano, this alone could produce a reasonable result.

### 6.7 Spectral Analysis (`spectral.h`)

Wraps aubio's spectral analysis functions:

```cpp
class SpectralAnalyzer {
public:
   // Create a spectral analyzer.
   //
   // @param bufSize    FFT window size.
   // @param hopSize    Step size between frames.
   // @param sampleRate Sample rate of the input signal.
   SpectralAnalyzer(uint32_t bufSize, uint32_t hopSize, uint32_t sampleRate);

   ~SpectralAnalyzer();

   // Compute the spectral centroid (brightness).
   float spectralCentroid(const float* samples);

   // Compute the spectral flux (change in spectral envelope).
   float spectralFlux(const float* prevSpectrum, const float* currSpectrum);

   // Compute the RMS energy of a buffer.
   float rmsEnergy(const float* samples, uint32_t length);

   // Compute the zero-crossing rate.
   float zeroCrossingRate(const float* samples, uint32_t length);

   // Compute the MFCC coefficients (Mel-frequency cepstral coefficients).
   std::vector<float> mfcc(const float* samples);

   // Compute the chroma features (12-bin pitch class distribution).
   std::vector<float> chroma(const float* samples);

   // Compute the spectral roll-off (frequency below which X% of energy is contained).
   float spectralRollOff(const float* samples, float rollOffRatio = 0.85f);

   // Compute the spectral bandwidth.
   float spectralBandwidth(const float* samples);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

**Use cases:**
- **Velocity estimation**: RMS energy and spectral centroid correlate with how hard a piano key was struck.
- **Pedal detection**: Spectral flux and energy decay patterns help detect when sustain pedal is pressed.
- **Polyphonic separation**: Chroma features and harmonic binning help separate simultaneous notes.

### 6.8 Time-Domain Processing (`temporal.h`)

Wraps aubio's temporal processing (resampling, filtering):

```cpp
class TemporalProcessor {
public:
   // Create a temporal processor.
   TemporalProcessor(uint32_t sampleRate);

   ~TemporalProcessor();

   // Resample audio to a different sample rate.
   // @param samples Input samples.
   // @param targetSampleRate Desired sample rate.
   // @return Resampled samples.
   std::vector<float> resample(const std::vector<float>& samples,
                               uint32_t targetSampleRate);

   // Apply a low-pass filter.
   // @param samples Input samples.
   // @param cutoffHz Cutoff frequency in Hz.
   // @return Filtered samples.
   std::vector<float> lowPass(const std::vector<float>& samples,
                              float cutoffHz);

   // Apply a high-pass filter.
   std::vector<float> highPass(const std::vector<float>& samples,
                               float cutoffHz);

   // Apply an A-weighting filter (for perceived loudness).
   std::vector<float> aWeighting(const std::vector<float>& samples);

   // Apply a C-weighting filter.
   std::vector<float> cWeighting(const std::vector<float>& samples);

   // Compute the Biquad filter coefficients.
   std::vector<std::vector<float>> biquadCoefficients(
      std::string_view filterType, float cutoffHz, float q);

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

**Use cases:**
- **Pre-processing**: High-pass filter to remove sub-bass rumble (below A0 = 27.5 Hz).
- **Normalization**: Resample to a consistent sample rate (48 kHz).
- **Mono downmix**: Average stereo channels for analysis.

### 6.9 Time-Stretching / Pitch-Shifting (`rubberband.h`)

Wraps rubberband for time-stretching and pitch-shifting:

```cpp
class RubberbandProcessor {
public:
   // Create a rubberband processor.
   //
   // @param sampleRate Sample rate of the input signal.
   // @param channels   Number of channels (1 = mono, 2 = stereo).
   RubberbandProcessor(uint32_t sampleRate, uint32_t channels = 1);

   ~RubberbandProcessor();

   // Process a block of audio samples.
   // @param samples Input samples.
   // @return Processed samples.
   std::vector<float> process(const std::vector<float>& samples);

   // Set the time-stretch factor (1.0 = no change).
   // > 1.0 = slow down, < 1.0 = speed up.
   void setTimeStretch(float factor);

   // Set the pitch shift (in semitones).
   // Positive = higher, negative = lower.
   void setPitchShift(float semitones);

   // Set the desired tempo (in BPM) for time-stretching.
   void setTempo(float bpm);

   // Flush any remaining samples.
   std::vector<float> flush();

   // Check if there are remaining samples to process.
   bool hasRemaining() const;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};
```

**Use cases:**
- **Normalization**: Adjust tempo to a standard BPM for beat tracking.
- **Pitch-shifting**: For analysis (e.g., comparing to piano templates).
- **Optional**: Time-stretching to match a target duration.

### 6.10 Main Summary Header (`libaudio.h`)

```cpp
// libaudio.h — Public API summary

#ifndef LIBAUDIO_LIBAUDIO_H
#define LIBAUDIO_LIBAUDIO_H

#include "audioFile.h"
#include "fft.h"
#include "pitch.h"
#include "onset.h"
#include "beat.h"
#include "notes.h"
#include "spectral.h"
#include "temporal.h"
#include "rubberband.h"
#include "hir.h"

#endif
```

---

## 7. Usage Example: Monophonic Piano Transcription

```cpp
#include <libaudio/libaudio.h>
#include <iostream>

int main() {
   // Open an AIFF file.
   AudioFileReader reader("recording.aiff");

   // Create analysis objects.
   const uint32_t bufSize = 2048;
   const uint32_t hopSize = bufSize / 4;  // 75% overlap

   NoteDetector noteDetector("default", bufSize, hopSize, reader.sampleRate());

   // Process the audio frame by frame.
   std::vector<float> buffer(hopSize);
   std::vector<Note> notes;

   while (true) {
      uint32_t framesRead = reader.readMono(buffer.data(), hopSize);
      if (framesRead == 0) break;  // EOF

      auto event = noteDetector.detect(buffer.data(), framesRead);
      if (event.has_value()) {
         Note note;
         note.pitch = static_cast<uint8_t>(event->pitchMidi + 0.5f);  // Round
         note.velocity = static_cast<uint8_t>(event->velocity * 127.0f);
         note.startTime = reader.totalFrames() > 0
            ? (reader.totalFrames() - reader.totalFrames() +
               static_cast<double>(reader.totalFrames()) / reader.sampleRate())
            : 0.0;
         // Track frame position for accurate timing.
         notes.push_back(note);
      }
   }

   // Build the HIR Score.
   Score score;
   score.notes = std::move(notes);
   score.tempo = 120.0;  // Default; refine with beat tracking.

   // Export to MIDI.
   // (Done by midicapture::MidiWriter, which takes a Score.)

   return 0;
}
```

---

## 8. Usage Example: Polyphonic Piano Transcription

```cpp
#include <libaudio/libaudio.h>
#include <iostream>

int main() {
   AudioFileReader reader("recording.aiff");

   const uint32_t bufSize = 4096;
   const uint32_t hopSize = bufSize / 4;

   // Use multiple detectors in parallel for polyphonic analysis.
   PitchDetector pitchDetector("yinfft", bufSize);
   OnsetDetector onsetDetector("specflux", bufSize, hopSize, reader.sampleRate());
   SpectralAnalyzer spectral(bufSize, hopSize, reader.sampleRate());

   std::vector<float> buffer(hopSize);
   std::vector<Note> notes;
   std::optional<double> lastOnsetTime;

   while (true) {
      uint32_t framesRead = reader.readMono(buffer.data(), hopSize);
      if (framesRead == 0) break;

      // Detect onsets.
      if (onsetDetector.detect(buffer.data(), framesRead)) {
         lastOnsetTime = reader.totalFrames() > 0
            ? static_cast<double>(reader.totalFrames() - framesRead) / reader.sampleRate()
            : 0.0;

         // At onset, estimate pitch.
         auto [pitch, confidence] = pitchDetector.detect(buffer.data(), framesRead);
         if (confidence > 0.5f && pitch > 20.0f && pitch < 109.0f) {
            Note note;
            note.pitch = static_cast<uint8_t>(pitch + 0.5f);
            note.velocity = static_cast<uint8_t>(
               (spectral.rmsEnergy(buffer.data(), framesRead) / 1.0f) * 127.0f);
            note.startTime = lastOnsetTime.value_or(0.0);
            note.channel = 1;
            note.sustain = false;
            notes.push_back(note);
         }
      }
   }

   // Post-process: estimate note durations, detect pedal, etc.
   // Build Score and export to MIDI.

   return 0;
}
```

---

## 9. Integration with midicapture

The `midicapture` module (Phase 2) will orchestrate libaudio's analysis and produce the HIR:

```
midicapture/
├── include/
│   └── midicapture/
│       ├── transcriber.h       // Orchestrates libaudio → HIR
│       ├── midiWriter.h        // HIR → .mid (Type 1, 480 ticks/qn)
│       └── lilypondWriter.h    // HIR → .ly (optional)
└── src/
    ├── main.cpp                // CLI: midicapture input.aiff -o output.mid
    ├── transcriber.cpp         // Orchestrates libaudio analysis
    ├── midiWriter.cpp          // Writes HIR to .mid
    └── lilypondWriter.cpp      // Writes HIR to .ly (optional)
```

The `transcriber` is the brain:

```cpp
class Transcriber {
public:
   // Transcribe an AIFF file to a Score (HIR).
   Score transcribe(std::string_view inputPath);

   // Transcribe with custom parameters.
   Score transcribe(std::string_view inputPath, TranscriptionConfig config);

private:
   // Internal analysis pipeline.
   Score analyzeMonophonic(AudioFileReader& reader);
   Score analyzePolyphonic(AudioFileReader& reader);
   Score detectPedal(const Score& rawNotes, AudioFileReader& reader);
   Score refineDurations(const Score& rawNotes);
   Score estimateTempo(const Score& rawNotes);
};

struct TranscriptionConfig {
   std::string pitchMethod = "yinfft";
   std::string onsetMethod = "specflux";
   uint32_t windowSize = 2048;
   uint32_t hopSize = 512;
   bool polyphonic = false;
   bool detectPedal = false;
   float confidenceThreshold = 0.5f;
   float silenceThreshold = -40.0f;  // dB
   double minNoteDuration = 0.05;    // seconds
   double maxNoteDuration = 10.0;    // seconds
   std::string title = "";
   std::string composer = "";
};
```

---

## 10. Error Handling and Edge Cases

### Audio Quality Issues

| Issue | Impact | Mitigation |
|---|---|---|
| **Low signal-to-noise ratio** | False pitch detections | Increase confidence threshold, use higher window size |
| **Room reverb** | Smears transients, makes onset detection unreliable | Use pre-emphasis filter, increase window size |
| **Stereo recordings** | Phase cancellation in stereo sum | Downmix to mono (average L+R), or analyze each channel separately |
| **Dynamic range** | Soft notes near noise floor are hard to detect | Normalize to [-1.0, 1.0], use adaptive threshold |
| **Non-piano content** | Confuses pitch detection | Use a piano-specific model (filter to 27.5 Hz–4186 Hz) |

### Algorithmic Limitations

| Limitation | Impact | Mitigation |
|---|---|---|
| **Monophonic assumption** | aubio's default note detector assumes one note at a time | Use harmonic binning for polyphony |
| **Harmonic overlap** | Piano harmonics from multiple notes blur frequency analysis | Use template matching or NMF (future) |
| **Pedal + polyphony** | Resonating harmonics from sustained notes blur pitch detection | Detect pedal separately (low-frequency energy analysis) |
| **Note overlap (legato)** | When note A is held while note B starts, when does A end? | Use aubio's note-off detection (release drop level) |
| **Silent passages** | No notes detected during rests | Use onset detection to mark rest boundaries |

---

## 11. Build System (CMake)

```cmake
# CMakeLists.txt — libaudio

cmake_minimum_required(VERSION 3.20)
project(libaudio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dependencies.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUBIO REQUIRED aubio)
pkg_check_modules(SNDFILE REQUIRED sndfile)
pkg_check_modules(RUBBERBAND REQUIRED rubberband)

# libaudio target.
add_library(libaudio
   src/audioFile.cpp
   src/fft.cpp
   src/pitch.cpp
   src/onset.cpp
   src/beat.cpp
   src/notes.cpp
   src/spectral.cpp
   src/temporal.cpp
   src/rubberband.cpp
   src/hir.cpp
)

target_include_directories(libaudio
   PUBLIC include
   PRIVATE ${AUBIO_INCLUDE_DIRS}
           ${SNDFILE_INCLUDE_DIRS}
           ${RUBBERBAND_INCLUDE_DIRS}
)

target_link_libraries(libaudio
   PUBLIC
      ${AUBIO_LIBRARIES}
      ${SNDFILE_LIBRARIES}
      ${RUBBERBAND_LIBRARIES}
   PRIVATE
      ${AUBIO_LDFLAGS}
      ${SNDFILE_LDFLAGS}
      ${RUBBERBAND_LDFLAGS}
)

# Install.
install(TARGETS libaudio
   LIBRARY DESTINATION lib
   ARCHIVE DESTINATION lib
)

install(DIRECTORY include/libaudio DESTINATION include)
```

---

## 12. Testing Strategy

### Unit Tests

| Test | Description |
|---|---|
| **Pitch detection accuracy** | Generate pure sine waves at known frequencies, verify pitch detection matches within ±0.5 semitones |
| **Onset detection** | Generate piano note samples, verify onset detection at note start |
| **Note detection** | Generate monophonic piano recordings, verify note pitch, velocity, and duration |
| **Polyphonic detection** | Generate polyphonic piano recordings, verify multi-note separation |
| **Pedal detection** | Generate piano recordings with/without sustain pedal, verify pedal detection |
| **File I/O** | Read/write AIFF files, verify round-trip fidelity |
| **FFT** | Verify forward/inverse FFT round-trip (reconstruction error < 1e-6) |
| **Velocity estimation** | Generate piano notes at different velocities, verify RMS energy correlates with velocity |

### Integration Tests

| Test | Description |
|---|---|
| **Monophonic transcription** | Record a single-note piano scale, verify all notes detected correctly |
| **Simple polyphony** | Record 2-3 note chords, verify notes detected |
| **Full piano piece** | Record a short piano piece, verify overall accuracy |
| **Pedal + polyphony** | Record a piano piece with sustain pedal, verify pedal detection and note separation |
| **MIDI export** | Transcribe a recording, export to MIDI, play in Logic Pro, verify it sounds correct |
| **LilyPond export** | Transcribe a recording, export to LilyPond, compile to PDF, verify sheet music is readable |

---

## 13. Development Phases

### Phase 0: libaudio (THIS DOCUMENT)

Build the DSP library with:
- Audio file I/O (libsndfile)
- FFT (aubio)
- Pitch detection (aubio: YIN, YINfft, YINfast, fcomb, mcomb, Schmitt)
- Onset detection (aubio: spectral flux, energy, HPS, phase vocoder)
- Beat tracking (aubio)
- Note detection (aubio: combines onset + pitch + velocity + note-off)
- Spectral analysis (aubio: FFT, MFCC, chroma, spectral features)
- Time-domain processing (aubio: resampling, filtering)
- Time-stretching (rubberband, optional)
- HIR data structures (see `hir.md`)

**Deliverable**: A working library that can transcribe monophonic piano recordings to HIR.

### Phase 1: midicapture (Audio → MIDI)

Build the transcription pipeline:
- Audio file reader (wraps libaudio's audioFile)
- Transcriber (orchestrates libaudio analysis → HIR)
- MIDI writer (HIR → .mid, Type 1, 480 ticks/qn)
- Optional: LilyPond writer (HIR → .ly)
- CLI: `midicapture input.aiff -o output.mid`

**Deliverable**: A CLI tool that converts AIFF piano recordings to MIDI files.

### Phase 2: midisheet (MIDI → Sheet Music)

Build the sheet music generation:
- MIDI reader (parses .mid files)
- Note renderer (converts MIDI notes to LilyPond notation)
- Layout engine (handles page breaks, beam grouping, slur placement)
- Output: PDF, SVG, PNG

**Deliverable**: A tool that converts MIDI files to publication-quality sheet music.

### Phase 3: sheetmidi (Sheet Music → MIDI)

Build the sheet music to MIDI conversion:
- LilyPond source parser (parses .ly files)
- Note extractor (converts LilyPond notation to HIR)
- MIDI writer (HIR → .mid)

**Deliverable**: A tool that converts sheet music to MIDI files.

### Phase 4: libaudio extensions

- Polyphonic transcription (harmonic binning, NMF)
- Advanced pedal detection (low-frequency energy analysis)
- Velocity refinement (spectral centroid + attack time)
- Note duration refinement (template matching)
- Real-time transcription (streaming analysis)

---

## 14. Summary

libaudio is the **DSP foundation** for the entire audio-to-MIDI transcription pipeline. It wraps three well-established open-source libraries (aubio, libsndfile, rubberband) in a modern C++20 interface, providing:

- **RAII resource management** — no manual memory management
- **Clean C++ API** — no C-style function pointers
- **Encapsulation** — aubio C API is hidden behind `unique_ptr<Impl>`
- **Swappability** — if aubio's API changes, only the Impl needs updating
- **Testability** — clean interface, easy to mock

The output of libaudio feeds into the HIR (see `hir.md`), which then produces both MIDI files and (optionally) LilyPond source. This is the **single source of truth** for the entire pipeline.

**Key insight**: aubio's `aubio_notes_t` (note detection) alone can produce a reasonable monophonic transcription with onset + pitch + velocity + note-off in a single call. This makes Phase 0 achievable with relatively modest effort. Polyphonic transcription and pedal detection are harder problems that can be added in later phases.

### Known Bugs (2026-09-20)

**`secondsToTicks` formula off by 60×** in `MidiFileWriter::Impl::secondsToTicks()`
(`libaudio/src/midiFileWriter.cpp`). The formula divides by 60 twice,
producing results 60× too small. All MIDI note times are compressed into
the first fraction of a second instead of their correct positions.

See `lode/midicapture/summary.md` for details and fix.

### Cross-References

- **libaudio/decisions.md** — Library choices, wrapper pattern, default parameters
- **libaudio/hir.md** — HIR data structures (Note, ControlEvent, Score)
- **MIDI.md** — MIDI file format (SMF), MIDI writer design
- **LilyPond.md** — LilyPond notation, LilyPond exporter design

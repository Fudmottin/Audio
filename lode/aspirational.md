

Now I have the full picture. Here's what I see:

## Current State

- **aiffcapture** (Phase 1, complete): Captures audio from BlackHole → AIFF. Pure recording, no DSP.
- **Planned**: `midicapture` (audio→MIDI), `midisheet` (MIDI→sheet), `sheetmidi` (sheet→MIDI), `libaudio` (shared DSP library)

## The Core Challenge

Your insight is exactly right: **MIDI is the "machine code" of music**, and you need a **high-level representation (HIR)** that sits between raw audio and both MIDI and LilyPond.

Let me map out how `midicapture` should work.

---

## The Pipeline: Audio → HIR → MIDI + Sheet Music

```
┌──────────────────────────────────────────────────────────────────┐
│                    midicapture Module                            │
│                                                                  │
│  AIFF (PCM samples)                                              │
│       │                                                          │
│       ▼                                                          │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │  libaudio (shared DSP library)                          │     │
│  │                                                         │     │
│  │  1. Pre-processing: de-noise, normalize, resample       │     │
│  │  2. Frame analysis: FFT / CQT / spectral flux           │     │
│  │  3. Note onset detection: transient + spectral flux     │     │
│  │  4. Pitch estimation: YIN + harmonic product spectrum   │     │
│  │  5. Polyphonic separation: harmonic binning / NMF       │     │
│  │  6. Velocity estimation: RMS energy / peak amplitude    │     │
│  │  7. Duration estimation: energy decay / spectral decay  │     │
│  │  8. Pedal detection: low-frequency continuity analysis  │     │
│  └─────────────────────────────────────────────────────────┘     │
│       │                                                          │
│       ▼                                                          │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │  High-level Instrumentation Representation (HIR)        │     │
│  │                                                         │     │
│  │  struct Note {                                          │     │
│  │    double startTime;    // seconds from start           │     │
│  │    double endTime;      // seconds from start           │     │
│  │    uint8_t  pitch;      // MIDI note number (0–127)     │     │
│  │    uint8_t  velocity;   // 0–127 (derived from energy)  │     │
│  │    uint8_t  channel;    // default 1 for piano         │     │
│  │    bool     sustain;    // true if pedal held           │     │
│  │  }                                                      │     │
│  │                                                         │     │
│  │  struct Score {                                         │     │
│  │    std::vector<Note> notes;                             │     │
│  │    std::vector<ControlEvent> controls;                  │     │
│  │    double tempo;                                        │     │
│  │    std::string title;                                   │     │
│  │  }                                                      │     │
│  └─────────────────────────────────────────────────────────┘     │
│       │                                                          │
│       ├──→ MIDI Writer → .mid file → Logic Pro (edit, produce)   │
│       │                                                          │
│       └──→ (optional) LilyPond exporter → .ly → pdf + .mid      │
└──────────────────────────────────────────────────────────────────┘
```

---

## The High-Level Instrumentation Representation (HIR)

This is the key abstraction. It's the **intermediate language** between audio and both MIDI and LilyPond.

```cpp
// hir.h — High-level Instrumentation Representation

#ifndef MIDICAPTURE_HIR_H
#define MIDICAPTURE_HIR_H

#include <cstdint>
#include <string>
#include <vector>

// A single note event
struct Note {
   double startTime;     // seconds from start of recording
   double endTime;       // seconds from start of recording
   uint8_t  pitch;       // MIDI note number (0–127), 21–108 for piano
   uint8_t  velocity;    // 0–127 (derived from RMS energy of note segment)
   uint8_t  channel;     // MIDI channel (default 1 for piano)
   bool     sustain;     // true if this note overlaps with sustain pedal
};

// Control change events (pedals, tempo changes, etc.)
struct ControlEvent {
   double time;          // seconds from start of recording
   uint8_t  controller;  // CC# (64 = sustain, 66 = soft pedal, etc.)
   uint8_t  value;       // 0–127
};

// A complete score
struct Score {
   std::vector<Note>       notes;
   std::vector<ControlEvent> controls;
   double tempo = 120.0;       // BPM (quarter notes per minute)
   std::string title;
   std::string composer;
};

#endif
```

**Why this matters:** From this single representation, you can generate:
- A `.mid` file (MIDI SMF format — already documented in `MIDI.md`)
- A `.ly` file (LilyPond source — documented in `LilyPond.md`)
- A JSON export for editing in a custom tool
- A CSV for spreadsheet analysis

This is the **single source of truth**. The MIDI writer and LilyPond exporter are just **renderers** for the same data.

---

## DSP Pipeline: How to Get from AIFF to HIR

This is the hard part. Here's the breakdown of what each stage needs to do:

### 1. Pre-processing

| Step | Purpose |
|---|---|
| **DC offset removal** | Remove any DC bias from the recording |
| **Normalization** | Scale to [-1.0, 1.0] range (Core Audio already does this) |
| **Resampling** | Convert to a consistent sample rate (48 kHz is fine) |
| **Mono downmix** | Piano is often recorded in stereo; downmix to mono for analysis |
| **High-pass filter** | Remove sub-bass rumble (below ~27.5 Hz = A0, lowest piano key) |

### 2. Frame Analysis

| Technique | Purpose |
|---|---|
| **FFT (Fast Fourier Transform)** | Convert time-domain samples to frequency domain |
| **CQT (Constant-Q Transform)** | Logarithmic frequency spacing (better for music than linear FFT) |
| **Spectral flux** | Measure how much the spectrum changes between frames (onset indicator) |
| **Spectral centroid** | "Brightness" of the sound (correlates with velocity for piano) |

**Frame size**: ~1024–4096 samples (21–85 ms at 48 kHz). Smaller = better timing, worse pitch resolution. For piano, **2048 samples** (~43 ms) is a good compromise.

### 3. Note Onset Detection

| Method | How It Works | Quality |
|---|---|---|
| **Spectral flux** | Compute the difference of spectral envelopes between frames; peaks = onsets | ★★★★☆ |
| **Energy-based** | When RMS energy exceeds a threshold above background | ★★★☆☆ |
| **Phase vocoder** | Detect phase discontinuities (transient changes) | ★★★★☆ |
| **Subband energy** | Energy in specific frequency bands (bass vs. treble) | ★★★☆☆ |
| **Combined** | Spectral flux + energy + phase (best results) | ★★★★★ |

**Key insight for piano**: Piano notes have a very characteristic attack — a fast transient followed by exponential decay. The spectral flux peaks sharply at note onsets. This is the most reliable signal for detecting when notes start.

### 4. Pitch Estimation

| Method | How It Works | Quality |
|---|---|---|
| **Autocorrelation (YIN)** | Find the lag that maximizes self-similarity of the signal | ★★★★★ |
| **Harmonic product spectrum (HPS)** | Multiply downsampled spectra to find fundamental frequency | ★★★★☆ |
| **Cepstral analysis** | FFT → log → IFFT → find peak in "quefrency" domain | ★★★★☆ |
| **Constant-Q + harmonic binning** | Map energy to harmonic series of candidate pitches | ★★★★★ (for polyphony) |

**YIN** is the gold standard for monophonic pitch detection. For piano, it handles the complex harmonics well. For polyphonic (multiple notes at once), you need **harmonic binning** or **non-negative matrix factorization (NMF)**.

### 5. Polyphonic Separation (The Hard Part)

This is where piano transcription gets difficult. When multiple notes are played simultaneously, you need to separate them.

| Method | How It Works | Quality |
|---|---|---|
| **Harmonic binning** | For each candidate pitch, sum energy at its harmonic frequencies (f, 2f, 3f, ...) | ★★★★☆ |
| **Non-negative matrix factorization (NMF)** | Factor the spectrogram into a dictionary of piano note templates and an activation matrix | ★★★★★ |
| **Constant-Q + template matching** | Match the CQT spectrogram to pre-computed piano note templates | ★★★★☆ |
| **Sub-band decomposition** | Split the signal into frequency bands, detect pitch in each band independently | ★★★☆☆ |

**Harmonic binning** is the most practical approach for a first implementation:
1. For each candidate pitch (A0–C8, 88 notes), compute the sum of spectral energy at its fundamental frequency and its harmonics (f, 2f, 3f, 4f, 5f).
2. The pitch with the highest harmonic sum at each time frame is the most likely note being played.
3. For polyphonic transcription, use a **growing** approach: detect the strongest note, subtract its harmonics, then detect the next strongest, etc.

### 6. Velocity Estimation

| Method | How It Works | Quality |
|---|---|---|
| **RMS energy** | Compute RMS of the note segment | ★★★★☆ |
| **Peak amplitude** | Maximum amplitude of the note segment | ★★★☆☆ |
| **Spectral centroid** | Brighter sound = harder strike (correlates with velocity for piano) | ★★★★☆ |
| **Attack time** | Faster attack = harder strike | ★★★☆☆ |
| **Combined** | RMS + spectral centroid (best for piano) | ★★★★★ |

### 7. Duration Estimation

| Method | How It Works | Quality |
|---|---|---|
| **Energy decay** | Note ends when energy drops below a threshold (relative to attack peak) | ★★★★☆ |
| **Spectral decay** | Note ends when harmonics disappear | ★★★☆☆ |
| **Template matching** | Compare to expected piano decay envelope (typically 2–8 seconds depending on pitch) | ★★★★☆ |
| **Silence detection** | Note ends when the signal drops below noise floor for N consecutive frames | ★★★☆☆ |

### 8. Sustain Pedal Detection

| Method | How It Works | Quality |
|---|---|---|
| **Low-frequency energy** | Sustained notes create continuous low-frequency energy even without new onsets | ★★★★☆ |
| **Spectral continuity** | Notes that continue without new onsets are likely sustained | ★★★★☆ |
| **Cross-correlation** | Compare the sustained signal to expected piano decay (without pedal) | ★★★☆☆ |
| **Onset-less energy** | When energy persists after a note's expected decay, pedal is likely held | ★★★★☆ |

**Key insight**: When the sustain pedal is pressed, notes continue to resonate after the key is released. This creates a characteristic pattern: energy in the frequency range of previously-played notes persists beyond their expected decay time. Detecting this pattern is the key to pedal detection.

---

## Implementation Strategy

### Phase 1: Monophonic Transcription (Single Note at a Time)

Start simple. Transcribe recordings where only one note is played at a time:

```
┌─────────────────────────────────────────────────────────┐
│  Phase 1: Monophonic                                    │
│                                                         │
│  AIFF → Pre-processing → FFT → YIN Pitch → Onsets →    │
│  Notes (one at a time)                                  │
│                                                         │
│  Output: Score with monophonic notes                    │
│  Accuracy: ~90% for clear piano recordings              │
└─────────────────────────────────────────────────────────┘
```

### Phase 2: Polyphonic Transcription (Multiple Notes)

Add harmonic binning for polyphonic detection:

```
┌─────────────────────────────────────────────────────────┐
│  Phase 2: Polyphonic                                    │
│                                                         │
│  AIFF → Pre-processing → CQT → Spectral Flux →         │
│  → Onsets → Harmonic Binning (multi-note) →             │
│  → Velocity + Duration → Notes (polyphonic)             │
│                                                         │
│  Output: Score with polyphonic notes                    │
│  Accuracy: ~70–80% for clear piano recordings           │
└─────────────────────────────────────────────────────────┘
```

### Phase 3: Pedal Detection + Refinement

Add sustain pedal detection and refinement:

```
┌─────────────────────────────────────────────────────────┐
│  Phase 3: Full Pipeline                                 │
│                                                         │
│  AIFF → ... → Polyphonic Notes → Pedal Detection →      │
│  → Score (notes + controls)                             │
│                                                         │
│  Output: Score with notes + sustain pedal               │
│  Accuracy: ~60–75% for real piano recordings            │
└─────────────────────────────────────────────────────────┘
```

### Phase 4: Post-Processing (Rule-Based)

Apply rules to fix common errors:
- Remove false positives (spurious short notes below a minimum duration)
- Merge split notes (same pitch, very close in time)
- Adjust velocity based on spectral centroid
- Quantize timing (snap to musical grid if desired)

---

## How This Fits with Your Existing Code

### Integration with aiffcapture

The aiffcapture module already produces AIFF files. The midicapture module would **consume** those AIFF files:

```cpp
// midicapture/main.cpp (conceptual)

#include <aiffcapture/audio_types.h>
#include <midicapture/hir.h>
#include <midicapture/transcriber.h>
#include <midicapture/midiWriter.h>
#include <midicapture/lilypondWriter.h>

int main(int argc, char* argv[]) {
   // Parse arguments: input AIFF file, output MIDI file, optional LilyPond
   auto config = parseArguments(argc, argv);

   // Read the AIFF file (reuse aiffcapture's AIFF reading logic,
   // or convert AIFF to raw PCM using afconvert/ffmpeg)
   auto pcmData = readAiff(config.inputFile);

   // Transcribe to HIR
   Transcriber transcriber;
   Score score = transcriber.transcribe(pcmData);

   // Export to MIDI (Type 1, 480 ticks/quarter note)
   MidiWriter midiWriter;
   midiWriter.write(score, config.outputMidi);

   // Optional: Export to LilyPond source
   if (config.outputLilyPond) {
      LilyPondWriter lyWriter;
      lyWriter.write(score, config.outputLilyPond);
   }

   return 0;
}
```

### libaudio (Shared DSP Library)

The DSP functions (FFT, CQT, YIN, spectral flux, etc.) should live in a shared `libaudio` library that both midicapture and any future audio processing modules can use:

```
Audio/
├── aiffcapture/          # Phase 1: Capture audio to AIFF
├── libaudio/             # Phase 0: Shared DSP library (FFT, CQT, YIN, etc.)
├── midicapture/          # Phase 2: Audio → MIDI transcription
├── midisheet/            # Phase 3: MIDI → sheet music
├── sheetmidi/            # Phase 4: Sheet music → MIDI
└── lode/                 # Documentation
```

`libaudio` would be the foundation — it has no dependencies on Core Audio, AIFF, MIDI, or LilyPond. It just takes PCM samples and produces DSP results.

---

## Practical Considerations for Piano

### What Works Well

| Technique | Quality | Notes |
|---|---|---|
| **Monophonic transcription** (single note) | ★★★★☆ | Very reliable with clean recordings |
| **Simple polyphony** (2–3 notes) | ★★★☆☆ | Possible with harmonic binning |
| **Clear recordings** (close mic, no reverb) | ★★★★☆ | Much better than room recordings |
| **Steady tempo** | ★★★★☆ | Easier to model note decay |

### What's Hard

| Challenge | Why It's Hard |
|---|---|
| **Dense chords** (4+ simultaneous notes) | Harmonics overlap, making pitch separation unreliable |
| **Sustain pedal + polyphony** | Resonating harmonics from multiple notes blur frequency analysis |
| **Room acoustics/reverb** | Reflections smear transients, making onset detection unreliable |
| **Dynamic range** | Soft notes near noise floor are hard to detect |
| **Overlapping notes** (legato playing) | Note A held while note B starts — when does A end? |
| **Non-piano content** | Any non-piano sound in the recording confuses the algorithm |

### Recommendations for Best Results

1. **Record close to the piano** (1–2 feet from the soundboard)
2. **Use a quiet room** (minimal reverb/noise)
3. **Record in mono** (stereo adds complexity without much benefit for transcription)
4. **Avoid the sustain pedal** in initial recordings (pedal detection is the hardest problem)
5. **Play slowly** (less polyphony = easier transcription)
6. **Use a good microphone** (close-mic'd concert grand > room mic)

---

## Architecture Recommendation

```
Audio/
├── aiffcapture/          # Phase 1: BlackHole → AIFF (COMPLETE)
├── libaudio/             # Phase 0: Shared DSP library (NEEDS TO BE BUILT)
│   ├── include/
│   │   └── libaudio/
│   │       ├── fft.h           // FFT / IFFT
│   │       ├── cqt.h           // Constant-Q Transform
│   │       ├── yin.h           // YIN pitch detection
│   │       ├── spectralFlux.h  // Spectral flux computation
│   │       ├── harmonicBinning.h // Polyphonic pitch separation
│   │       ├── onsetDetector.h // Note onset detection
│   │       └── velocityEstimator.h // Velocity from energy
│   └── src/
│       ├── fft.cpp
│       ├── cqt.cpp
│       ├── yin.cpp
│       └── ...
├── midicapture/          # Phase 2: Audio → MIDI (PLANNED)
│   ├── include/
│   │   └── midicapture/
│   │       ├── hir.h             // High-level Instrumentation Representation
│   │       ├── score.h           // Score container
│   │       ├── transcriber.h     // Orchestrates the DSP pipeline
│   │       ├── midiWriter.h      // HIR → .mid (Type 1, 480 ticks/qn)
│   │       └── lilypondWriter.h  // HIR → .ly (optional)
│   └── src/
│       ├── main.cpp
│       ├── transcriber.cpp
│       ├── midiWriter.cpp
│       └── lilypondWriter.cpp
├── midisheet/            # Phase 3: MIDI → sheet music
├── sheetmidi/            # Phase 4: Sheet music → MIDI
└── lode/                 # Documentation (MIDI.md, LilyPond.md, summary.md, etc.)
```

---

## Summary of Your Vision

You're proposing a pipeline:

```
Audio Recording (piano)
       │
       ▼
  aiffcapture → AIFF file (PCM)
       │
       ▼
  libaudio (DSP pipeline)
  ┌─────────────────────────────────────┐
  │  Pre-processing → Frame analysis    │
  │  → Onset detection → Pitch est.     │
  │  → Polyphonic separation → Velocity │
  │  → Duration → Pedal detection       │
  └─────────────────────────────────────┘
       │
       ▼
  HIR (High-level Instrumentation Representation)
  ┌─────────────────────────────────────┐
  │  struct Score {                      │
  │    std::vector<Note> notes;          │
  │    std::vector<ControlEvent> controls│
  │    double tempo;                     │
  │  }                                  │
  └─────────────────────────────────────┘
       │
       ├──→ MIDI Writer → .mid → Logic Pro
       │
       └──→ (optional) LilyPond Writer → .ly → pdf
```

The **HIR is the key innovation** — it's the "high-level language" that both MIDI and LilyPond can consume. MIDI takes precedence as the primary target; sheet music is a secondary export.

This is a **very ambitious but achievable** project. The first release (monophonic transcription of clear piano recordings) could be reasonably accurate. Polyphonic transcription with pedal detection is much harder and would be Phase 2–3.


# libaudio — Design Decisions

> Key design decisions for the libaudio library: library choices, wrapper pattern, and default parameters.

---

## 1. Why aubio?

### The Core Choice

**aubio** was selected as the primary DSP library over building from scratch or using alternatives (Essentia, KISS-FFT, etc.) for the following reasons:

### aubio vs. Building from Scratch

| Factor | aubio | From Scratch |
|---|---|---|
| **Development time** | Weeks (use existing algorithms) | Months (implement, test, verify) |
| **Algorithm quality** | Research-grade (YIN, HPS, spectral flux) | Likely inferior without DSP expertise |
| **Maintenance** | Upstream maintains bugs, improvements | We maintain everything |
| **Testing** | Extensive existing test suite | We write all tests |
| **Risk** | Low (stable API, long history) | High (untested algorithms) |

**Decision**: Use aubio. The time saved (months of DSP development) far outweighs the cost of wrapping a C API.

### aubio vs. Essentia

| Factor | aubio | Essentia |
|---|---|---|
| **Scope** | Focused (pitch, onsets, beats, notes) | Comprehensive (every audio analysis imaginable) |
| **Size** | ~200 KB source, small dependency tree | ~5 MB source, large dependency tree (boost, etc.) |
| **Build time** | Minutes | Minutes to hours (depending on dependencies) |
| **API** | C API (simple, predictable) | C++ API (complex, many classes) |
| **License** | GPL-3.0 | GPL-3.0 |

**Decision**: aubio is more focused and lighter-weight. Essentia's comprehensiveness is unnecessary for our use case and adds build complexity.

### aubio vs. Other Alternatives

| Library | Why Not? |
|---|---|
| **KissFFT** | Only does FFT. We need pitch, onsets, beats, notes. |
| **libsndfile** | Only does file I/O. We need DSP. |
| **rubberband** | Only does time-stretching/pitch-shifting. We need analysis. |
| **rtcmix** | Too heavy, custom language, not a library. |
| **librosa** | Python only. We need C++. |
| **aubio-cffi** | Python bindings to aubio. We need C++. |
| **JUCE** | Audio framework, not analysis library. Overkill. |
| **PortAudio** | Audio I/O, not analysis. Already have libsndfile for I/O. |

---

## 2. Why libsndfile?

### The Core Choice

**libsndfile** was selected for audio file I/O (reading and writing) because:

- **Format support**: AIFF, WAV, FLAC, OGG, MP3, and many others
- **Stability**: Long history, well-tested, widely used
- **Simplicity**: Simple C API, easy to wrap in C++
- **Availability**: Already installed via Homebrew (`brew install libsndfile`)
- **License**: LGPL-2.1+ (compatible with our GPL-3.0 aubio dependency)

### Why Not aubio's source/sink?

aubio also has `aubio_source_t` and `aubio_sink_t` for file I/O. However:

- **libsndfile** is more general-purpose and better documented
- **libsndfile** supports more formats (FLAC, MP3, OGG, etc.)
- **aiffcapture** already uses a custom AIFF writer (Core Audio → AIFF). We don't need aubio's I/O for that module.
- **Separation of concerns**: aubio for DSP, libsndfile for I/O. Clear boundaries.

---

## 3. Why rubberband?

### The Core Choice

**rubberband** was selected for time-stretching and pitch-shifting because:

- **Quality**: High-quality time-stretching and pitch-shifting (used by Qtractor, Ardour, and other DAWs)
- **Simplicity**: Simple C/C++ API, easy to wrap
- **Availability**: Already installed via Homebrew (`brew install rubberband`)
- **License**: GPL-3.0 + commercial (compatible with our GPL-3.0 aubio dependency)

### Optional Use

rubberband is **optional**. It's used for:
- Normalizing recordings to a consistent sample rate
- Pitch-shifting for analysis (e.g., comparing to piano templates)
- Time-stretching to match a target duration

It's not needed for the core transcription pipeline (pitch detection, onset detection, note segmentation).

---

## 4. The Wrapper Pattern: `unique_ptr<Impl>`

### The Core Choice

All libaudio modules use the **Pimpl (Pointer to Implementation) pattern** with `std::unique_ptr<Impl>`:

```cpp
class PitchDetector {
public:
   PitchDetector(uint32_t bufSize, float tolerance = 0.15f);
   std::pair<float, float> detect(const float* samples, uint32_t length);
   // ...
private:
   struct Impl;
   std::unique_ptr<Impl> impl_;  // All aubio calls isolated here
};
```

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

---

## 5. Default Parameters

### Window Size: 2048

| Window Size | Time at 48 kHz | Pitch Resolution | Timing Resolution |
|---|---|---|---|
| 1024 | 21 ms | Good | Good |
| **2048** | **43 ms** | **Good** | **Good** |
| 4096 | 85 ms | Excellent | Poor |

**Decision**: 2048 is the best compromise for piano. It gives ~43 ms per frame, which is fast enough for note timing but accurate enough for pitch detection.

### Hop Size: 512 (75% overlap)

| Hop Size | Overlap | Latency | Smoothing |
|---|---|---|---|
| 1024 (50%) | 50% | Low | Poor |
| **512 (75%)** | **75%** | **Low** | **Good** |
| 256 (87.5%) | 87.5% | Very low | Very good |

**Decision**: 512 (75% overlap) is the best compromise. Higher overlap increases latency and computation without much benefit for piano.

### Pitch Method: YINfft

| Method | Accuracy | Speed | Notes |
|---|---|---|---|
| yin | ★★★★☆ | Slow | Full autocorrelation, most accurate |
| **yinfft** | **★★★★☆** | **Fast** | **FFT-optimized YIN, best tradeoff** |
| yinfast | ★★★☆☆ | Very fast | Approximation, less accurate |
| fcomb | ★★★☆☆ | Fast | Harmonic comb filter |
| mcomb | ★★★★☆ | Medium | Multiple-comb filter (polyphonic) |
| schmitt | ★★☆☆☆ | Very fast | Schmitt trigger (simple, noisy) |

**Decision**: YINfft is the best tradeoff between accuracy and speed for piano. It's the default.

### Confidence Threshold: 0.5

| Threshold | Sensitivity | Notes |
|---|---|---|
| 0.0 | Very high (many false positives) | Detects everything, even noise |
| **0.5** | **Moderate** | **Good balance for piano** |
| 0.7 | Low (few false positives) | Only confident detections |
| 1.0 | None (no detections) | Extremely conservative |

**Decision**: 0.5 is a reasonable default. Users can adjust based on recording quality.

### Silence Threshold: -40 dB

| Threshold | Sensitivity | Notes |
|---|---|---|
| -20 dB | High (misses soft notes) | Only loud notes detected |
| **-40 dB** | **Moderate** | **Good for piano** |
| -60 dB | Low (many false positives) | Detects soft notes and noise |
| -80 dB | Very low (noisy) | Detects everything, including noise |

**Decision**: -40 dB is a reasonable default for piano recordings. Users can adjust based on recording quality.

### Release Drop: 10 dB

| Release Drop | Notes Held | Notes Released | Notes |
|---|---|---|---|
| 0 dB | Very short (notes released immediately) | Very long | Notes held until next note |
| **10 dB** | **Moderate** | **Moderate** | **Good for piano** |
| 20 dB | Short | Long | |
| 100 dB | Very short (notes never released) | Very long | Notes held until next note (no release) |

**Decision**: 10 dB is the default in aubio and is a reasonable starting point for piano.

---

## 6. Summary

| Decision | Value | Rationale |
|---|---|---|
| **DSP library** | aubio | Focused, research-grade algorithms, stable API |
| **File I/O** | libsndfile | General-purpose, well-tested, widely used |
| **Time-stretching** | rubberband (optional) | High-quality, simple API |
| **Wrapper pattern** | `unique_ptr<Impl>` | RAII, encapsulation, swappability, testability |
| **Window size** | 2048 | Best compromise for piano (43 ms at 48 kHz) |
| **Hop size** | 512 (75% overlap) | Best compromise for latency vs. smoothing |
| **Pitch method** | YINfft | Best tradeoff between accuracy and speed |
| **Confidence threshold** | 0.5 | Good balance for piano |
| **Silence threshold** | -40 dB | Good for piano recordings |
| **Release drop** | 10 dB | Reasonable default (aubio default) |

# Plan: In-process FFmpeg decode — link the shared libraries

> **Status: ✅ Implemented.** This plan was approved and executed. The
> [ffmpeg-in-memory.md](ffmpeg-in-memory.md) miniaua plan is superseded.

---

## 1. Motivation & pivot

The prior plan recommended **miniaua** (single-header, public domain). Pivoted to
**FFmpeg shared libraries** because:
- MAESTRO and real-world recordings include AAC/M4A and other formats miniaua lacks.
- libswresample is the *same* resampler the ffmpeg CLI applies — identical audio.
- The user already has `brew install ffmpeg`; the shared libraries are a free ride.

## 2. Goal

Make `AudioSource::decodeToRate` (the BasicPitch front-end) **in-process,
streaming, temp-file-free**:

- **No subprocess** — call the ffmpeg C API directly from libaudio.
- **No temp file** — decode → resample → append to `std::vector<float>` in one pass.
- **Streaming** — `av_read_frame` → `avcodec_receive_frame` → `swr_convert` per frame.
- **Identical audio** — same libswresample engine, same 22050 Hz mono float32.

## 3. Libraries & licensing

| Library | Role |
|---|---|
| `libavformat` | Demux (any container) |
| `libavcodec` | Decode audio frames to raw PCM |
| `libswresample` | Resample + channel downmix |
| `libavutil` | Frame/channel-layout/rational utilities |

All **LGPL 2.1+**. Dynamic linking from Apache-2.0 code is compliant.

## 4. CMake

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
   libavformat libavcodec libswresample libavutil)
target_link_libraries(libaudio PRIVATE ${FFMPEG_LIBRARIES})
target_include_directories(libaudio PRIVATE ${FFMPEG_INCLUDE_DIRS})
```

Required when `LIBAUDIO_ENABLE_TIER2=ON`; optional for Tier-1-only.

## 5. Implementation

New internal unit: `libaudio/src/ffmpegDecode.{h,cpp}`.

```cpp
namespace libaudio::detail {
std::vector<float> decodeToMonoFloat(std::string_view inputPath,
                                     uint32_t targetRate);
}
```

Canonical ffmpeg C API: `avformat_open_input` → `av_find_best_stream` →
`avcodec_open2` → `swr_alloc_set_opts2` → loop (`av_read_frame` →
`avcodec_send_packet` → `avcodec_receive_frame` → `swr_convert`) → flush →
teardown. Output: mono float32 at `targetRate`.

## 6. API changes

| What | Change |
|---|---|
| `AudioSource::decodeToRate(path, rate, ffmpegPath)` | **Removed** |
| `libaudio::decodeToMonoFloat(path, rate)` | **New** (public, in `audioDecode.h`) |
| `AudioSource::open(path, ffmpegPath)` | `ffmpegPath` now optional (default `""`) |
| `BasicPitch(ffmpegPath)` | Constructor parameter **removed** |
| midicapture `--ffmpeg <path>` | **Deprecated** (accepted, ignored, prints notice) |

## 7. Parity gate

14-file corpus: notes must be identical or better (same recall/precision,
same pitches/durations within tolerance). The in-process decode uses the same
libswresample engine, so output should be byte-identical for PCM containers.

**Result: PASSED — byte-identical.** The 14-file corpus run on a pre-change
baseline build (`git archive HEAD`) and on the in-process build produced
identical output (100% recall / 68.2% precision; medians 5.1/11.8/9.7/0.0/0.0
ms), matching the stored pre-change baseline. A real AAC-in-mp4 recording
(Cranberries "Zombie" piano) also transcribes end-to-end in one command
(722 notes; the libsndfile-unopenable container no longer aborts the CLI).

## 8. Non-goals

- Does not change the ONNX model, windowing, post-processor, or tier structure.
- Does not change `AudioSource::open()` behavior (Tier-1 path unchanged).
- Does not address MAESTRO integration (next task).

## 9. Cross-references

- [ffmpeg-in-memory.md](ffmpeg-in-memory.md) — the superseded miniaua plan
- [../libaudio/tier2.md](../libaudio/tier2.md) — the `BasicPitch` front-end
- [../libaudio/summary.md](../libaudio/summary.md) — module overview
- **Source of truth:** `libaudio/src/ffmpegDecode.cpp`

# Audio

Local-first audio processing utilities for macOS, written in C++.

## Currently Available

| Module | Description |
|--------|-------------|
| [`aiffcapture/`](aiffcapture/) | Capture audio from BlackHole 2ch virtual device to uncompressed AIFF files. |
| [`libaudio/`](libaudio/) | DSP library wrapping aubio, libsndfile, rubberband — audio analysis, pitch detection, MIDI export. |
| [`midicapture/`](midicapture/) | Audio → MIDI transcription (monophonic prototype). |

### aiffcapture

A macOS command-line utility that routes audio through the BlackHole virtual device and writes it to an AIFF file. See the [module README](aiffcapture/README.md) for build instructions and usage.

### libaudio

A C++ DSP library providing audio analysis (pitch, onsets, beats, notes), spectral features, and MIDI export. See the [module README](libaudio/README.md) for the full API.

### midicapture

A command-line utility that converts audio recordings (AIFF, WAV, FLAC) to Standard MIDI Files (SMF). Uses aubio for pitch detection (YINfft) and onset detection (spectral flux). See the [module README](midicapture/README.md) for build instructions and usage.

## Build

### aiffcapture

```bash
cd aiffcapture
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Requires macOS, BlackHole 2ch (`brew install blackhole-2ch`), and Xcode Command Line Tools.

### libaudio

```bash
cd libaudio
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Requires macOS, aubio (`brew install aubio`), and libsndfile (`brew install libsndfile`).

### midicapture

```bash
cd midicapture
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Requires macOS, aubio (`brew install aubio`), libsndfile (`brew install libsndfile`), and Boost (`brew install boost`).

## Documentation

- [`lode/`](lode/) — Structured project knowledge (coding practices, terminology, decisions)
- [`aiffcapture/README.md`](aiffcapture/README.md) — Module-specific docs
- [`libaudio/README.md`](libaudio/README.md) — Module-specific docs
- [`midicapture/README.md`](midicapture/README.md) — Module-specific docs


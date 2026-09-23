# Audio

Local-first audio processing utilities for macOS, written in C++.

## Currently Available

| Module | Description |
|--------|-------------|
| [`aiffcapture/`](aiffcapture/) | Capture audio from BlackHole 2ch virtual device to uncompressed AIFF files. |
| [`libaudio/`](libaudio/) | DSP library wrapping aubio, libsndfile, rubberband — audio analysis, pitch detection, MIDI export. |
| [`midicapture/`](midicapture/) | Audio → MIDI transcription (monophonic prototype). |
| [`waterfall/`](waterfall/) | Audio → frequency analysis; renders a SONAR-style waterfall of acoustic energy over time as a scriptable text format. |

### aiffcapture

A macOS command-line utility that routes audio through the BlackHole virtual device and writes it to an AIFF file. See the [module README](aiffcapture/README.md) for build instructions and usage.

### libaudio

A C++ DSP library (in `namespace libaudio`) providing audio analysis (pitch, onsets, beats, notes), spectral features, audio file I/O (via libsndfile), and MIDI export. See the [module README](libaudio/README.md) for the full API.

### midicapture

A command-line utility that converts audio recordings (AIFF, WAV, FLAC) to Standard MIDI Files (SMF). Uses aubio for pitch detection (YINfft) and onset detection (spectral flux). See the [module README](midicapture/README.md) for build instructions and usage.

### waterfall

A command-line utility that reads an audio file and renders a frequency analysis of its acoustic energy over time as a SONAR-style waterfall. It FFTs each row (via libaudio), quantizes the spectrum to 16-bit integers, and prints a header line plus one space-separated hex line per time slice. See the [module README](waterfall/README.md) for the output format, CLI, and build instructions.

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

### waterfall

```bash
cd waterfall
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
- [`waterfall/README.md`](waterfall/README.md) — Module-specific docs


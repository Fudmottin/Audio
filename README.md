# Audio

Local-first audio processing utilities for macOS, written in C++.

## Currently Available

| Module | Description |
|--------|-------------|
| [`aiffcapture/`](aiffcapture/) | Capture audio from BlackHole 2ch virtual device to uncompressed AIFF files. |
| [`libaudio/`](libaudio/) | DSP library wrapping aubio and libsndfile — audio analysis, pitch detection, MIDI export. |
| [`midicapture/`](midicapture/) | Audio → MIDI transcription (polyphonic basic-pitch by default; aubio monophonic as an option). |
| [`waterfall/`](waterfall/) | Audio → frequency analysis; renders a SONAR-style waterfall of acoustic energy over time as a scriptable text format. |

### aiffcapture

A macOS command-line utility that routes audio through the BlackHole virtual device and writes it to an AIFF file. See the [module README](aiffcapture/README.md) for build instructions and usage.

### libaudio

A C++ DSP library (in `namespace libaudio`) providing audio analysis (pitch, onsets, beats, notes), spectral features, audio file I/O (via libsndfile), and MIDI export. See the [module README](libaudio/README.md) for the full API.

### midicapture

A command-line utility that converts audio recordings (AIFF, WAV, FLAC, MP3, …) to Standard MIDI Files (SMF). It selects a transcription engine with `--model`: the polyphonic **basic-pitch** neural engine (ONNX + Core ML, the default in a Tier-2 build) or the monophonic **aubio** engine, both from libaudio. See the [module README](midicapture/README.md) for build instructions and usage.

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

# Tier-1 (default): aubio monophonic engine only.
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Tier-2: adds the polyphonic basic-pitch engine (the default model), --model, --run-corpus.
mkdir ../build-tier2 && cd ../build-tier2
cmake .. -DCMAKE_BUILD_TYPE=Release -DLIBAUDIO_ENABLE_TIER2=ON
cmake --build . --config Release
```

Requires macOS, aubio (`brew install aubio`), libsndfile (`brew install libsndfile`), and Boost (`brew install boost`). The Tier-2 build additionally needs onnxruntime (`brew install onnxruntime`) and ffmpeg (`brew install ffmpeg`).

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


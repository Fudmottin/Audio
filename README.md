# Audio

Local-first audio processing utilities for macOS, written in C++20.

## Currently Available

| Module | Description |
|--------|-------------|
| [`aiffcapture/`](aiffcapture/) | Capture audio from BlackHole 2ch virtual device to uncompressed AIFF files. |
| [`libaudio/`](libaudio/) | DSP library wrapping aubio, libsndfile, rubberband — audio analysis, pitch detection, MIDI export. |

### aiffcapture

A macOS command-line utility that routes audio through the BlackHole virtual device and writes it to an AIFF file. See the [module README](aiffcapture/README.md) for build instructions and usage.

### libaudio

A C++20 DSP library providing audio analysis (pitch, onsets, beats, notes), spectral features, and MIDI export. See the [module README](libaudio/README.md) for the full API.

## Planned (Not Yet Implemented)

These modules are documented in [`lode/summary.md`](lode/summary.md) but have not been started:

- **midicapture** — Audio → MIDI transcription (DSP + AI inference)
- **midisheet** — MIDI → sheet music generation
- **sheetmidi** — Sheet music → MIDI file generation

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

## Documentation

- [`lode/`](lode/) — Structured project knowledge (coding practices, terminology, decisions)
- [`aiffcapture/README.md`](aiffcapture/README.md) — Module-specific docs
- [`libaudio/README.md`](libaudio/README.md) — Module-specific docs
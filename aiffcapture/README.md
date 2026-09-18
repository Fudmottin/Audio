# aiffcapture

A macOS command-line utility that captures audio from the BlackHole virtual audio device and writes it to an uncompressed AIFF file.

## Overview

`aiffcapture` is designed to convert Apple Music tracks (or any audio playing through your Mac) into DRM-free AIFF files. It works by:

1. Enumerating available Core Audio devices on your system.
2. Finding the BlackHole virtual audio device (by name).
3. Opening the device for recording.
4. Reading PCM data from the recording stream in real-time.
5. Writing the PCM data to an AIFF file (uncompressed, stereo).

## Requirements

- **macOS** (Core Audio dependency — not cross-platform)
- **BlackHole 2ch** (virtual audio driver, installed via Homebrew)
- **Xcode Command Line Tools** (for `clang++` and CMake)
- **CMake 3.20+**

## Installation

### 1. Install BlackHole

```bash
brew install blackhole-2ch
```

Then reboot (or restart the audio subsystem) to activate the virtual device.

### 2. Build

```bash
cd aiffcapture
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

The binary will be at `build/bin/aiffcapture`.

## Usage

```bash
# Basic usage (record indefinitely, stop with Ctrl-C):
./build/bin/aiffcapture output.aiff

# Record for a specific duration (in seconds):
./build/bin/aiffcapture --duration 120 output.aiff

# Use a specific device (default: "BlackHole 2ch"):
./build/bin/aiffcapture --device "BlackHole 4ch" output.aiff

# Verbose mode (prints progress to stderr):
./build/bin/aiffcapture --verbose --duration 60 output.aiff

# Show help:
./build/bin/aiffcapture --help
```

### How to Use

1. **Install BlackHole** (if not already installed):
   ```bash
   brew install blackhole-2ch
   ```

2. **Route your audio to BlackHole**:
   - Open the Music app (or any audio source).
   - Play the track you want to capture.
   - Route the audio output to BlackHole 2ch (via Audio MIDI Setup → Multi-Output Device, or by setting BlackHole as the system output).

3. **Run aiffcapture**:
   ```bash
   ./build/bin/aiffcapture --duration 120 output.aiff
   ```
   - Replace `120` with the duration in seconds (or omit for indefinite recording).
   - Replace `output.aiff` with your desired output filename.

4. **Stop recording**:
   - Press `Ctrl-C` to stop recording and close the file.
   - Or wait for the specified duration to elapse.

5. **Use the AIFF file**:
   - The output is a standard AIFF file (uncompressed, stereo, 16-bit signed integer PCM, 32-bit integer sample rate).
   - Convert it to other formats using `afconvert`, `ffmpeg`, or similar tools.

## Project Structure

```
aiffcapture/
├── CMakeLists.txt          # CMake build configuration
├── README.md               # This file
├── .clang-format           # Code formatting configuration
├── include/
│   └── aiffcapture/
│       ├── audio_types.h   # Shared types (AudioFormat, CaptureConfig)
│       ├── device.h        # Device enumeration and BlackHole detection
│       ├── recorder.h      # Recording session management
│       └── aiff.h          # AIFF file format writing
└── src/
    ├── main.cpp            # Entry point, CLI parsing, recording loop
    ├── device.cpp          # Core Audio device enumeration
    ├── recorder.cpp        # Core Audio recording session
    └── aiff.cpp            # AIFF file format writing
```

## Design Principles

- **macOS-specific**: Uses Core Audio, AudioToolbox, and CoreFoundation frameworks.
- **Simple**: No DSP, no signal processing, no model inference. Just records PCM and writes AIFF.
- **Extensible**: The AIFF output can be converted to WAV, MP3, FLAC, etc. using standard tools.

## Future Work

- **Audio-to-MIDI transcription**: The AIFF output can be fed into an audio-to-MIDI converter (the next phase of this project).
- **Cross-platform support**: Replace Core Audio with PortAudio or RtAudio for Linux/Windows support.
- **Additional formats**: Support for WAV, FLAC, or MP3 output (though out of scope for this project).

## License

MIT License (see LICENSE file).

## Acknowledgments

- **BlackHole** by rolandy000 (virtual audio driver for macOS).
- **Core Audio** by Apple (macOS audio framework).
- **AIFF specification** by Apple Computer, Inc.
- **Qwen3.6 35B A3B** vibe coded this project.


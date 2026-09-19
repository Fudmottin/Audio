# aiffcapture — Module Summary

## Purpose

Capture audio from the BlackHole virtual audio device and write it to an uncompressed, DRM-free AIFF file. This is the data-generation step for a future audio-to-MIDI transcription pipeline.

## CLI Interface

```
aiffcapture [options] <output.aiff>
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--duration <seconds>` | 0 (indefinite) | Recording duration. 0 = stop on Ctrl-C. |
| `--device <name>` | "BlackHole 2ch" | Name of the capture device. |
| `--verbose` | off | Print progress and callback info to stderr. |
| `--help` | — | Show usage information. |

### Example

```bash
# Record 5 seconds to test.aiff
./build/bin/aiffcapture --duration 5 test.aiff

# Record with verbose output
./build/bin/aiffcapture --duration 10 --verbose output.aiff
```

## Architecture

```
main.cpp (CLI parsing)
    ↓
DeviceManager (Core Audio device enumeration)
    ↓
Recorder (IO proc-based recording session)
    ↓
AiffWriter (AIFF file format writing)
```

### Core Components

| Component | File | Responsibility |
|-----------|------|----------------|
| `AudioFormat` | `audio_types.h/cpp` | Shared data type: sample rate, channels, bits per sample |
| `DeviceInfo` | `device.h` | Individual device info (ID, name, format) |
| `DeviceManager` | `device.h/cpp` | Enumerate available audio devices |
| `CaptureConfig` | `audio_types.h` | User configuration (output file, device, duration) |
| `Recorder` | `recorder.h/cpp` | IO proc registration, start/stop, callback dispatch |
| `AiffWriter` | `aiff.h/cpp` | AIFF file format writing (FORM, COMM, SSND chunks) |

## Current Status

### What works
- Device enumeration (finds BlackHole 2ch)
- Device format detection (48000 Hz, stereo, 32-bit float)
- IO proc registration and starting
- Audio data flowing through callbacks (verified: ~470 callbacks in 5 seconds)
- **libsndfile writes standard AIFF**: Writes 80-bit extended float sample rate (18-byte COMM), producing files that ffprobe, QuickTime, and afinfo accept directly. **This also fixes playback distortion** — macOS tools correctly parse 80-bit extended float (48,000 Hz) instead of trying to parse 12 bytes as 80-bit extended float (producing garbage values like 30,464 Hz, causing wrong playback speed).
- **Correct duration control**: `--duration 30` produces a 30-second file (verified: ffprobe 29.99s, file size 5.49 MB). Previously half the duration due to libsndfile sample count interpretation.
- **Float-to-int16 conversion**: Verified correct via frame-by-frame comparison — 0 mismatches between AIFF and WAV for both formats.
- **aiff2wav.sh dual-format support**: Handles both our 32-bit integer format (48-byte header) and standard AIFF with 80-bit extended float (54-byte header). Verified with both `clip.aiff` (Audacity, 47,920 frames) and `long-test.aiff` (our capture, 5,473,278 frames) — all frames match (0 mismatches in both).
- **BlackHole 2ch attenuation**: Diagnosed — ~3 dB fixed attenuation, adjustable via System Settings volume slider.

### Known Issues

**macOS tools misread the sample rate.** `afinfo` and `ffprobe` always try to parse 80-bit extended float for the sample rate regardless of COMM chunk size, reporting garbage values. The files are valid per the AIFF spec. Use `aiff2wav.sh` to convert to WAV for playback in QuickTime Player or any other tool.

**BlackHole 2ch applies ~3 dB fixed attenuation.** This is a BlackHole setting, not a code bug. Increase the BlackHole 2ch volume slider in System Settings → Sound → Output to compensate.

## Files

| File | Path | Role |
|------|------|------|
| `CMakeLists.txt` | `aiffcapture/CMakeLists.txt` | CMake build config |
| `.clang-format` | `aiffcapture/.clang-format` | Coding style (3-space indent, Attach braces, 80-col) |
| `include/aiffcapture/audio_types.h` | `aiffcapture/include/aiffcapture/audio_types.h` | AudioFormat, CaptureConfig structs |
| `include/aiffcapture/device.h` | `aiffcapture/include/aiffcapture/device.h` | DeviceInfo, DeviceManager class |
| `include/aiffcapture/recorder.h` | `aiffcapture/include/aiffcapture/recorder.h` | Recorder class |
| `include/aiffcapture/aiff.h` | `aiffcapture/include/aiffcapture/aiff.h` | AiffWriter class |
| `src/main.cpp` | `aiffcapture/src/main.cpp` | Entry point, CLI parsing |
| `src/device.cpp` | `aiffcapture/src/device.cpp` | Core Audio device enumeration |
| `src/recorder.cpp` | `aiffcapture/src/recorder.cpp` | IO proc registration, start/stop |
| `src/aiff.cpp` | `aiffcapture/src/aiff.cpp` | AIFF chunk writing (FORM, COMM, SSND) |
| `src/audio_types.cpp` | `aiffcapture/src/audio_types.cpp` | AudioFormat::toString() |
| `README.md` | `aiffcapture/README.md` | Module documentation |

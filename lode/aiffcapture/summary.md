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
- AIFF file created with correct headers (FORM, COMM, SSND chunks)
- `file` command recognizes output as "AIFF audio"
- **Float-to-int16 conversion**: Verified correct via frame-by-frame comparison of `long-test.aiff` vs `long-test.wav` — all 5,473,278 stereo frames match (0 mismatches)
- **aiff2wav.sh byte-swap**: Verified correct — WAV output is identical to AIFF PCM data after proper byte-swap
- **BlackHole 2ch attenuation**: Diagnosed — ~3 dB fixed attenuation, adjustable via System Settings volume slider

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

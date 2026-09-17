# aiffcapture — Key Decisions

## Decisions Made Jointly with User

1. **Path A (audio capture via virtual device)** — Capture whatever is playing through BlackHole by routing it to a virtual audio device, then recording the PCM stream.

2. **BlackHole 2ch** is the default capture target (installed via `brew install blackhole-2ch`).

3. **C++20 via `clang++`** is the target language. Python was rejected by the user.

4. **Project name:** `aiffcapture` (user approved).

5. **CMake project structure** with canonical layout (user approved).

## Decisions Made by Agent

1. **IO proc-based Core Audio API** instead of deprecated synchronous `AudioDeviceRead` — necessary because macOS 12+ removed `AudioDeviceRead`. Chose callback-based IO proc approach.

2. **Output callback pattern** — The `Recorder` class accepts a `std::function<void(const AudioBufferList*)>` callback that the IO proc invokes on each buffer. Keeps modules decoupled.

3. **`kAudioHardwarePropertyDevices`** used instead of deprecated `kAudioObjectPropertyList`.

4. **`kAudioObjectPropertyElementMain`** used instead of deprecated `kAudioObjectPropertyElementMaster`.

5. **`AudioDeviceCreateIOProcID`** + `AudioDeviceDestroyIOProcID` used instead of `AudioDeviceCreate`/`AudioDeviceDestroy`.

6. **`mIsInterleaved`** removed from `AudioStreamBasicDescription` in newer macOS — hardcoded to `true` for uncompressed AIFF.

7. **10ms `nanosleep` polling loop** in the recording main loop.

8. **80-bit extended float** written per AIFF specification for sample rate in COMM chunk. macOS tools (`afinfo`, `ffprobe`) misread it as a 32-bit integer — this is a known limitation of those tools.

## Bugs Fixed (Historical)

### Bug 1: `findDeviceByName` searched empty `devices_`
- **Cause**: `devices_` was not populated because enumeration happened in a method that wasn't called at the right time.
- **Fix**: Made `devices_` mutable and stored enumeration results properly.

### Bug 2: Core Audio `mData == nullptr` crash
- **Cause**: Attempting to read audio data from a device that hadn't provided a stream format yet.
- **Fix**: Added null check and fallback to `kAudioDevicePropertyStreamFormat`.

### Bug 3: `AudioDeviceStart` called with `nullptr` instead of `ioProcID_`
- **Cause**: The IO proc ID was not being passed correctly to `AudioDeviceStart`.
- **Fix**: Passing the correct IO proc ID returned by `AudioDeviceCreateIOProcID`.

### Bug 4: AIFF header byte order and offset errors
- **Cause**: FORM and SSND placeholder sizes were patched at wrong offsets; multi-byte integers written in native (little-endian) byte order.
- **Fix**:
  - Record file offset **before** writing placeholder (not after).
  - Write all multi-byte integers in **big-endian** byte order (byte-by-byte).
  - Patch COMM chunk's `numSamples` field with actual count on close.
  - Correct FORM size calculation (`46 + bytesWritten_` instead of `12 + 26 + 16 + bytesWritten_`).

### Bug 5: `writeExtendedFloat()` produced invalid 80-bit extended float
- **Cause**: Three sub-bugs:
  1. Exponent written as 8(high)+7(low) instead of 7(high)+8(low).
  2. Loop wrote to `extendedFloat[10]` (out of bounds of 10-byte array), corrupting byte 2.
  3. Wrong significand formula: `intVal << (63 - msbPos)` instead of `(intVal - 2^msbPos) × 2^(64 - msbPos)`.
- **Fix**: Corrected exponent byte order, fixed array indexing, and used the correct fractional-part formula for the 80-bit significand. Added integer-precision path for exact integer sample rates.

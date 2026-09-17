# Practices

## Language & Standards

- **Language**: C++20, compiled with `clang++` (Apple Clang)
- **Standard**: Core Guidelines for C++ compliant
- **Build system**: CMake (canonical directory structure)

## Coding Style

- **Indentation**: 3 spaces (no tabs)
- **Brace style**: Attach (K&R style — braces on same line as control statement)
- **Column limit**: 80 characters
- **Include sorting**: Standard library → project headers → local headers
- **Output**: Prefer `std::cout` / `std::cerr` over `fprintf`
- **Pointers**: No `void*` pointers in our own code. Core Audio's C API may produce them internally, but cast away immediately.

## File Format

Managed by `.clang-format` in the repository root. Run `clang-format -i <file>` before committing.

## Project Structure

```
project/
├── CMakeLists.txt          # Top-level build configuration
├── .clang-format           # Shared coding style
├── README.md               # Project documentation
├── include/
│   └── <project>/
│       └── *.h             # Public headers
├── src/
│   ├── main.cpp            # Entry point (if CLI tool)
│   └── *.cpp               # Implementation files
└── build/                  # CMake build output (gitignored)
```

## Core Audio Development

- Use **IO proc-based** pattern (not deprecated synchronous `AudioDeviceRead`).
- Use `kAudioHardwarePropertyDevices` (not deprecated `kAudioObjectPropertyList`).
- Use `kAudioObjectPropertyElementMain` (not deprecated `kAudioObjectPropertyElementMaster`).
- Use `AudioDeviceCreateIOProcID` + `AudioDeviceDestroyIOProcID` (not deprecated variants).
- `mIsInterleaved` removed from `AudioStreamBasicDescription` in newer macOS — hardcode `true` for uncompressed AIFF.

## AIFF Writing

- Write all multi-byte integers in **big-endian** byte order (byte-by-byte, not `htonl()`).
- Record file offsets **before** writing placeholders, so `finalize()` can patch correct positions.
- The 80-bit extended float in the COMM chunk is **spec-compliant** but macOS tools (`afinfo`, `ffprobe`) misread it as a 32-bit integer. This is a known limitation of those tools, not our code.
- For integer sample rates, compute the 64-bit significand exactly using integer arithmetic (avoids the 53-bit precision limit of `double`).

## Documentation (Lode Coding)

- Every new module gets a lode subsystem summary under `lode/<module>/`.
- Keep comments **human-readable AND LLM-friendly**: explain the *why*, not just the *what*.
- Use Core Guidelines annotations in comments where relevant.
- Handoff documents between sessions should include: task, decisions, status, known bugs, next steps.

## Build & Testing

- CMake project with `bin/` output directory.
- Test with `file`, `ffprobe`, and macOS tools (`afinfo`, `afconvert`).
- BlackHole 2ch must be installed (`brew install blackhole-2ch`) and active (requires reboot after installation).
- Verify audio routing: macOS System Settings → Sound → Output → select BlackHole 2ch.

## Platform

- **Primary**: macOS (Apple Silicon M-series, Xcode CLI tools)
- **Future**: POSIX (Linux, BSD) — portability via reproducing appropriate frameworks.

## Legal / DRM

- This project captures audio playing through BlackHole (user's own audio).
- DRM stripping from Apple Music is a separate concern and should only be used on content the user has a legal right to copy.
- No cloud dependencies. All processing is local.

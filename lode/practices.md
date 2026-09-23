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
- **Third-party include ordering**: Some third-party libraries (aubio) have
  implicit ordering dependencies — `types.h` must be included before other
  headers because it defines `uint_t` and `smpl_t`. Use `// clang-format off`
  / `// clang-format on` around such includes to prevent `SortIncludes: true`
  from reordering them. See `lode/libaudio/decisions.md` for details.
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
- Core Audio outputs **32-bit float** PCM. Convert to 16-bit signed integer before AIFF writing.
- Calculate input frame count from the **source** format (32-bit float = 8 bytes/frame stereo),
  not the output format (16-bit = 4 bytes/frame). Using the wrong bytesPerFrame doubles the
  frame count, reads past the buffer, and produces half-speed, low-pitched garbage.
- **Never call destructors explicitly** (e.g., `recorder.~Recorder()`). The RAII destructor
  runs automatically when the object goes out of scope. Calling it twice is undefined
  behavior — the first call frees resources, the second call frees them again, causing
  a crash. Let the compiler handle cleanup.

## AIFF Writing

- Write all multi-byte integers in **big-endian** byte order (byte-by-byte, not `htonl()`).
- Record file offsets **before** writing placeholders, so `finalize()` can patch correct positions.
- AIFF output uses 16-bit signed integer PCM (CDDA standard) and 32-bit integer sample
  rate encoding. Both are spec-compliant and correctly interpreted by all standard tools.
- Remove the 80-bit extended float encoding — macOS tools (`afinfo`, `ffprobe`) always try
  to parse it regardless of COMM chunk size, producing garbage values (e.g., 30464 Hz).
  The 80-bit float is spec-compliant but practically unusable with these tools.

## Documentation (Lode Coding)

- Every new module gets a lode subsystem summary under `lode/<module>/`.
- Keep comments **human-readable AND LLM-friendly**: explain the *why*, not just the *what*.
- Handoff documents between sessions should include: task, decisions, status, known bugs, next steps.

## Code Documentation (Literate Programming)

This project applies **literate programming principles** through structured
comments in source code. The standalone document
[literate-programming.md](../literate-programming.md) covers Knuth's philosophy,
the toolchain (JavaDoc, Doxygen, TROFF/NROFF), and the principle that source
code is for humans.

Key takeaways from the standalone document:

- **Comments capture the "why"**: domain knowledge, tradeoffs, decisions,
  and reasoning that executable code cannot express.
- **AI-assisted synchronization**: AI tools update comments alongside code
  changes, solving the historical problem of stale comments.
- **Literate programming is philosophy, not toolchain**: apply Knuth's
  principles through structured comments in any language.
- **Source code is for humans**: the compiler is a translator, not the
  audience. The real purpose is communicating intent between humans.

For implementation details (JavaDoc-style markup, comment structure
checklist, what to document), see the standalone document.

## Build & Testing

- CMake project with `bin/` output directory.
- Compiler warnings are treated as **errors**: all CMake targets compile with
  `-Werror` alongside `-Wall -Wextra -Wpedantic -Wconversion
  -Wsign-conversion` (suppress `-Wno-unused-parameter`; Boost-using targets
  also carry `-Wno-pragma-clang-attribute` for third-party header noise).
  New code must build warning-free on Apple Clang.
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
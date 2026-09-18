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
- Use Core Guidelines annotations in comments where relevant.
- Handoff documents between sessions should include: task, decisions, status, known bugs, next steps.

## Code Documentation (Literate Programming)

This project embraces **literate programming principles** in source code comments.
Comments are not an afterthought — they are the externalized memory for a project
where the developer is learning unfamiliar domains (DSP, Core Audio, file formats,
music theory). The code explains the *how*; comments explain the *why*.

### Philosophy

- **Comments capture the "why"**: domain knowledge, tradeoffs, decisions, and
  reasoning that executable code cannot express. The code shows what it does;
  comments explain why it does it that way.
- **Comments are the externalized memory**: for a project where the user is
  learning unfamiliar domains, comments serve as a searchable reference that
  can be revisited months later.
- **AI-assisted synchronization**: since AI tools update comments alongside
  code changes, the synchronization problem that traditionally makes comments
  stale is largely solved. Comments stay current because they are updated
  together with the code.
- **Literate Programming**: Donald Knuth's vision — documentation and code
  are intertwined, with narrative explaining the structure and reasoning.
  This project applies that philosophy through structured comments.

### JavaDoc-Style Markup

Comments use structured markup inspired by JavaDoc and Doxygen. This allows
comments to be extracted into documentation by tools like Doxygen, even if
we don't use Doxygen today. The markup is also highly readable by AI tools.

**Header files** — use `/** ... */` block comments on public interfaces:

```cpp
/**
 * @file module.h
 * @brief Brief description of the module's purpose.
 *
 * Longer narrative explaining the module's role, design rationale,
 * and key decisions. This is the "why" that code alone cannot express.
 *
 * @section section-name Section Title
 *
 * A detailed explanation of a specific aspect:
 * - Why a particular approach was chosen
 * - What tradeoffs were considered
 * - What common pitfalls to avoid
 *
 * @param paramName Description of the parameter.
 * @return Description of the return value.
 * @brief One-line summary (optional).
 * @note Important caveats or gotchas.
 * @see related_file.md — Cross-reference to lode documentation.
 */
```

**Source files** — use `/** ... */` block comments at the top of files and
sections, with `// Core Guidelines:` annotations for specific rules:

```cpp
/**
 * @file module.cpp
 * @brief Implementation of the module described in module.h.
 *
 * Domain context explaining implementation decisions:
 * - Why this pattern was chosen over alternatives
 * - What bugs were fixed and why
 * - What debugging information is available
 *
 * @see lode/terminology.md — Related terminology.
 * @see lode/practices.md — Related coding practices.
 */
```

### Comment Structure Checklist

When writing or reviewing comments, ensure each file has:

1. **File-level narrative** (`@file`, `@brief`): What this file does and why.
2. **Section blocks** (`@section`): Domain-specific explanations for key
   decisions (e.g., why a particular API pattern, why a conversion formula).
3. **Public interface docs** (`@param`, `@return`, `@brief`): On every
   public method in header files.
4. **Domain context notes** (`@note`): Gotchas, pitfalls, common mistakes,
   and the reasoning behind non-obvious code.
5. **Cross-references** (`@see`): Links to relevant lode documentation
   files (terminology.md, practices.md, module-specific docs).
6. **Core Guidelines annotations**: Existing `// Core Guidelines:` comments
   are preserved and coexist with narrative comments.

### What to Document

Priority order for adding domain context comments:

1. **API boundary decisions** — Why a particular function signature or
   class design was chosen.
2. **Format/protocol decisions** — Why a specific encoding, byte order,
   or format was selected (e.g., 32-bit integer vs 80-bit extended float).
3. **Bug fixes** — What the bug was, why it happened, and why the fix
   works (so it doesn't reappear).
4. **Debugging hooks** — What diagnostic values are available and how
   to interpret them (e.g., what ioCallbackCount == 0 means).
5. **Algorithmic choices** — Why one algorithm was chosen over alternatives
   (e.g., YINfft over YIN for piano transcription).

### What NOT to Document

- Trivial code that explains itself (e.g., `i++` — increment i).
- Core Guidelines annotations duplicated in narrative comments.
- Code that is about to be deleted.
- API documentation that is already in the lode markdown files
  (avoid duplication between lode docs and code comments).

### Example: Float-to-Int16 Conversion

```cpp
// Domain context: The clamping and scaling process:
// 1. std::max(-1.0f, ...) clamps negative values (handles no overflow).
// 2. std::min(1.0f, ...) clamps positive values (prevents overflow).
// 3. Multiply by 32767.0f (not 32768.0f) — this is critical.
// 4. Cast to int16_t — this truncates the float to integer.
//
// Why 32767? int16_t ranges from -32768 to +32767 (asymmetric
// in two's complement). If we scaled to 32768, a float of 1.0
// would produce 32768, which overflows to -32768 (a loud,
// distorted sample). Using 32767 avoids this well-known gotcha.
```

This comment explains the *why* (asymmetric two's complement range),
the *what* (clamping and scaling steps), and the *gotcha* (32767 vs
32768) that a beginner would need to know.

### Reference Implementation

The `aiffcapture` module (commit `79998b0`) is the reference implementation
for literate programming comments. Every source file in that module
(4 headers, 4 implementations) follows this pattern. Use it as a template
when creating new modules.

### Verification Checklist

When reviewing new code for literate programming compliance, check:

- [ ] File has a `@file` / `@brief` narrative explaining what it does and why
- [ ] Header files have `@section` blocks for domain-specific decisions
- [ ] Every public method in header files has `@param` and `@return`
- [ ] Critical decisions have `@note` explaining gotchas and pitfalls
- [ ] Cross-references (`@see`) link to relevant lode documentation
- [ ] Existing `// Core Guidelines:` annotations are preserved
- [ ] No trivial self-explanatory code is commented (e.g., `i++`)
- [ ] No duplication between code comments and lode markdown files

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
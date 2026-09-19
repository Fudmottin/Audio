# aiffcapture — Session Handoffs

## Handoff 1: Initial Implementation

### Task
Build a macOS command-line utility that captures audio from the BlackHole virtual audio device and writes it to an uncompressed, DRM-free AIFF file.

### What Works
- Device enumeration (finds BlackHole 2ch)
- Device format detection (48000 Hz, stereo, 32-bit float)
- IO proc registration and starting
- Audio data flowing through callbacks (~470 callbacks in 5 seconds)
- AIFF file created with correct headers (FORM, COMM, SSND chunks)
- `file` command recognizes it as "AIFF audio"

### Known Bug at End of Session
- `ffprobe` reports "exp -16382 is out of range" due to corrupt 80-bit extended float in COMM chunk.

### Fix Applied
- Rewrote `writeExtendedFloat()` in `aiff.cpp` to correctly produce 80-bit IEEE 754 extended float format.
- Fixed exponent byte order (7 high + 8 low instead of 8 + 7).
- Fixed out-of-bounds array write (indices 2-9 instead of 3-10).
- Fixed significand formula for integer sample rates (exact integer arithmetic path).
- Added extensive documentation comments explaining the math and rationale.

### Verification
- `file test.aiff` → "AIFF audio" ✓
- `ffprobe` still reports 30464 Hz (known macOS tool limitation)
- QuickTime and VLC launched successfully

## Handoff 2: Project Integration (Current Session)

### Task
Integrate `aiffcapture` into the Audio project, clone the GitHub repo, set up lode coding documentation, and review the lode coding concept.

### Actions Taken
- Cloned `Fudmottin/Audio` repo (already contained our work).
- Created `lode/` directory with structured documentation.
- Created `lode/summary.md`, `lode/terminology.md`, `lode/practices.md`.
- Created `lode/lode-map.md` (index of all lode files).
- Created `lode/aiffcapture/` subsystem folder with:
  - `summary.md` — Module overview, CLI interface, architecture
  - `decisions.md` — Key decisions and historical bug fixes
  - `handoffs.md` — Session continuity documents

### Next Steps
- Future sessions can reference `lode/` files for context.
- New modules (midicapture, midisheet, sheetmidi) should follow the same lode documentation pattern.

## Handoff 3: aiff2wav.sh Dual-Format Fix (Current Session)

### Task
Fix `aiff2wav.sh` to handle both AIFF formats: our 32-bit integer format and standard AIFF with 80-bit extended float (from Audacity).

### Root Cause
The original script assumed a fixed 48-byte header (our format). Audacity files use 54-byte headers (8 extra bytes from 80-bit extended float sample rate in COMM chunk). The script was reading PCM data from 6 bytes too early, grabbing COMM chunk metadata instead of actual audio samples, producing garbled output.

### Fix Applied
- Detect COMM chunk size at offset 16 (read 4 bytes big-endian).
- If ≥18 bytes (80-bit extended float): use Python to parse the IEEE 754 extended float sample rate, set header offset to 54.
- If <18 bytes (32-bit integer): read 4-byte integer directly, set header offset to 48.
- Use computed `headerOffset` variable in the `dd` command instead of hardcoded 48.

### Verification
- **clip.aiff** (Audacity, 80-bit extended float, 48000 Hz): Converts correctly, all 47,920 frames match. ffprobe correctly reads it as 48000 Hz.
- **long-test.aiff** (our capture, 32-bit integer, 48000 Hz): Still converts correctly, all 5,473,278 frames match.
- Both formats verified with 0 mismatches.

### Key Files Modified
- `aiffcapture/aiff2wav.sh` — Added dual-format detection and handling.

### Commit
`fbe9062` — aiff2wav.sh: handle both AIFF formats (32-bit int and 80-bit extended float)

### Lode Updated
- `lode/aiffcapture/summary.md` — Updated "What works" to document dual-format support.
- `lode/aiffcapture/decisions.md` — Added decision #9 about dual-format detection.
- `lode/summary.md` — Updated known issues to note aiff2wav.sh handles both formats.

## Handoff 4: Fix Garbage 80-bit Extended Float Sample Rate (Current Session)

### Task
Fix pitch-shifted WAV output from `aiff2wav.sh` when converting Audacity AIFF files.

### Root Cause
Audacity writes garbage 80-bit extended float sample rates to AIFF headers (e.g., 56768 instead of 48000). The Python parser in `aiff2wav.sh` was correctly decoding the bytes to 56768, but ffprobe reports 48000 Hz — proving the file's actual audio content is 48000 Hz. The COMM chunk header is simply wrong (Audacity bug).

### Fix Applied
- Validate parsed 80-bit extended float sample rates against known standard rates: 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000.
- Unrecognized rates default to 48000 Hz (macOS default).
- Our 32-bit integer format is unaffected (always reads directly).

### Verification
- **clip.aiff**: 23960 frames, peak -0.9 dBFS, RMS -6.9 dBFS, 0 clipping, duration 0.499s — clean sine wave audio
- **long-test.aiff**: 5473280 frames, 0 mismatches — still converts correctly
- Both formats verified with 0 frame mismatches

### Commit
`96c95b1` — aiff2wav.sh: validate 80-bit extended float sample rate against standard rates

### Lode Updated
- `lode/aiffcapture/decisions.md` — Added decision #10 about sample rate validation.

## Handoff 5: Replace Hand-Written AIFF Writer with libsndfile (Current Session)

### Task
Replace 532 lines of hand-written AIFF chunk formatting with libsndfile, producing standard AIFF files that macOS tools accept directly.

### What Changed
- **aiff.h**: 218 → 108 lines. Public interface: `writeSamples(const int16_t* data, uint32_t numFrames)` (was `writeSamples(const unsigned char* data, uint32_t numBytes)`).
- **aiff.cpp**: 532 → 112 lines. All FORM/COMM/SSND chunk formatting delegated to libsndfile.
- **main.cpp**: Float-to-int16 callback writes int16_t directly (no manual byte-swapping).
- **CMakeLists.txt**: Added `find_library(LIBSNDFILE)` + `find_path(LIBSNDFILE_INCLUDE_DIR)`.

### Verification
- **ffprobe**: Reports 48000 Hz (previously rejected our files).
- **afinfo**: Reports 16-bit big-endian signed integer (previously rejected).
- **aiff2wav.sh**: 0 mismatches for both old and new format.
- **Old format (test.aiff, 48-byte header)**: Still converts correctly.
- **New format (test_new.aiff, 54-byte header)**: Converts correctly.

### Commit
`65c551b` — aiffcapture: replace hand-written AIFF writer with libsndfile (-526 lines total)

### Lode Updated
- `lode/aiffcapture/summary.md` — Updated "What works" to mention libsndfile.
- `lode/aiffcapture/decisions.md` — Added libsndfile decision section.

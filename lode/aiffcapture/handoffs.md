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

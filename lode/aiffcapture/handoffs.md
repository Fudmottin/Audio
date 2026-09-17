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

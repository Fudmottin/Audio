# midicapture — Design Document

> Audio-to-MIDI transcription module. Converts audio recordings (AIFF, WAV, FLAC) to Standard MIDI Files (SMF).

---

## 1. Purpose

`midicapture` is a command-line utility that analyzes audio recordings and generates **Type 1 MIDI files** (480 ticks per quarter note), compatible with Apple Logic Pro. It is the second phase of the Audio project.

The module uses:
- **aubio** (via libaudio): Pitch detection (YINfft), onset detection (spectral flux)
- **libsndfile** (via libaudio): Audio file I/O (AIFF, WAV, FLAC, etc.)
- **Boost program_options**: Command-line argument parsing

---

## 2. Architecture

```
Audio/
├── libaudio/             # DSP library (pitch, onsets, MIDI writing)
│   ├── include/libaudio/
│   │   ├── pitch.h       # PitchDetector (YINfft, YINfast, fcomb, Schmitt)
│   │   ├── onset.h       # OnsetDetector (specflux, energy, hpsst, phase, combs)
│   │   ├── audioFile.h   # AudioFileReader (libsndfile wrapper)
│   │   ├── midiFileWriter.h  # MidiFileWriter (HIR → SMF)
│   │   └── hir.h         # Note, ControlEvent, Score (HIR)
│   └── src/             # Implementation files
├── midicapture/          # Phase 2: Audio → MIDI
│   ├── CMakeLists.txt   # Build config (libaudio, Boost)
│   ├── include/midicapture/
│   │   ├── transcriber.h         # High-level transcription API
│   │   └── audioFile.h           # Audio file reading (libsndfile)
│   └── src/
│       ├── main.cpp              # CLI entry point (Boost program_options)
│       ├── transcriber.cpp       # Transcription pipeline (pitch + onset)
│       └── audioFile.cpp         # Audio file I/O (libsndfile)
└── lode/midicapture/    # Module documentation
```

---

## 3. Transcription Pipeline

```mermaid
graph LR
    A[Input Audio File] --> B[AudioFile]
    B --> C{Silent?}
    C -->|Yes| D[End current note]
    C -->|No| E[PitchDetector YINfft]
    E --> F{Confident?}
    F -->|No| D
    F -->|Yes| G[OnsetDetector specflux]
    G --> H{Onset?}
    H -->|No| I[Continue current note]
    H -->|Yes| J{Playing?}
    J -->|No| K[Start new note]
    J -->|Yes| I
    K --> L[Add to Score.notes]
    I --> L
    D --> L
    L --> M{EOF?}
    M -->|No| B
    M -->|Yes| N[Score → MidiFileWriter]
    N --> O[Type 1 MIDI File]
```

---

## 4. State Machine (Monophonic)

The monophonic prototype uses a simple two-state machine:

| State | Condition | Action |
|-------|-----------|--------|
| **IDLE** | Confidence < threshold | Wait for onset |
| **PLAYING** | Confidence ≥ threshold | Track note, watch for note-off |

Transitions:
- **IDLE → PLAYING**: Confidence ≥ threshold AND onset detected
- **PLAYING → IDLE**: Confidence < threshold (note-off) or new onset

---

## 5. Key Design Decisions

| Decision | Value | Rationale |
|----------|-------|-----------|
| **Pitch method** | YINfft (default) | Best accuracy/speed tradeoff for piano |
| **Onset method** | Spectral flux | Most reliable for piano transients |
| **Window size** | 2048 (configurable) | 43 ms at 48 kHz, good balance |
| **Hop size** | 512 (75% overlap) | Good latency vs. smoothing |
| **Confidence threshold** | 0.5 (configurable) | Moderate sensitivity for piano |
| **Silence threshold** | -40 dB (configurable) | Filters out background noise |
| **MIDI format** | Type 1, 480 ticks/qn | Logic Pro compatible |
| **CLI library** | Boost program_options | Standard, robust, extensible |

---

## 6. CLI Interface

An input file is required, **except** in `--test` mode (which needs none).
When `--input` is given without `--output`, the output path defaults to
`<input>.mid` (extension replaced); in `--test` mode with no input it
defaults to `midicapture-test.mid`.

`--help` prints a POSIX-style help message with a `Usage:` line and exits — no
input file required.

```
midicapture — audio-to-MIDI transcription

Usage: ./midicapture [options] <input.aiff> [output.mid]

Main options:
  -h [ --help ]             Print usage information.
  -i [ --input ] arg        Input audio file path (AIFF, WAV, FLAC, etc.).
  -o [ --output ] arg       Output MIDI file path (.mid).
  --window-size arg (=2048) FFT window size (power of 2, default: 2048).
  --hop-size arg (=512)     Hop size between frames (default: 512).
  --confidence arg (=0.5)   Pitch detection confidence threshold (0.0–1.0,
                            default: 0.5).
  --silence arg (=-40)      Silence threshold in dB (default: -40).
  --tempo arg (=120)        Tempo in BPM (default: 120).
  --method arg (=yinfft)    Pitch detection method (default: "yinfft").
  -t [ --test ]             Sanity test: write a single middle-C note
                            (C4, velocity 100, 1s) regardless of input.
```

Examples:
```bash
midicapture song.aiff                          # → song.mid
midicapture song.aiff output.mid               # explicit output
midicapture -i song.aiff                       # → song.mid
midicapture --input song.aiff --output out.mid # explicit output
midicapture --help                             # usage only
midicapture --test song.aiff                   # fixed sanity note -> song.mid
midicapture -t --output sanity.mid             # sanity note, no input needed
```

---

## 7. Build

```bash
cd midicapture
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Requires: aubio, libsndfile, Boost (program_options).

---

## 8. Validation & Known Issues

The writer is **validated, not a bug** — see [writer.md](writer.md). What
remains open is transcription quality.

### Validation toolchain (decided: not ffprobe)

`ffprobe` is **not** authoritative for small MIDI files — it reports
"Invalid data" on valid ones. We validate with:
- **`midicsv <file>`** — parses to CSV; a clean parse = structurally valid.
- **`timidity -Ow out.wav <file>`** — renders audio; `Notes lost totally: 0`
  and a correctly-timed note = semantically valid.

The writer round-trips cleanly through both (53-byte `--test` file;
clean 62-byte real-transcription file on `aiffcapture/final-fantasy.aiff`).

### Open: transcription detects far too few notes (WIP)

Only ~2 notes are detected from a ~30 s Final Fantasy AIFF (C2, A#5),
both with very low velocities. Suspected causes:
- Onset detection (spectral flux) threshold mis-tuned for this recording
- Pitch confidence threshold (0.5) too high
- IDLE/PLAYING state-machine flickering
- Stereo-to-mono downmix quality

This is a **separate** issue from writer validity, which is now solved.

---

## 9. Cross-References

- **libaudio**: `lode/libaudio/summary.md`, `lode/libaudio/decisions.md`, `lode/libaudio/hir.md`
- **MIDI writer**: `lode/midicapture/writer.md` (SMF byte layout, invariants, `--test` flag, midicsv/timidity validation)
- **MIDI format**: `lode/MIDI.md`
- **aiffcapture**: `lode/aiffcapture/summary.md`
- **Session handoff**: `lode/tmp/session-handoff-midicapture-diagnosis.md`
- **Project overview**: `lode/summary.md`

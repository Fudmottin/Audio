# libaudio — High-level Instrumentation Representation (HIR)

> The intermediate data structure between audio analysis and both MIDI and LilyPond output. This is the **single source of truth** for the entire audio-to-MIDI transcription pipeline.

---

## 1. Overview

The **High-level Instrumentation Representation (HIR)** is a pure C++ data structure with no dependencies on aubio, Core Audio, AIFF, MIDI, or LilyPond. It's the **intermediate language** between audio analysis and both MIDI file output and LilyPond source output.

From a single HIR `Score`, you can generate:
- A `.mid` file (MIDI SMF format — documented in `MIDI.md`)
- A `.ly` file (LilyPond source — documented in `LilyPond.md`)
- A JSON export for editing in a custom tool
- A CSV for spreadsheet analysis

The HIR is the **single source of truth**. The MIDI writer and LilyPond exporter are just **renderers** for the same data.

---

## 2. Data Structures

### 2.1 `Note` — A Single Note Event

```cpp
// hir.h — High-level Instrumentation Representation

#ifndef LIBAUDIO_HIR_H
#define LIBAUDIO_HIR_H

#include <cstdint>
#include <string>
#include <vector>

// A single note event — the atomic unit of musical representation.
struct Note {
   double startTime;     // seconds from start of recording
   double endTime;       // seconds from start of recording
   uint8_t  pitch;       // MIDI note number (0–127), 21–108 for piano
   uint8_t  velocity;    // 0–127 (derived from RMS energy of note segment)
   uint8_t  channel;     // MIDI channel (default 1 for piano)
   bool     sustain;     // true if this note overlaps with sustain pedal
};

// Control change events (pedals, tempo changes, etc.)
struct ControlEvent {
   double time;          // seconds from start of recording
   uint8_t  controller;  // CC# (64 = sustain, 66 = soft pedal, etc.)
   uint8_t  value;       // 0–127
};

// A complete score — the output of the transcription pipeline.
struct Score {
   std::vector<Note>       notes;
   std::vector<ControlEvent> controls;
   double tempo = 120.0;       // BPM (quarter notes per minute)
   std::string title;
   std::string composer;
};

#endif
```

### Field Details

#### `Note`

| Field | Type | Description |
|---|---|---|
| `startTime` | `double` | Seconds from start of recording (not MIDI ticks). Makes the representation platform-independent and human-readable. |
| `endTime` | `double` | Seconds from start of recording. |
| `pitch` | `uint8_t` | MIDI note number (0–127). For piano, valid range is 21 (A0) to 108 (C8). |
| `velocity` | `uint8_t` | 0–127. Derived from RMS energy of the note segment during analysis. |
| `channel` | `uint8_t` | MIDI channel (default 1 for piano, which is channel 0 in MIDI). |
| `sustain` | `bool` | True if this note overlaps with sustain pedal. Derived from pedal detection analysis. |

#### `ControlEvent`

| Field | Type | Description |
|---|---|---|
| `time` | `double` | Seconds from start of recording. |
| `controller` | `uint8_t` | MIDI Control Change number (64 = sustain pedal, 66 = soft pedal, etc.). |
| `value` | `uint8_t` | 0–127. Pedal position (0 = up, 127 = down). |

#### `Score`

| Field | Type | Description |
|---|---|---|
| `notes` | `std::vector<Note>` | All detected notes. |
| `controls` | `std::vector<ControlEvent>` | All control change events (pedals, tempo changes, etc.). |
| `tempo` | `double` | BPM (quarter notes per minute). Default 120.0. |
| `title` | `std::string` | Score title. |
| `composer` | `std::string` | Composer name. |

### Key Design Decisions

- **`startTime` and `endTime` are in seconds** (not MIDI ticks). This makes them platform-independent and human-readable. Conversion to MIDI ticks happens in the MIDI writer (using 480 ticks per quarter note, as documented in `MIDI.md`).
- **`pitch` is a `uint8_t`** (MIDI note number 0–127). For piano, valid range is 21–108.
- **`velocity` is a `uint8_t`** (0–127). Derived from RMS energy of the note segment during analysis.
- **`channel` defaults to 1** (MIDI channel 0, Acoustic Grand Piano).
- **`sustain` is a boolean flag** derived from pedal detection analysis.
- **`Score` owns all notes and controls**. It's the single source of truth for both MIDI and LilyPond output.

---

## 3. From HIR to MIDI

The MIDI writer (documented in `MIDI.md`) converts a `Score` to a Type 1 MIDI file:

1. For each `Note`, create a Note On event at `startTime` (converted to MIDI ticks using 480 ticks per quarter note) and a Note Off event at `endTime`.
2. For each `ControlEvent`, create a Control Change event at `time` (converted to MIDI ticks).
3. Set the tempo using a Set Tempo meta event (using the `Score::tempo` field).
4. Set the instrument using a Program Change message (Acoustic Grand Piano = patch 0, channel 0).
5. Include sustain pedal (CC#64) messages from `ControlEvent` entries where `controller == 64`.

The output is a Type 1 MIDI file with 480 ticks per quarter note, compatible with Logic Pro.

---

## 4. From HIR to LilyPond

The LilyPond exporter (documented in `LilyPond.md`) converts a `Score` to LilyPond source:

1. For each `Note`, create a LilyPond note expression with the appropriate pitch, octave, and rhythm (duration derived from `endTime - startTime`).
2. For each `ControlEvent`, create a LilyPond pedal marking or dynamic marking.
3. Set the tempo using a `\tempo` directive.
4. Set the instrument using a `\set Staff.midiInstrument` directive.
5. Compile with `lilypond` to produce PDF, SVG, PNG, and MIDI output.

---

## 5. Example: Monophonic Piano Transcription

```cpp
#include <libaudio/hir.h>
#include <libaudio/libaudio.h>
#include <iostream>

int main() {
   // Open an AIFF file.
   AudioFileReader reader("recording.aiff");

   // Create analysis objects.
   const uint32_t bufSize = 2048;
   const uint32_t hopSize = bufSize / 4;  // 75% overlap

   NoteDetector noteDetector("default", bufSize, hopSize, reader.sampleRate());

   // Process the audio frame by frame.
   std::vector<float> buffer(hopSize);
   std::vector<Note> notes;

   while (true) {
      uint32_t framesRead = reader.readMono(buffer.data(), hopSize);
      if (framesRead == 0) break;  // EOF

      auto event = noteDetector.detect(buffer.data(), framesRead);
      if (event.has_value()) {
         Note note;
         note.pitch = static_cast<uint8_t>(event->pitchMidi + 0.5f);  // Round
         note.velocity = static_cast<uint8_t>(event->velocity * 127.0f);
         note.startTime = static_cast<double>(reader.totalFrames() - framesRead) / reader.sampleRate();
         note.channel = 1;
         note.sustain = false;
         notes.push_back(note);
      }
   }

   // Build the HIR Score.
   Score score;
   score.notes = std::move(notes);
   score.tempo = 120.0;  // Default; refine with beat tracking.

   // Export to MIDI (done by midicapture::MidiWriter).
   // Export to LilyPond (done by midicapture::LilyPondWriter, optional).

   return 0;
}
```

---

## 6. Cross-References

- **MIDI.md** — MIDI file format (SMF), MIDI writer design
- **LilyPond.md** — LilyPond notation, LilyPond exporter design
- **libaudio/summary.md** — DSP library design, module-by-module API
- **libaudio/decisions.md** — Design decisions (why aubio, default parameters)

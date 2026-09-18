# MIDI Reference for Software Development

> A practical reference for generating and consuming Standard MIDI Files (`.mid`) with piano as the primary instrument, targeting Apple Logic Pro compatibility.

---

## 1. What MIDI Is (and Isn't)

**MIDI (Musical Instrument Digital Interface)** is a **control protocol**, not an audio format. There is no sound data in a MIDI file — only instructions telling a sound source what to do:

| MIDI Concept | Analogous to |
|---|---|
| Note On / Note Off | Pressing / releasing a piano key |
| Velocity | How hard you pressed the key |
| Pitch bend | Sliding between notes |
| Program Change | Switching from piano to strings |
| Sustain pedal | Holding notes after releasing keys |

The actual sound comes from a **sound source**: a synthesizer, sampler, or DAW (like Logic Pro). That is why a MIDI file plays as whatever instrument is loaded in the receiving device.

---

## 2. The MIDI Protocol (Real-Time)

The original 1983 spec defines a serial bus (5-pin DIN) and later USB/MIDI over IP variants. The key real-time messages are:

- **Note On** `9n nn vv` — channel `n`, note number `nn`, velocity `vv`
- **Note Off** `8n nn` — same, but velocity is usually ignored (or used for release velocity)
- **Control Change** `Bn cc vv` — e.g., cc#64 = sustain pedal
- **Program Change** `Cn pp` — select instrument patch
- **Pitch Bend** `En ll hh` — 14-bit wheel value

These are **event-based**, time-stamped by relative delta-times. The receiving instrument interprets them and generates sound in real time.

### Message Byte Layout

```
[Status Byte] [Data Byte 1] [Data Byte 2]
   MSB = 1           MSB = 0
```

Status bytes always have the high bit set (`0x80–0xFF`). Data bytes never have the high bit set (`0x00–0x7F`).

---

## 3. General MIDI (GM)

**General MIDI (1991)** is a standardization so that a MIDI file plays reasonably on any GM-compatible device.

### Key GM Specifications

| Category | Detail |
|---|---|
| 128 presets | 8 percussion channels + 120 melodic instruments |
| Channel 10 | Always percussion (drums) |
| 8 voices polyphony | Minimum — most modern synths exceed this |
| Reverb / chorus | Optional effects, cc#91 and cc#93 |
| Bank Select | cc#0 (MSB) + cc#32 (LSB) for extended patches |
| MMC | MIDI Machine Control: Start/Stop/Continue for sync |

### Piano on GM

| GM Patch # | Name | Program Value (0-based) |
|---|---|---|
| 1 | Acoustic Grand Piano | 0 |
| 2 | Bright Acoustic Piano | 1 |
| 3 | Electric Grand Piano | 2 |
| 4 | Honky-tonk Piano | 3 |
| 5 | Rhodes Electric Piano | 4 |
| 6 | Chorused Piano | 5 |
| 7 | Harpsichord | 6 |
| 8 | Clavi | 7 |

For piano-focused MIDI, use **channel 1** (0-indexed: channel 0) with Program Change `#1` (value 0).

### GM Level 1 vs Level 2

| Feature | GM1 | GM2 |
|---|---|---|
| Patches | 128 fixed | 128 + 128 alternate |
| Polyphony | 8 voices | 32 voices |
| Timbre selection | Fixed | User selectable |
| Pitch bend range | Fixed (±2 semitones) | User definable |
| Polyphonic expression | No | Yes (aftertouch) |

Logic Pro does not require GM compliance — it understands raw MIDI natively. GM is useful as a fallback compatibility baseline.

---

## 4. MIDI File Format (SMF — Standard MIDI File)

MIDI files use the **Standard MIDI File** format, defined by the MIDI Manufacturers Association (MMA). The file extension is `.mid`.

### File Types

| Type | Description | Use Case |
|---|---|---|
| **Type 0** | All tracks merged into one | Simple export, some older software |
| **Type 1** | Multiple independent tracks | **Most common; what you want for Logic Pro** |
| **Type 2** | Multiple independent sequences | Rare; multi-song players |

**Use Type 1 for Logic Pro.** It lets you organize separate tracks for piano, bass, drums, etc.

### Binary File Structure

```
+---------------------------+
| Header Chunk              |  "MThd" + length (6 bytes)
|  Format: 0, 1, or 2       |
|  Num tracks: N            |
|  Division: ticks/beat     |  ticks per quarter note
+---------------------------+
| Track 0: "MTrk"           |  variable-length chunk
|  ... events ...           |
+---------------------------+
| Track 1: "MTrk"           |  (if Type 1)
|  ... events ...           |
+---------------------------+
| ...                       |
+---------------------------+
```

### Header Chunk (14 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 bytes | `"MThd"` (ASCII) |
| 4 | 4 bytes | Chunk length = 6 (big-endian uint32) |
| 8 | 2 bytes | Format (0, 1, or 2, big-endian) |
| 10 | 2 bytes | Number of tracks (big-endian) |
| 12 | 2 bytes | Division (big-endian) |

### Division Field (Ticks Per Beat)

This is critical for timing accuracy:

| Division | Meaning | Common Use |
|---|---|---|
| 24 | ticks per quarter note | Old GM/GS devices |
| 48 | ticks per quarter note | Some sequencers |
| 96 | ticks per quarter note | Common |
| **480** | ticks per quarter note | **Logic Pro default** |
| 500 | ticks per quarter note | Some DAWs |
| 1000 | ticks per beat (SMPTE) | Timecode-based |

**For Logic Pro compatibility, use 480 ticks per quarter note.**

### Track Chunk

| Offset | Size | Field |
|---|---|---|
| 0 | 4 bytes | `"MTrk"` (ASCII) |
| 4 | 4 bytes | Chunk length (big-endian uint32) |
| 8 | variable | Event data |

---

## 5. Encoding Details

### Variable-Length Integers

MIDI uses a compact encoding for integers of arbitrary size. Each byte contributes 7 data bits, with the MSB as a continuation flag:

- **MSB = 1**: More bytes follow
- **MSB = 0**: Last byte

Example: the value 300 (`0x012C`) encodes as `0x82 0x2C`:

```
0x82 = 1 0000010  (continuation, data = 0x02)
0x2C = 0 0010110  (final,    data = 0x2C)
Result: 0x02 2C = 300
```

**Use variable-length integers for**: chunk lengths, delta-times, and note numbers.

### Delta-Time

Each event in a track is preceded by a variable-length integer representing the number of **ticks** since the previous event. This is how timing works — there are no absolute timestamps.

Example: with 480 ticks per quarter note:

| Delta | Time |
|---|---|
| 0 | 0.0 s |
| 120 | 0.25 s (1/4 note) |
| 240 | 0.50 s (1/2 note) |
| 480 | 1.00 s (whole note) |
| 720 | 1.50 s (dotted whole note) |

### Running Status

If the channel byte does not change, it can be omitted, saving bytes. The last status byte is "running" until a new status byte appears.

Example:

```
90 3C 64  90 45 80  90 47 60
```

Can be compressed to:

```
90 3C 64 45 80 47 60
```

**Note**: Some parsers mishandle running status. If you encounter bugs, disable it and always include the status byte.

---

## 6. Event Types

### Note On

```
9n nn vv
```

| Byte | Description |
|---|---|
| `9n` | Status: note on, channel n (0–15) |
| `nn` | Note number (0–127) |
| `vv` | Velocity (0–127; 0 = silent) |

- **Middle C (C4)** = note 60
- **A0** (lowest piano key) = note 21
- **C8** (highest piano key) = note 108
- **88-key piano range**: notes 21–108

### Note Off

```
8n nn [vv]
```

Same as Note On but with status byte `0x8n`. Velocity is optional (defaults to 0 if omitted).

### Control Change (CC)

```
Bn cc vv
```

| CC# | Name | Piano Use |
|---|---|---|
| 1 | Modulation | Vibrato on some synths |
| 6 | Data Entry | Fine tuning |
| 7 | Volume | Overall volume |
| 10 | Pan | Stereo position |
| 11 | Expression | Volume swells (distinct from velocity) |
| 64 | **Sustain Pedal** | **Critical for piano** |
| 65 | Portamento | Glide between notes |
| 66 | **Soft Pedal** | Half-pedal support |
| 67 | Sostenuto | Pedal on held notes |
| 91 | Reverb | Reverb send level |
| 93 | Chorus | Chorus send level |
| 123 | All Notes Off | Safety reset |

**Sustain pedal (CC#64)** is the single most important controller for piano realism. Without it, piano sounds dead and disconnected. Values: 0 = pedal up, 127 = pedal down. Values 1–63 = half-pedal.

### Program Change

```
Cn pp
```

Selects an instrument patch. For Acoustic Grand Piano, use `pp = 0` (GM patch #1).

### Pitch Bend

```
En ll hh
```

A 14-bit value: `ll` is the 7-bit low byte, `hh` is the 7-bit high byte. Center position is `ll = 0x40, hh = 0x00` (value 0x2000 = 8192). Range is typically ±2 semitones (configurable).

### Meta Events (inside track chunks)

| Event | Data | Description |
|---|---|---|
| `FF 00` | 2 bytes | Sequence number |
| `FF 01` | variable | Text event |
| `FF 03` | variable | Copyright notice |
| `FF 05` | variable | Marker |
| `FF 06` | variable | Cue point |
| `FF 2F` | 0 bytes | End of track (must be last event) |
| `FF 51` | 3 bytes | **Set Tempo** (microseconds per quarter note) |
| `FF 58` | 4 bytes | Time signature (numerator, denominator, ticks/cl, 8/8) |
| `FF 59` | 2 bytes | Key signature |
| `FF 7F` | variable | Sequencer-specific |

### Tempo

The Set Tempo meta event (`FF 51`) specifies microseconds per quarter note:

```
FF 51 [length: 3] [t1 t2 t3]
```

The tempo in BPM is: `60,000,000 / (t1*2^16 + t2*2^8 + t3)`

Common tempos:

| BPM | Microseconds per quarter note |
|---|---|
| 60 | 1,000,000 |
| 120 | 500,000 |
| 144 | 416,667 |
| 200 | 300,000 |

---

## 7. Piano-Specific Considerations

### Key Range

- MIDI note 21 = A0 (lowest on 88-key piano)
- MIDI note 108 = C8 (highest on 88-key piano)
- 88 keys = 88 note numbers

### Velocity Layers

Real pianos have multiple velocity layers (often 8–32+) with different recordings per layer. A basic MIDI file uses a single layer with velocity as a volume cue. For high quality:

- **Multiple velocity layers**: velocity 0–40, 41–80, 81–127 → different samples
- **Round-robin sampling**: different recordings for repeated notes
- **Key-switching**: alternate techniques (bowed piano, prepared piano)

### Aftertouch (Polyphonic Key Pressure)

Message type `An nn pp` — not all synthesizers support it. Used for subtle vibrato/brightness changes per note.

### Pedaling and Expression

Pedaling is the difference between a mechanically correct MIDI file and a musically expressive one:

1. **Sustain (CC#64)** — always include for piano
2. **Soft pedal (CC#66)** — optional, adds nuance
3. **Expression (CC#11)** — for volume swells independent of velocity
4. **Damper resonance** — some samplers simulate the sympathetic vibration of sustained strings

---

## 8. Quick Reference: Common Note Numbers

| Note | MIDI # | Note | MIDI # |
|---|---|---|---|
| C0 | 12 | C4 (Middle C) | 60 |
| C1 | 24 | C5 | 72 |
| A0 (lowest piano) | 21 | C8 (highest piano) | 108 |

---

## 9. Importing into Apple Logic Pro

### Best Practices for Logic Pro Import

1. **Use Type 1** multi-track files
2. **Use 480 ticks per quarter note** (Logic's default)
3. **Use standard GM program numbers** (Acoustic Grand = 1 on channel 1)
4. **Include sustain pedal (CC#64)** for realistic piano
5. **Use reasonable velocity values** (40–127 range works well)
6. **Include tempo map** if your composition has tempo changes
7. **Include a Set Tempo event** (even if just 500,000 µs/qn = 120 BPM)
8. **End every track with an End of Track (`FF 2F 00`) event**
9. **Avoid running status** if possible (some parsers mishandle it)

### How to Import in Logic Pro

1. **File → Import → MIDI File…**
2. Select your `.mid` file
3. Logic Pro creates one **Region** per track
4. Each region is assigned to a **Software Instrument** track
5. You can then:
   - Change the instrument (Logic's "Studio Grand Piano" is excellent)
   - Edit notes in the **Piano Roll**
   - Adjust velocity, timing (quantize), expression
   - Add effects

### Logic Pro Built-in Piano Instruments

| Instrument | Description |
|---|---|
| **Studio Grand** | Concert grand, multiple velocity layers |
| **Vintage E.P.** | Electric pianos (Rhodes, Wurlitzer) |
| **Player Piano** | Sampled upright |
| **Prepared Piano** | Experimental (muted strings) |
| **Upright** | Acoustic upright piano |
| **Tape Piano** | Tape-saturated piano |
| **Soft Grand** | Muted concert grand |
| **Clavinet** | Electric clavinet |

---

## 10. Minimal Type 1 MIDI File Example (Conceptual)

```
Header:  MThd  000006  Format:1  Tracks:2  Division:480

Track 0 (Tempo + Piano):
  00  MTrk  [data]
    00  FF 51 03 0C 42 A0    // t=0, 120 BPM (500,000 µs/qn)
    00  FF 3B 01 48 00        // t=0, tempo name: "Allegro"
    00  90 3C 64              // t=0, ch1, C4 (60), vel 100
   240  80 3C 40              // t=0.5s, ch1, C4 off, vel 64
    00  B0 40 7F              // t=0, sustain pedal ON (127)
    00  C0 00                 // t=0, Program Change = 0 (Acoustic Grand)
   480  90 45 80              // t=1.0s, ch1, D4 (69), vel 128
   480  80 45 40              // t=1.5s, D4 off
   480  B0 40 00              // t=1.5s, sustain pedal OFF (0)
    00  FF 2F 00              // t=1.5s, End of Track

Track 1 (Bass):
  00  MTrk  [data]
    00  91 36 50              // t=0, ch2, C3 (54), vel 80
   480  81 36 40              // t=1.0s, C3 off
    00  FF 2F 00              // t=1.0s, End of Track
```

---

## 11. Common Pitfalls When Generating MIDI Files

| Pitfall | Solution |
|---|---|
| Wrong file type | Use **Type 1** for multi-track |
| Wrong division | Use **480** ticks per quarter note for Logic Pro |
| Missing tempo | Always include a **Set Tempo** meta event |
| Missing end of track | Every track must end with **`FF 2F 00`** |
| No sustain pedal | Always include **CC#64** for piano |
| Wrong program number | GM Acoustic Grand = **patch 0** (not 1) |
| Running status bugs | Disable running status for maximum compatibility |
| Negative delta-times | Delta-times must always be **non-negative** |
| Wrong byte order | All multi-byte integers are **big-endian** |
| Truncated track | Chunk length must match actual data length |
| Channel 10 used for piano | Use **channels 0–9** for melodic instruments |
| Velocity = 0 for note on | Velocity 0 = Note Off; use velocity ≥ 1 for notes |

---

## 12. Standards and Specifications

| Standard | Description | Link |
|---|---|---|
| **MIDI 1.0 Spec** | Original 1983 specification | [midi.org/specifications](https://www.midi.org/specifications) |
| **MIDI 2.0 Spec** | Updated 2020 spec, enhanced resolution | [midi.org/specifications/midi-2-0](https://www.midi.org/specifications/item/midi-2-0-specification) |
| **SMF 1.0** | Standard MIDI File format spec (MMA, 1996) | [MMA SMF 1.0](https://www.midi.org/file-format-specifications) |
| **General MIDI** | GM specification (MMA, 1991) | [MMA GM spec](https://www.midi.org/specifications-old/item-general-midi-1) |
| **GS / XG** | Roland GS and Yamaha XG extensions | [Roland GS](https://www.midi.org/specifications-old/item-roland-gs) / [Yamaha XG](https://www.midi.org/specifications-old/item-xg) |
| **MIDI over USB** | USB implementation spec | [MMA USB MIDI](https://www.midi.org/specifications-old/item-usb-midi-implementation-guidelines) |
| **MIDI over IP (MIDI 2.0)** | Network transport | [MMA MIDI over IP](https://www.midi.org/specifications-old/item-midi-over-ip) |

---

## 13. Software Libraries and Tools

| Tool / Library | Language | Notes |
|---|---|---|
| **gmidi** | C | Lightweight MIDI library |
| **liblo** | C/C++ | OSC (MIDI-over-network alternative) |
| **rtmidi** | C++ | Real-time MIDI I/O (Iain Paterson) |
| **rtmidi** | C++ | Modern fork, cross-platform |
| **pymid** / **mido** | Python | MIDI file reading/writing |
| **mido** | Python | Popular Python MIDI library |
| **midiutil** | Python | Simple MIDI file generation |
| **tinyxml** / **rapidxml** | C++ | XML (not MIDI, but useful for config) |
| **FluidSynth** | C | Sound font synthesizer (plays MIDI as audio) |
| **Timidity++** | C | Open-source sound font player |
| **Qtractor** | Linux | MIDI sequencer |
| **Ardour** | C++ | DAW with MIDI support |
| **MIDIFlow** | Java | MIDI file parsing/generation |

---

## 14. Checklist for Generating a Logic Pro-Compatible MIDI File

- [ ] File type is **Type 1**
- [ ] Division is **480** ticks per quarter note
- [ ] Header chunk is exactly 14 bytes
- [ ] Each track chunk starts with `"MTrk"`
- [ ] Each track ends with `FF 2F 00` (End of Track)
- [ ] Chunk length matches actual data length
- [ ] All multi-byte integers are **big-endian**
- [ ] Delta-times are **non-negative** variable-length integers
- [ ] A **Set Tempo** (`FF 51`) meta event is present
- [ ] Program Change to **patch 0** (Acoustic Grand) on channel 0
- [ ] **Sustain pedal (CC#64)** messages are included
- [ ] Velocities are in range **1–127** (not 0 for Note On)
- [ ] Note numbers are in range **21–108** (full 88-key piano)
- [ ] No **running status** (for maximum parser compatibility)

---

## 15. Extending Beyond Piano

When you want to expand beyond piano, the same principles apply:

- **Strings**: Use portamento (CC#65) for smooth glides
- **Brass**: Use expression (CC#11) for dynamics
- **Drums**: Channel 10, note numbers per drum kit map
- **Bass**: Lower register, use portamento for slides
- **Vocals**: Use vibrato (CC#1 modulation) and expression (CC#11)

The file format and protocol remain the same regardless of instrument.

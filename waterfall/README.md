# Waterfall

A command line utility that reads an audio file and renders a frequency
analysis of its acoustic energy over time.

## Overview

Waterfall is named for the SONAR-style display it produces: frequency runs
left to right and time runs top to bottom, one row per time slice. Although
the display is SONAR-shaped, the underlying analysis is a **spectral
analysis** — a forward FFT per row via [libaudio](../libaudio), with the
magnitude spectrum quantized to 16-bit integers.

The columns follow the MIDI note grid: **128 notes × 8 bands = 1024
columns** (the 128 is the number of representable MIDI notes; each note is
split into 8 bands from low to high). A column's position encodes both *which
MIDI note* and *which band within that note*.

There are two analysis modes (see [Modes](#modes)): by default the mapping is
the musically faithful **MIDI** mode, where each column's center frequency is
the true exponential (equal-tempered) frequency of its note; `--pcm` selects
the legacy **PCM** linear sweep.

`waterfall` shares its audio-file-reading layer with `midicapture`, so both
tools analyze the same `libaudio` representation of a recording. The longer-
term goal is to make `libaudio`'s acoustic analysis useful enough to convert
an audio file into a MIDI performance that recreates similar audio.

## Output Format

Output is a **text format** designed for scripting:

- **Line 1** — a single line of `name=value` pairs (a header), terminated by
  a newline. It carries the grid geometry and one center frequency per column:

  ```
  sampleRate=48000 timeSliceMs=10.000000 hopSize=2048 windowSize=2048 numBins=1025 midiNotes=128 bandsPerNote=8 nyquistHz=24000.000000 numColumns=1024 mode=MIDI refDb=38.295300 autoScale=1 col0=8.175758 col1=8.661642 ... col1023=12543.733887
  ```

  `mode` is `MIDI` (default) or `PCM`. A consumer that does **not** find a
  `mode` key should assume **PCM** — the legacy linear behavior — so output
  produced before the mode was introduced remains interpretable. The
  `colN=` values are each column's center frequency in Hz; in MIDI mode they
  are the true exponential note frequencies, in PCM mode they form the legacy
  linear sweep.

- **Each following line** — one time slice: `numColumns` values, each a 16-bit
  integer printed as **4 uppercase hex digits** (`0000`–`FFFF`), space-
  separated. `0000` is silence; `FFFF` is full scale.

A row is one FFT frame advanced by the hop, so `timeSliceMs` and the FFT
window together set both the temporal resolution and the (power-of-two) hop.

## CLI

```
waterfall [options] <input.aiff>

Options:
  --help                     Print this message.
  --window-size <int>        FFT window size, power of two (default: 2048).
  --time-slice <float>       Row duration in milliseconds (default: 10).
  --bands-per-note <int>     Bands per MIDI note (default: 8).
  --ref-db <float>           Full-scale reference level in dB (default: 0).
  --no-auto-scale            Use --ref-db verbatim instead of auto-scaling.
  --pcm                      Use the legacy linear frequency mapping
                             (default: the musically faithful MIDI mapping).
```

`<input.aiff>` may be any format libsndfile supports (AIFF, WAV, FLAC, OGG,
etc.). The tool writes to **stdout**; redirect or pipe it:

```bash
waterfall recording.aiff > waterfall.txt            # MIDI (default)
waterfall --pcm recording.aiff > legacy.txt         # legacy linear sweep
waterfall --time-slice 2.78 --window-size 4096 recording.aiff > fine.txt
waterfall --no-auto-scale --ref-db -12 recording.aiff | less
```

### Scaling

Each column magnitude is converted to dB and mapped linearly onto the 16-bit
range between a noise floor (`-60 dB`) and a reference (`refDb`):

```
level16 = round( clamp( (db - -60) / (refDb - -60), 0, 1 ) * 65535 )
```

The `-60 dB` floor is a **pragmatic display cut-off**, not a physical limit:
although a 16-bit word has ~96 dB of theoretical dynamic range, an FFT *bin
magnitude* rarely spans anything like that in practice (a full-scale tone
sits ~60 dB below a full-scale reference at a 2048-pt window, the window
function adds ~6 dB more, and real signals spread energy across many bins).
Reachable levels cluster in the `-60…0 dB` band, which is what the floor is
tuned to; anything at or below it reads as silence (`0000`).

By default `waterfall` **auto-scales**: it makes one pass to find the loudest
FFT bin in the file, sets `refDb` to that level (in dB), and reports the
resolved value plus `autoScale=1` in the header. This keeps the usable dynamic
range visible instead of saturating a loud recording to `FFFF` everywhere.
Pass `--no-auto-scale` to use `--ref-db` exactly as given (`autoScale=0`).

## Modes

`waterfall` runs in one of two modes, chosen by the `--pcm` flag. The column
count is **128 notes × bands-per-note** in both; only the *placement* of each
column's center frequency differs.

| Mode | Selected by | Column frequencies |
|---|---|---|
| **MIDI** *(default)* | *(no flag)* | True exponential (equal-tempered): column for note *n* sits at `440·2^((n−69)/12)` Hz, with the note's bands spread log-evenly across its 12th-of-an-octave span. Sample-rate independent. |
| **PCM** | `--pcm` | Legacy linear sweep: the full Nyquist is divided into 128 equal *Hz* chunks, each subdivided into equal-Hz bands. Sample-rate dependent; not a faithful pitch mapping. |

In **MIDI** mode the mapping is what you'd expect: a 100 Hz source lights
column 33, a 200 Hz source lights column 65 (one octave up, two MIDI notes
higher), and a 440 Hz source lights column 89 (A4). In **PCM** mode a 100 Hz
source lands in a different column depending on the file's sample rate — the
linear sweep is a display choice, not a pitch model.

The active mode is written to the header (`mode=MIDI` or `mode=PCM`). A
consumer that does not find a `mode` key should assume **PCM** (the legacy
behavior), so pre-mode output stays interpretable.

## Build

```bash
cd waterfall
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

The executable lands in `build/bin/waterfall`. Requires aubio, libsndfile,
and Boost (program_options) — all resolved via pkg-config / `find_package`.
`libaudio` is pulled in as a sibling subdirectory and built automatically.

## Video Renderer (`waterfall_video.py`)

A companion Python script animates the waterfall text output as a scrolling
color video (H.264 / MP4, 30 fps) with the source audio muxed in sync:

```bash
python3 waterfall_video.py <text_file> <audio_file> <height> <width> [-o out.mp4] [--vscale N]
```

- `<text_file>`   — the `waterfall` output (header line + one hex row per slice).
- `<audio_file>`  — the same recording the waterfall was generated from.
- `<height>` `<width>` — output frame size in pixels (rounded to even for `yuv420p`).
- `-o/--output`  — output path (default: `<text_file>.mp4`).
- `--vscale N`   — optional vertical magnification multiplier. The waterfall image
  is stretched vertically by `DEFAULT_VSCALE × N` (a named constant at the top of
  the script, default 2.0), so `--vscale 1.0` reproduces the default look, `0.5`
  halves it, `2.0` doubles it, etc. The scroll speed is scaled by the same factor
  so the audio stays in sync and the video still ends when the tail passes the
  playhead. Higher values reveal more per-row detail (up to the number of data
  rows); beyond that the rows are only zoomed, not multiplied.

There is no `--mode` flag: the script reads the `mode` key from the waterfall
header to choose the horizontal layout (MIDI vs PCM; see [How it works](#how-it-works)).
A header without a `mode` key renders as **PCM** (the legacy layout), so output
produced before the mode was introduced is unchanged.

### How it works

- **Color:** each sample is mapped through an **arc of the HSV color wheel**
  — value 0 is black, value 65535 is full yellow, and the hue arcs from blue
  through red to yellow (brightness is proportional to the sample). The arc is
  controlled by the `HUE_START` / `HUE_SWEEP` constants at the top of the script.
- **Scroll & sync:** the rows (listed in recording order, row 0 = the start) are
  reversed and scrolled **downward** at constant speed past a playhead drawn as a
  1-px line at the vertical center — the playhead is the *onset line*, a zero
  duration instant of time. Scroll speed is the image's full height divided by the
  audio's duration, so the video's duration equals the audio file's duration. The
  sounding band is **edge-anchored** to the playhead, not centered on it: at
  t=0 the *first* band's **bottom edge** sits on the playhead (so the band lies
  entirely above the line, and the start of the recording plays at the start of
  the clip), and at the end the *last* band's **top edge** reaches the playhead —
  the moment the band has fully passed and there is no more audio. At any instant
  the row at the playhead is the audio you hear. (For short clips at high
  `--vscale`, where a band spans multiple pixels, the edge anchoring is visible;
  for longer files a band is ≤ 1 px and the shift is sub-pixel, so the two reads
  the same.)
- **Vertical scale:** the waterfall image is magnified vertically by
  `DEFAULT_VSCALE × --vscale` and the scroll speed is scaled identically, so audio
  sync and the end-point (tail at the playhead) are unchanged — only the
  per-row vertical resolution of the display changes.
- **Vertical anti-aliasing:** the scroll is continuous (sub-pixel) but the source
  is one data row per pixel, so a nearest-neighbor sample steps in 1-row
  increments and looks blocky. Each output row instead samples `V_SUPERSAMPLE`
  sub-rows straddling its fractional position and block-means them (a `V_SUPERSAMPLE=1`
  invocation disables it), smoothing the vertical stepping the way the horizontal
  mean smooths the compressed high-frequency end. Constant at the top of the
  script; the default 8 matches the horizontal factor.
- **Horizontal (frequency) scale — mode-dependent:** the script reads the
  waterfall header's `mode` key to choose the layout. A missing `mode` key is
  treated as **PCM** (the legacy behavior), so old output renders exactly as it
  always did.
  - **MIDI** (`mode=MIDI`): the 128 MIDI notes are given **equal horizontal
    width** — each note is a 12th of an octave, so equal notes get equal width
    on a log axis. Each note's internal bands are placed at **equal pixel
    width** within the note. Because `waterfall` already lays each band's center
    frequency out log-evenly within a note, equal-width pixels reproduce the
    true equal-tempered *frequency* spacing (a band spanning a 2× ratio isn't
    over-widened the way a linear-Hz split would be). This is the musically
    faithful display: a singer's fundamental and its harmonics land on the
    correct notes across the full width.
  - **PCM** (legacy, `mode=PCM` or absent): the old behavior — each column's
    center frequency (from the header's `col_c` keys) is placed on a
    **logarithmic** axis at `x = (ln f − ln F_MIN) / (ln F_MAX − ln F_MIN)`,
    which widens the low bands (where most musical energy lives) and compresses
    the high bands. The map runs `F_MIN_HZ` (default 16 Hz) to `F_MAX_HZ`
    (default 16 kHz), so content outside that audible window is dropped. This is
    what made recordings of vocal singing so legible before the mode existed.

  Both layouts are **display-only**: the underlying waterfall text and data are
  untouched. In each case sub-pixel columns are **mean-anti-aliased** (a
  temporary buffer `H_SUPERSAMPLE`× wider than the output is block-averaged
  down) so thin columns read as dimmer pixels instead of vanishing; `H_SUPERSAMPLE`
  and the `F_MIN_HZ` / `F_MAX_HZ` bounds (PCM only) are constants at the top of
  the script. (Vertical anti-aliasing is a separate transform, `V_SUPERSAMPLE`,
  documented above.)
- **Rendering:** numpy builds each frame (no Pillow / ImageMagick needed); raw
  RGB frames are piped to FFmpeg over stdin, which encodes H.264 and muxes the
  audio.

Requires **numpy** and **ffmpeg** on `PATH`.

## Project Layout

```
waterfall/
├── CMakeLists.txt              # Build config (libaudio, Boost program_options)
├── waterfall_video.py          # Optional: animate the text output as an MP4 video
├── include/waterfall/
│   └── audioFile.h             # AudioFile — shared file-reading wrapper
└── src/
    ├── main.cpp                # Entry point: CLI, FFT loop, quantization, output
    └── audioFile.cpp           # AudioFile implementation (libsndfile via libaudio)
```

`main.cpp` is the entire analysis tool. `audioFile` is the only module
shared conceptually with `midicapture` (each tool has its own copy, so the
file I/O path stays identical across both).

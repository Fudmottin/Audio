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
split into 8 bands from low to high). Each column's center frequency is laid
out linearly across the Nyquist, so a column's position encodes both *which
MIDI note* and *which band within that note*.

`waterfall` shares its audio-file-reading layer with `midicapture`, so both
tools analyze the same `libaudio` representation of a recording. The longer-
term goal is to make `libaudio`'s acoustic analysis useful enough to convert
an audio file into a MIDI performance that recreates similar audio.

## Output Format

Output is a **text format** designed for scripting:

- **Line 1** — a single line of `name=value` pairs (a header), terminated by
  a newline. It carries the grid geometry and one center frequency per column:

  ```
  sampleRate=48000 timeSliceMs=10.000000 hopSize=2048 windowSize=2048 numBins=1025 midiNotes=128 bandsPerNote=8 nyquistHz=24000.000000 numColumns=1024 bandWidthHz=23.437500 refDb=38.295300 autoScale=1 col0=11.718750 col1=35.156250 ... col1023=23988.281250
  ```

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
```

`<input.aiff>` may be any format libsndfile supports (AIFF, WAV, FLAC, OGG,
etc.). The tool writes to **stdout**; redirect or pipe it:

```bash
waterfall recording.aiff > waterfall.txt
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

### How it works

- **Color:** each sample is mapped through an **arc of the HSV color wheel**
  — value 0 is black, value 65535 is full yellow, and the hue arcs from blue
  through red to yellow (brightness is proportional to the sample). The arc is
  controlled by the `HUE_START` / `HUE_SWEEP` constants at the top of the script.
- **Scroll & sync:** the rows (listed in recording order, row 0 = the start) are
  reversed and scrolled **downward** past a playhead at the vertical center —
  the head (first row of the recording) begins on the playhead, newer rows enter
  from the top, and the oldest leave from the bottom. Scroll speed is tied to the
  *real* per-row audio duration (`hopSize / sampleRate` from the header), so the
  row at the playhead is the audio you hear at that instant. The audio plays for
  the full clip, and the video ends when the *tail of the data* passes the
  playhead — i.e. at the audio's duration.
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
- **Horizontal (frequency) scale:** the waterfall's columns are *linear* in
  frequency (128 MIDI notes × `bands-per-note`), so most audible content sits on
  the left of the frame. The display warps the horizontal axis to a
  **logarithmic** frequency map — each column's center frequency (from the
  header's `col_c` keys) is placed at `x = (ln f − ln F_MIN) / (ln F_MAX − ln F_MIN)`
  — which widens the low bands (where most musical energy lives) and compresses
  the high bands, matching perceptual spacing. The map runs `F_MIN_HZ` (default
  20 Hz) to `F_MAX_HZ` (default 20 kHz, the audible window), so ultrasonic
  content above 20 kHz is dropped. Sub-pixel columns in the compressed high end
  are **mean-anti-aliased** (a temporary buffer `H_SUPERSAMPLE`× wider than the
  output is block-averaged down) so they read as dimmer pixels instead of
  vanishing. All three constants (`F_MIN_HZ`, `F_MAX_HZ`, `H_SUPERSAMPLE`) are
  at the top of the script. This is a display-only transform: the underlying
  waterfall text and data are untouched. (Vertical anti-aliasing is a separate
  transform, `V_SUPERSAMPLE`, documented above.)
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

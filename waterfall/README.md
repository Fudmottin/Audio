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
                             [--eq-per-note [dB]] [--eq-per-octave [dB]] [--eq-over-all [dB]]
                             [--preserve-energy] [--eq] [--floor dB]
                             [--color {hsv,ironbow}] [--ironbow] [--h-supersample N]
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
- `--eq-per-note [dB]` — per-note equalizer (default off): peak-normalize each
  note, then a center bell (mids at 0 dB, edges down by up to `dB`, default
  `EQ_ROLLOFF_DB`) so each note's core pops and its edge bands recede. Best with
  MIDI output.
- `--eq-per-octave [dB]` — per-octave equalizer (default off): peak-normalize
  each octave, then a center bell across the octave. Balances the octaves (the
  low ones carry far more energy).
- `--eq-over-all [dB]` — whole-width equalizer (default off): peak-normalize the
  file, then a single bell across the full width. A coarse global balance.
- `--preserve-energy` — rescale each note after shaping so its **total energy**
  is unchanged by the bell (the note's energy budget is restored; nothing clips).
  Applies to `--eq-per-note`.
- `--eq` — shorthand for `--eq-per-note` (the legacy per-note behavior).

  The equalizer flags are **stackable**: any combination is composed in a fixed
  coarse-to-fine order (whole width → octaves → notes). See
  [Equalization](#equalization).
- `--floor dB` — hard **noise gate** (default off): zero out any sample whose
  level is below a dB floor. See [Noise floor](#noise-floor).
- `--color {hsv,ironbow}` — choose the **false-color ramp** (default `hsv`).
  `hsv` is the original blue→red→yellow arc; `ironbow` is the FLIR/thermal
  black→blue→purple→red→orange→yellow→white gradient. See [Color map](#color-map).
- `--ironbow` — shorthand for `--color ironbow`.
- `--h-supersample N` — horizontal supersample factor for the frequency warp
  (default 8). Higher spreads more distinct colors across the compressed
  high-frequency end at the cost of a little softness; 1 disables it.

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

### Equalization

Real recordings — especially piano — read as a bright left side and a dim right
side: the low octaves carry far more average energy, and each note's *edge*
bands pick up bleed from the neighboring notes, muddying the center. The
equalizer addresses both, and is now **stackable**: three composable transforms,
each an optional CLI flag, applied in the order the flags appear on the command
line.

Every transform follows the same two steps, applied over a *group* of columns
(a group is 12 notes = an octave, 1 note, or the whole width):

1. **Peak-normalize** each group so its loudest sample across the whole file
   reaches full scale. This equalizes the groups against one another *and*
   enforces the core rule — **no value is ever boosted above that group's own
   maximum**. A group that already peaks at full scale is left flat. This is a
   pure scale, so it cannot introduce brightness beyond a group's true peak.
2. Apply a smooth **raised-cosine bell** across the group: the group's *center*
   stays at 0 dB (full), and the edges roll off by up to the transform's dB.
   This is the "mids at 0, bass/treble down" shape of a graphic equalizer.
   The bell's maximum weight is 1.0 (at the center), so it only ever *reduces*
   edge energy relative to the peak-normalized result — it sharpens the core
   and lets neighbor-bleed recede.

| Flag | Group | Bell span | Default depth |
|---|---|---|---|
| `--eq-per-octave [dB]` | 12 notes (an octave) | across the octave | `EQ_PER_OCTAVE_DB` (6 dB) |
| `--eq-per-note [dB]` | 1 note | across the note's bands | `EQ_ROLLOFF_DB` (6 dB) |
| `--eq-over-all [dB]` | the whole width | across the full range | `EQ_OVERALL_DB` (0 dB = flat) |

The optional dB argument overrides the named constant; a bare flag uses the
constant. `--eq` is a shorthand for `--eq-per-note` (the legacy behavior).

- **Energy preservation (`--preserve-energy`)**: the bell *redistributes* a
  group's energy (the integral of its band amplitudes). By default the group's
  total energy after shaping is whatever the bell leaves. Pass
  `--preserve-energy` to rescale each group by `raw_total / shaped_total` after
  shaping, restoring the group's total energy to exactly what peak-normalization
  set. Because the peak weight is 1.0, the rescale factor is always ≤ 1.0, so
  the bell *shape* (the pillow) is preserved, nothing clips, and the group's
  new peak = `raw_total / sum(weights)` stays at or below full scale. Applies to
  `--eq-per-note`. This is the "area under the curve" step: the note's energy
  budget is restored to the peak-normalized level. (Note the trade-off: because
  the bell *reduces* edge energy, restoring the total rescales the whole note up,
  which flattens the bell's taper — you keep the note's energy, at the cost of a
  less pronounced mids-only peak. Without the flag the bell's taper is left as
  is.)
- **Stacking**: transforms compose in a fixed **coarse-to-fine** order
  (whole width → octaves → notes) regardless of how you order them on the
  command line; each is a pure static transform, so the composition is
  well-defined and deterministic. A typical stack is `--eq-over-all 3
  --eq-per-octave 6 --eq-per-note 6 --preserve-energy`: a mild global tilt, an
  octave balance, then a per-note sharpen (the last, finest stage) with the
  note's energy budget preserved.
- The curve is computed **once from the file** (a static model, not a dynamic
  compressor), so the scroll stays stable and there are no pumping artifacts.
- The equalization is **display-only**: the underlying waterfall text and data
  are untouched. It operates on the raw 16-bit rows before color mapping.
- The transforms are intended for **MIDI** output, where columns map cleanly
  onto the 128-note grid. For PCM/legacy output the columns are a linear
  frequency sweep, so the note/octave grouping is less meaningful (the flags
  still work, but are best with MIDI).

Anything that displays well this way is a good candidate for generating a MIDI
file: the same per-note normalization that reveals each note's core (and tames
the neighbor-bleed at its edges) is exactly the information a note-tracking step
needs to see quiet upper-register onsets cleanly, not just the loudest lows.

### Color map (`--color`)

Each sample is mapped to a color on a **false-color ramp** — a *linear* map of the
(dB-quantized) 16-bit value onto a fixed RGB path, so the data is unchanged, only
the coloring. Two ramps are available:

| `--color` | Look | Top end |
|---|---|---|
| `hsv` *(default)* | HSV hue arc: blue → magenta → red → yellow (saturation fixed at 1) | pure yellow |
| `ironbow` | FLIR / weather-radar / "ironbow" gradient: black → blue → purple → magenta → red → orange → yellow → white | hot white |

`ironbow` is the classic thermal / radar false-color look. It is a **piecewise-
linear RGB gradient** (a 256-entry lookup table sampled by 8-bit brightness —
zero per-frame cost), chosen over HSV because a multi-channel linear-in-value ramp
spreads the full 0–255 range across distinct colors more evenly (less low-end
banding than the HSV arc's dark-blue end) and ends on FLIR's signature **hot
white** rather than pure yellow. Use `--ironbow` for the shorthand.

**A/B'ing distinct-color density:** `--h-supersample N` (default 8) is the lever.
Higher values block-average more neighboring source columns into each output pixel,
which — because adjacent columns have slightly different colors — spreads the
compressed high-frequency end over more distinct colors at the cost of a little
anti-aliasing softness. Render the same file at, e.g., `--h-supersample 8` and
`16` to compare. (Vertical scroll smoothing is a separate factor, `V_SUPERSAMPLE`.)

### Noise floor (`--floor`)

A hard noise gate that removes the dim background a recording carries (room
noise, hiss, the faint bleed under a note). It is the inverse of the
auto-scaling `waterfall` performs: auto-scale lifts the file's *peak* to full
scale (`FFFF` = 0 dB); the floor gates the *low* end, zeroing the quiet noise
beneath a level you choose.

- **The argument is a dB magnitude read as `-abs(dB)`.** `--floor 30` and
  `--floor -30` both mean **−30 dB**. `0 dB` is full scale (`FFFF`) and `--floor 0`
  is the no-op gate, so you can't accidentally gate above full scale.
- **Integer mapping:** the floor is converted to a 16-bit integer with the same
  normalization `waterfall` used to *write* the file —
  `int = round(clamp((floor − −60 dB) / (refDb − −60 dB), 0, 1) × 65535)`, where
  `refDb` is read from the header — and **any sample strictly below that integer
  is set to 0.** Because it reuses the tool's own dB↔16-bit map, a floor you set
  in dB lands on exactly the level the tool quantized, so a −30 dB floor is −30 dB.
- **Applied after the equalizer, before color mapping.** A stack like
  `--eq-per-note 6 --floor 24` first peak-normalizes each note (which can lift a
  note's quiet noise up to full scale) and then gates the residual noise back out,
  leaving each note's core bright and clean. It works in both MIDI and PCM layouts.
- **Display-only:** the waterfall text and underlying data are untouched; it only
  changes what is drawn. A warning is printed if the header lacks a usable `refDb`
  (a hand-crafted file) and the reference is instead derived from the file's peak.

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

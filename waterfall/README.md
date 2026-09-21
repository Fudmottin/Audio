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
range between a floor (`-60 dB`) and a reference (`refDb`):

```
level16 = round( clamp( (db - -60) / (refDb - -60), 0, 1 ) * 65535 )
```

By default `waterfall` **auto-scales**: it makes one pass to find the loudest
FFT bin in the file, sets `refDb` to that level (in dB), and reports the
resolved value plus `autoScale=1` in the header. This keeps the full dynamic
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

## Project Layout

```
waterfall/
├── CMakeLists.txt              # Build config (libaudio, Boost program_options)
├── include/waterfall/
│   └── audioFile.h             # AudioFile — shared file-reading wrapper
└── src/
    ├── main.cpp                # Entry point: CLI, FFT loop, quantization, output
    └── audioFile.cpp           # AudioFile implementation (libsndfile via libaudio)
```

`main.cpp` is the entire analysis tool. `audioFile` is the only module
shared conceptually with `midicapture` (each tool has its own copy, so the
file I/O path stays identical across both).

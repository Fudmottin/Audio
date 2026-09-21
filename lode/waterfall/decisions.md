# waterfall — Key Decisions

## Decisions Made Jointly with User

1. **Single analysis source file.** The whole tool lives in `src/main.cpp`;
   only `audioFile` is split out. Keeps the spectral-analysis path in one
   readable place.

2. **`audioFile` is the shared file-reading layer** with `midicapture`. Each
   tool keeps its own copy so the libsndfile-via-libaudio I/O path stays
   identical across both.

3. **Text output, 16-bit unsigned integers in hex, space-separated, one time
   slice per line.** Chosen for scripting ease over a binary or glyph-ramp
   format. The user noted it won't matter for C++ processing.

4. **`--time-slice` in milliseconds, stored as a `float`.** The user wanted a
   float so that `--time-slice 10` and `--time-slice 2.78` are both valid.
   Parsed like the compiler reads it (C `strtod` semantics via Boost
   `value<double>`).

5. **First line is `name=value` pairs** carrying semantic information for the
   rows, including the frequency of each column, so the header documents the
   grid.

6. **Boost program_options** for command-line processing (consistent with
   midicapture).

7. **Internal hop is integral, with possible overlap between rows and a
   minimum of one sample per row.** The user explicitly allowed overlap.

## Decisions Made by Agent

1. **Hop = window size (a power of two), one hop per row.** aubio's FFT
   requires a power-of-two window/hop, so the hop cannot be an arbitrary
   "ms → samples" value. We therefore set `hop = windowSize` (default 2048)
   and advance one hop per row. This guarantees ≥1 sample/row and gives
   maximum overlap. `--time-slice` is reported in the header but the *actual*
   temporal stepping is the power-of-two hop. **Open:** a `--hop-size` option
   (constrained to powers of two) could decouple row count from window size
   later.

2. **Columns = 128 notes × `--bands-per-note` (default 8 → 1024).** This
   matches the README's "128 × 8" scheme and the MIDI note grid. Replaced the
   earlier `--freq-min/--freq-max/--num-columns` options, which would have
   produced a uniform linear sweep that contradicted the note/band scheme.

3. **Column center frequencies linear across the Nyquist, grouped by note.**
   `noteWidth = nyquist/128`, `bandWidthHz = noteWidth/bandsPerNote`; column
   `c`'s center is at `(noteIndex + (bandIndex+0.5)/bandsPerNote)*noteWidth`.
   Keeps the header's per-column Hz values consistent with the analysis and
   covers the full audible range.

4. **Column value = max magnitude over the bins inside its band.** A max (not
   a sum/mean) preserves transient peaks, which is what a waterfall is meant
   to surface.

5. **Auto-scale by default.** A fixed `refDb=0` saturated loud files to a
   `FFFF` wall. The tool now makes one peak-scan pass, sets `refDb` to the
   loudest FFT bin (in dB), and reports `refDb=` + `autoScale=1` in the
   header. `--no-auto-scale` reverts to `--ref-db` verbatim. The header always
   carries the *resolved* reference so downstream scripts know the scale.

6. **`AudioFile` owns its `AudioFileReader` via a raw pointer + `delete` in
   the destructor** (mirrors midicapture), because the libaudio reader is
   non-copyable/non-movable. `std::unique_ptr`/`make_unique` does not work
   here.

7. **`AudioFile::readInto(count, buf)` zero-fills the buffer, then reads.**
   This zero-pads the final (partial) row at EOF so the FFT always has a full
   `windowSize`-length input, and returns the count of *real* frames read.

8. **`readInto` checks `eof()` *before* reading.** At construction
   `currentFrame` is 0, so the first read is unaffected; this guard prevents
   a final over-read past EOF from returning stale data.

## Design Reconciliation (README ↔ implementation)

The original README described the display conceptually ("0 to 20 kHz", "65,536
max", "yet to be determined time slice"). The implementation refines this:

- **Frequency range** is 0 → **Nyquist** (sampleRate/2), not a fixed 20 kHz —
  it follows the input file's sample rate. The header reports `nyquistHz`.
- **Max value is 65,535** (`0xFFFF`), not 65,536 (2^16 is out of range for a
  16-bit unsigned). The README's "65,536" was corrected.
- **Time slice** is `--time-slice` (ms); the row count is derived from the
  file length and the hop.

## Verification

- **Builds** warning-free for the two waterfall sources; links libaudio +
  Boost program_options.
- **All 84 libaudio unit tests pass.**
- **Run-tested** on `final-fantasy.aiff` (auto-scale shows a real spectral
  shape; `--no-auto-scale --ref-db 0` correctly clips a loud file to `FFFF`)
  and the `1000Hz` sine (silent lead-in → leading `0000` rows, as expected).

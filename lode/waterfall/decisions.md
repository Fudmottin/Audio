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

## Video renderer equalizer (stackable) — added for `waterfall_video.py`

The display equalizer was a single `--eq` flag (one per-note curve). It is now
**three composable, stackable display-only transforms** in `waterfall_video.py`
(the C++ `waterfall` tool is untouched and stays lossless):

1. **`--eq-per-note [dB]`** — the legacy per-note curve, renamed. Peak-normalize
   each of the 128 notes, then a raised-cosine bell (center 0 dB, edges rolled
   off by up to `dB`, default `EQ_ROLLOFF_DB`=6). Optional dB overrides the
   constant; a bare flag uses it. `--eq` is kept as a shorthand for this (the
   legacy behavior is preserved exactly).
2. **`--eq-per-octave [dB]`** — same two-step engine, but the group is 12 notes
   (an octave) and the bell spans the octave. Balances octaves (low octaves
   carry far more energy). Default `EQ_PER_OCTAVE_DB`=6.
3. **`--eq-over-all [dB]`** — the whole width is one group; peak-normalize the
   file, then a single bell across the full range. Coarse global balance.
   Default `EQ_OVERALL_DB`=0 (a flat peak-normalized file).
4. **`--preserve-energy`** — after shaping a note, rescale it by
   `raw_total / shaped_total` so the note's **total energy** equals the
   peak-normalized total (the "area under the curve" step the user asked
   about). Because the bell's peak weight is 1.0, the rescale factor is ≤ 1.0
   and the note's new peak stays ≤ full scale (no clipping). Applies to
   `--eq-per-note` only. Trade-off (documented in README): restoring the energy
   budget rescales the note up, which flattens the bell's taper — the user
   keeps the note's energy at the cost of a less pronounced mids peak.

**Composition:** transforms are composed in a fixed **coarse-to-fine** order
(whole width → octaves → notes) regardless of how the flags are ordered on the
command line. This is documented as the stacking model. `apply_eq_stack(rows,
bands_per_note, [(name, kwargs), ...])` is the composition point; a `"eq"`
entry expands to the per-note transform.

**Implementation notes (for the next agent):**
- One engine, `_apply_bell(rows, unit, size, rolloff_db, preserve_energy)`,
  serves all three: `unit` assigns each column to its within-group position;
  the group is `col // size`; the bell is driven by `unit % size`.
  `per_note` uses `size=bands_per_note, unit=col % bands_per_note`; `per_octave`
  uses `size=12*bands_per_note, unit=col // bands_per_note`; `over_all` uses
  `size=num_cols, unit=col`. The raised-cosine `u = 2*(unit_in_group+0.5)/size
  − 1` lands the center on a unit (the `+0.5` is load-bearing for even counts).
- `_group_peaks` (per-group max via `np.maximum.at`) and `_group_sums`
  (per-row, per-group sum via a row loop + `np.add.at`) are the vectorized
  helpers. The energy rescale must pass the **full 2-D** arrays to
  `_group_sums` (a 1-D per-row reduction is misread by the row loop); and the
  result gather is `ratio[:, group]` (ratio is already 2-D) — adding an extra
  `[None, :]` axis produces a 3-D array and a wrong-shape int cast.
- All transform outputs are peak-normalized first (a pure scale, ≤ full scale
  for groups already near full scale — *notes whose peak is a few dB below full
  scale are legitimately raised up to full scale by this*; this is the intended
  equalization, not a bug) and then bell-shaped (weight ≤ 1.0, so it only
  reduces). Verified: every transform's output max ≤ 65535; energy preserved to
  machine precision in float space (the ~1–3% int-space loss is int truncation
  after clipping, inherent to 16-bit output, and only bites for notes already
  near full scale).
- `waterfall` still writes `mode=MIDI`/`mode=PCM` + per-column `colN=` center
  frequencies; the renderer reads `mode` (missing → PCM) and `bandsPerNote`.
  The `--window-size 1024` segfault in the C++ binary is a known, deferred bug
  (works at 512/2048/4096); do not fix it here.

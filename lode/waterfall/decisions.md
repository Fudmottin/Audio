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

- **Builds** warning-free under `-Werror`; links libaudio + Boost
  program_options.
- **All 84 libaudio unit tests pass.**
- **`--window-size` validation verified:** 1048 (non-power-of-two) is
  rejected with exit 1 and a clear message; 1024 and 2048 both run to
  completion on a generated tone. `FFT`'s constructor validation (added to
  libaudio) covers any other future caller: a bad size throws
  `std::invalid_argument` instead of letting aubio abort the process (see
  `lode/libaudio/decisions.md`).
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

## Video renderer noise floor (`--floor`) — added for `waterfall_video.py`

A **display-only hard noise gate** in `waterfall_video.py` (the C++ `waterfall`
tool is untouched and stays lossless). It zeros every sample below a dB floor,
removing the dim background noise a recording carries (hiss, room noise, the
faint bleed under a note). It is the inverse of auto-scale: auto-scale lifted the
file's peak to `FFFF` (0 dB); the floor gates the *low* end.

**User-approved rule (exact):**
- `--floor` takes a **float dB**; the gate level is `floorDb = -abs(args.floor)`
  (so `--floor 60` and `--floor -60` both mean −60 dB; `--floor 0` is 0 dB =
  `FFFF` = the no-op gate).
- Map `floorDb` onto the 16-bit range with the **same** normalization
  `quantizeTo16bit` in main.cpp uses: `int_val = round(clamp((floorDb − (−60)) /
  (refDb − (−60)), 0, 1) * 65535)`, where `refDb` is read from the header.
- **Any sample strictly below `int_val` is set to 0** (`np.where(rows < int_val,
  0, rows)`). Display-only; works in both MIDI and PCM layouts.
- Reusing the tool's own dB↔16-bit map guarantees a floor set in dB lands on
  exactly the integer the tool would have written, so a −30 dB floor is −30 dB.

**Agent decisions (flag: NOT user-confirmed — confirm before relying on them):**
- **Placement: after the equalizer stack, before color mapping.** The user did
  not specify ordering vs EQ; inferred from "a floor cleans noise the EQ's
  peak-normalization surfaced." `main()` applies `apply_floor(display_rows, ...)`
  immediately after `apply_eq_stack`. If the user wants the floor first (before
  EQ), move the block above the EQ block — `resolve_ref_db`/`apply_floor` are
  self-contained so it is a two-line relocation. **Confirm with the user.**
- **`refDb` fallback** (`resolve_ref_db`): the C++ tool always emits `refDb=`, so
  this is defensive only. For a header lacking a usable `refDb` (hand-crafted /
  legacy text file), the reference is derived as `20*log10(max/65535)` from the
  file's peak (a peak-normalized file's peak is ~full scale → ~0 dB), with a
  warning; an all-zero file or a degenerate span (`refDb ≤ K_FLOOR_DB`) makes the
  gate a no-op. **Confirm the fallback is acceptable.**

**Implementation notes (for the next agent):**
- `K_FLOOR_DB = -60.0` constant (matches `kFloorDb` in main.cpp) sits in the
  constants block after `EQ_OVERALL_DB`.
- Two small helpers sit in the transform section: `resolve_ref_db(header, rows)`
  (read/fallback the reference) and `apply_floor(rows, floor_db, ref_db)` (map to
  int, zero-below). `apply_floor` is a pure static transform like the EQ
  transforms, so it composes cleanly.
- In `main()`: `--floor` defaults to `None` (off) → existing renders are
  byte-identical; it is applied only when `abs(args.floor) > 0.0`, and prints a
  one-line stderr note with the computed int (hex + decimal) and the count of
  samples zeroed.
- Verified: `py_compile` clean; the int mapping matches `quantizeTo16bit` exactly
  for `refDb ∈ {0, 5, 38}` across floors; zero-below is strict and exact;
  `--floor 0` is a no-op (guard `abs(0)==0`); `60 == -60` (symmetry); floor at
  the low end (`−60`) and a degenerate span are no-ops. End-to-end render on a
  generated MIDI sample (48 kHz, 1.2 s, `--window-size 2048` to dodge the 1024
  segfault) with `--floor 30` and with `--eq-per-note 6 --floor 24` produced
  valid H.264/AAC MP4s (video duration == audio duration). The user's chord test
  AIFF is not in the repo (copyright); test with `final-fantasy.aiff` or a
  generated tone.

## Video renderer color map (`--color`) + `--h-supersample` — added for `waterfall_video.py`

A **display-only** choice of false-color ramp plus a horizontal-anti-alias A/B
lever (C++ `waterfall` untouched; stays lossless). The user's stated goal was
the *FLIR / weather-radar / thermal* "false color" look — much more legible than
grayscale — and they had chosen HSV for it. We expose both ramps so the user can
A/B them, and a supersample flag to A/B distinct-color density.

**User-approved (explicit this turn):**
- `--color {hsv,ironbow}`, **default `hsv`** (so a bare render is byte-identical
  to before).
- `--ironbow` — a boolean **shorthand** for `--color ironbow`.
- `--h-supersample N`, **default 8** (the existing `H_SUPERSAMPLE` constant);
  validated `>= 1`.

**Ramp designs (agent-chosen, NOT user-confirmed on the specific stops — tasteable
in `IRONBOW_STOPS`):**
- **HSV** — the original arc-around-through-red: `t = sample/65535`,
  `hue = (HUE_START + HUE_SWEEP·t) % 360` (240°→60°), saturation 1. Ends on pure
  yellow. (Behavior unchanged; now just one branch of `sample_to_rgb`.)
- **Ironbow** — a **piecewise-linear RGB gradient** (not an HSV rotation): the
  9 stops `black→darkblue→blue→purple→magenta→red→orange→yellow→white` in
  `IRONBOW_STOPS`. Built once at import into a 256-entry `IRONBOW_LUT` via
  `build_ironbow_lut()` (position `i/255`, `searchsorted` for the bracketing
  stops, per-channel lerp). Sampled per 8-bit brightness in `sample_to_rgb` by a
  single gather (`IRONBOW_LUT[idx]`) — zero per-frame cost beyond the gather.
  Rationale over HSV: a multi-channel linear-in-value ramp uses all three channels
  monotonically, so the full 0..255 spans more distinct colors with less low-end
  banding, and ends on FLIR's signature hot **white** (all channels 255) rather
  than pure yellow.

**Precedence:** `color = "ironbow" if (args.ironbow and args.color == "hsv")
else args.color` — i.e. `--ironbow` implies ironbow *only when no explicit
`--color` was given*; an explicit `--color` always wins. (Verified all four
flag combinations.)

**Wiring:** `sample_to_rgb(samples, color="hsv")` and
`render_waterfall_image(rows, reversed_, color="hsv")` take the ramp; `main()`
computes `color` and passes it, and threads `args.h_supersample` into
`build_warped(ss=...)` (replacing the hardcoded `H_SUPERSAMPLE`) and the two
layout log lines.

**Implementation notes (for the next agent):**
- Both ramps are a *linear* map of the dB-quantized value, so neither touches
  the data — purely coloring. The whole path is still a 1-D curve through RGB;
  `--h-supersample` is the real lever for *more distinct colors* (block-mean of
  adjacent columns with slightly different colors) at the cost of softness.
- Verified: `py_compile` clean; `IRONBOW_LUT` is (256,3) float, `[0]=black`,
  `[255]=white`, monotonic warm core; `sample_to_rgb` gives black/white at the
  extremes for ironbow and FFFF→(1,1,0) yellow for hsv (unchanged);
  precedence table passes all 4 combos. End-to-end renders on a generated 2.5 s
  chord sample (`--window-size 2048`) in hsv/ss8, ironbow/ss8 (`--ironbow`), and
  ironbow/ss16 (`--color ironbow --h-supersample 16`) all produced valid
  H.264/AAC MP4s (256x128, duration == audio). The user's chord test AIFF is not
  in the repo; A/B against `final-fantasy.aiff` or a generated tone.

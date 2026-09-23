#!/usr/bin/env python3
"""
waterfall_video.py — render a waterfall text file into a scrolling MP4 video.

Reads the text output produced by the `waterfall` C++ tool (one `name=value`
header line, then one line per time slice of space-separated 16-bit hex values)
and renders an animated, color waterfall that scrolls **downward** past an
imaginary playhead at the vertical center of the frame.

The text file lists rows in recording order (row 0 = the *start* of the
recording). For a downward scroll the rows are reversed so the *latest* sample
sits at the top of the image; the image then moves down, newest rows entering
at the top and oldest leaving at the bottom.

The playhead is the **onset line**: a zero-duration instant of time drawn as a
1-px line at the vertical center. The sounding band is **edge-anchored** to it
rather than centered on it — at t=0 the *first* band's **bottom edge** sits on
the playhead (so the start of the recording plays at the start of the clip),
and the video ends when the *last* band's **top edge** reaches the playhead,
i.e. the moment the band has fully passed below the line and there is no more
audio. Scroll speed is the image's full height divided by the audio's duration,
so the video's duration equals the audio file's duration; the `hopSize /
sampleRate` value from the header is read only to label each row's duration.

Rendering:
    Each frame is a color-mapped grid built with numpy (no Pillow, no
    ImageMagick). The whole waterfall is pre-rendered to a small (num_rows x
    num_cols x 3) RGB image ONCE, color-mapped. For each output frame we slice
    the visible row band out of that pre-rendered image, mean-anti-alias the
    vertical scroll (`V_SUPERSAMPLE` sub-rows) and the horizontal axis
    (a log-frequency warp, `H_SUPERSAMPLE`-wide buffer block-averaged to the
    output width), stretch the result to the requested (width, height), and
    pipe the raw bytes to FFmpeg's stdin. FFmpeg encodes H.264 / MP4 at 30 fps
    and muxes the audio.

Color mapping (per spec):
    value 0      -> black
    value 65535  -> full yellow
The hue travels along an arc of the HSV color wheel: from blue, arcing around
through red, to yellow. The HSV *value* channel is proportional to the sample
(brightness). See `hue_for_value` and the HUE_START / HUE_SWEEP constants below
to taste the arc.

Usage:
    python3 waterfall_video.py <text_file> <audio_file> <height> <width> [-o out.mp4]

    <text_file>   waterfall output (from the `waterfall` tool)
    <audio_file>  the same recording the waterfall was made from
    <height>      output frame height in pixels
    <width>       output frame width in pixels
    -o/--output   output video path (default: <text_file>.mp4)

The script reads the waterfall header's `mode` key to pick a display layout:

  - **MIDI** (default when present): the 128 MIDI notes are given *equal*
    horizontal screen width, and each note's internal bands are laid out on a
    logarithmic (equal-tempered) frequency scale so the intra-note pitch
    spacing is perceptually correct. This is the musically faithful display.
  - **PCM** (legacy, or when `mode` is absent): the old behavior — every column
    is mapped onto a *log frequency* axis across the full frame
    (F_MIN_HZ..F_MAX_HZ), so a linear-Hz waterfall is warped to be
    perceptually even. This is what made recordings of vocal singing so
    legible before the mode existed, and it is preserved bit-for-bit.

    Files produced before the `mode` key was added therefore render exactly
    as they always did.

Requires: numpy, ffmpeg on PATH.
"""

import argparse
import os
import shutil
import subprocess
import sys

import numpy as np


# ============================================================================
# Global settings
# ============================================================================

FPS = 30                    # Output video frame rate (fixed, per spec).
MAX_SAMPLE = 65535          # 16-bit unsigned full-scale.

# The "arc around through red" hue path.
#
# HSV hue is degrees in [0, 360). We want:
#   low value  -> blue
#   mid value  -> ... through ... (red at the apex of the arc)
#   high value -> yellow
#
# Blue is ~240deg. To "arc around through red" to yellow (60deg) we sweep
# hue *up* from blue: 240 -> 300 (magenta) -> 360/0 (red) -> 60 (yellow).
# That is a single continuous increasing sweep of 180deg that passes red at
# the midpoint of the arc. The value (brightness) ramps 0 -> 1 over the same
# interval, so the path starts as dark blue (black) and ends as full yellow.
HUE_START = 240.0   # deg — blue (the low end of the waterfall)
HUE_SWEEP = 180.0   # deg — total hue rotation from start to end (wraps past 360)

# Default vertical magnification applied to the waterfall image.
#
# The pre-rendered waterfall is stretched vertically by `DEFAULT_VSCALE`
# times the frame height, so each data row occupies more screen space and
# detail is revealed. To keep the audio in sync, the scroll speed is
# increased by the same factor so that one row still crosses the playhead
# every `row_dur_s` of real time. Tuning this constant (or passing `--vscale`
# on the command line, which is multiplied against it) lets the user pick
# the magnification. An effective scale above `num_rows / out_h` is
# pointless: the stretched image is taller than the frame and additional
# magnification only shows the same rows more zoomed in, not more of them.

# The default vertical magnification for a bare `--vscale 1.0` invocation.
# 2.0 means "the waterfall image is 2x taller than the frame" by default;
# scroll slope is scaled by the same factor to preserve audio sync.
# Bump this up to 3.0, 4.0, etc. if you want more detail by default.
# Pass `--vscale` on the command line to scale this value (see above).
DEFAULT_VSCALE = 2.0

# --- Horizontal (frequency) scaling ---
#
# The horizontal layout is mode-dependent (see `extract_mode`):
#
#   * MIDI — the 128 MIDI notes are given equal screen width. Each note's
#       internal bands are laid out on a logarithmic (equal-tempered) frequency
#       scale, so the intra-note pitch spacing is perceptually correct.
#
#   * PCM (legacy, default when `mode` is absent) — the columns are LINEAR in
#       frequency (128 MIDI notes x bands-per-note, sweeping 0..nyquist), so
#       most audible content sits on the left of the frame. To spread it the
#       way human hearing does, the display maps each column's center frequency
#       to screen x on a LOGARITHMIC axis:
#
#           x_frac = (ln(f) - ln(F_MIN)) / (ln(F_MAX) - ln(F_MIN))
#
#       This widens the low bands (where most musical energy lives) and
#       compresses the high bands, matching perceptual spacing.
#
# F_MIN / F_MAX bound the *PCM* log map. F_MIN is the low end (columns below
# it collapse to the left edge); F_MAX is the high end (the audible window) —
# the PCM map is clamped to it so ultrasonic content is dropped.
F_MIN_HZ = 16.0      # Hz — low end of the log frequency map (PCM mode)
F_MAX_HZ = 16000.0   # Hz — high end of the log frequency map (PCM mode)
# H_SUPERSAMPLE: supersample factor applied when down-sampling the temporary
#       horizontal buffer to the output width. Higher values reduce aliasing
#       artifacts in the high-frequency (compressed) region at the cost of
#       a few extra per-frame operations. 4 is a good default; 8 is sharper.
H_SUPERSAMPLE = 8
# --- Equalizer curves (display-only; see apply_eq_stack and the eq_* helpers) ---
#
# The equalizer is now *stackable*: a display-only transform applied to the raw
# 16-bit rows, composed in the order the flags appear on the command line
# (see apply_eq_stack). Each EQ_*_DB constant below is the default *depth* for
# one transform; each corresponding CLI flag takes an optional dB value that
# overrides the constant.
#
# EQ_ROLLOFF_DB (--eq-per-note): depth of the per-note roll-off. The transform
# peak-normalizes each note (so no value is boosted above the note's own
# maximum) and then applies a smooth bell that peaks at the note's *center band*
# and rolls off toward the note's low and high edges by up to this many dB. This
# is the "mids up, bass/treble down" shape of a graphic equalizer: it lifts the
# center of every note so that each note's core energy pops, while its edge
# bands (which pick up bleed from neighboring notes) recede. 6.0 dB is a good
# default; 0 disables the curve (leaving the flat peak-normalized result), and
# 10 gives a stronger taper.
EQ_ROLLOFF_DB = 6.0   # dB — default max roll-off at a note's edges (--eq-per-note)

# EQ_PER_OCTAVE_DB (--eq-per-octave): peak-normalize each *octave* (12 MIDI
# notes) to full scale, then apply the same raised-cosine bell across the octave
# with this edge roll-off. This balances the octaves (the low octaves carry far
# more average energy) at a coarser grain than --eq-per-note. 6.0 dB matches the
# per-note default; 0 disables the bell (leaving the flat peak-normalized
# octave result).
EQ_PER_OCTAVE_DB = 6.0   # dB — default edge roll-off when peaking per octave

# EQ_OVERALL_DB (--eq-over-all): a single whole-width linear dB tilt (peak-
# normalize the whole file, then lift the center and roll the two outer edges
# by up to this many dB). A coarse global balance: 0 (default) = a flat
# peak-normalized file, a small value brightens the mids across the full range.
EQ_OVERALL_DB = 0.0     # dB — default edge roll-off of the whole-width tilt

# K_FLOOR_DB: the noise floor the `waterfall` C++ tool used to map dB onto the
# 16-bit range. `quantizeTo16bit` in main.cpp normalizes dB as
# (db − K_FLOOR_DB) / (refDb − K_FLOOR_DB), where K_FLOOR_DB is −60 dB and
# refDb is the file's resolved reference (in the header). We reuse the same
# constant + mapping in the renderer's `--floor` gate so a floor in dB lands on
# exactly the integer the C++ tool would have written.
K_FLOOR_DB = -60.0   # dB — the low end of the dB→16-bit map (matches main.cpp)

# V_SUPERSAMPLE: supersample factor for the vertical scroll. The scroll offset
# is continuous (sub-pixel) but the source has one data row per pixel, so a
# plain nearest-neighbor sample steps in 1-row increments and looks blocky.
# Each output row instead samples `V_SUPERSAMPLE` sub-rows straddling its
# fractional position and block-means them, smoothing the vertical stepping
# the same way `H_SUPERSAMPLE` smooths the compressed high-frequency end.
# 8 matches the horizontal factor; raise for a smoother look at the cost of
# a little more per-frame work.
V_SUPERSAMPLE = 8


# ============================================================================
# Equalizer (display-only) — composable per-group transforms
# ============================================================================
#
# The equalizer sharpens a display that would otherwise read as a bright left
# side and a dim right side: the low octaves carry far more average energy, and
# each note's *edge* bands pick up bleed from neighboring notes, muddying the
# center. It is **display-only** (it never touches the waterfall text or data)
# and is applied to the raw 16-bit rows before color mapping. It is *stackable*:
# `apply_eq_stack` composes any combination of the `eq_*` transforms below in
# the order the user places them on the command line.
#
# Every transform follows the same two steps, applied over a *group* of columns
# (a group is 12 notes = an octave, 1 note, or the whole width):
#
#   1. **Peak-normalize** each group to full scale (0 dB = 65535). This
#      equalizes the groups against one another *and* enforces the core rule
#      that **no value is ever boosted above the group's own maximum** — a group
#      that already peaks at full scale is left flat. This pure scaling cannot
#      introduce brightness beyond a group's true peak.
#   2. Apply a smooth **raised-cosine bell** across the group: 0 dB (full) at
#      the group's *center*, rolling off toward its edges by up to `rolloff_db`
#      (the "mids at 0, bass/treble down" shape of a graphic equalizer). The
#      bell's maximum weight is 1.0 (at the center), so it only ever *reduces*
#      edge energy relative to the peak-normalized result — it sharpens the core
#      and lets the neighbor-bleed recede.
#
# Optional **energy preservation**: shaping a group's bell *redistributes* its
# total energy (the integral of the band amplitudes over the group). When a
# transform is asked to `preserve_energy`, it rescales the whole group by
# `raw_total / shaped_total` after shaping, restoring the group's total energy
# to exactly what peak-normalization set. Because the peak weight is 1.0 the
# rescale factor is always ≤ 1.0, so the bell *shape* (the pillow) is preserved
# and the group's new peak = `raw_total / sum(weights)` stays at or below full
# scale — nothing clips and the pillow never re-brightens.
#
# The transforms here are static (computed once from the file, applied to every
# row), so the scroll stays stable with no pumping artifacts.
# ---------------------------------------------------------------------------


def _group_peaks(rows, group, max_group):
    """Vectorized per-group peak across all rows.

    `group` is an int array of shape (num_cols,) assigning each column to a
    group 0..max_group. Returns a (max_group+1,) float array where entry `g`
    is the maximum of `rows[:, group == g]` (0.0 for an empty group). Uses
    `np.maximum.at` so no per-group Python loop is needed.
    """
    peaks = np.zeros(max_group + 1, dtype=np.float64)
    peaks[group] = np.maximum(peaks[group], rows.max(axis=0))
    return peaks


def _apply_bell(rows, unit, size, rolloff_db, preserve_energy=False):
    """Peak-normalize each group, apply a raised-cosine bell, clip, rescale.

    The single engine behind `eq_per_note` / `eq_per_octave` / `eq_over_all`.
    A *group* is a run of `size` contiguous columns (1 note, 12 notes = an
    octave, or the entire width). `unit` assigns each column to its position
    within a group; the bell's bell-curve is driven by `unit % size` (the unit's
    index within its group), with the raised-cosine peaking at the group's
    center. This lets the same math span 1, 12, or all of the columns.

    Steps: peak-normalize each group to full scale -> multiply by a
    raised-cosine bell that is 1.0 at the center and 10^(-rolloff/20) at the
    edges -> (optionally) rescale each group by its pre-shape / post-shape
    total energy to restore the group's total energy -> clip to 0..MAX_SAMPLE.

    rows            : (num_rows, num_cols) float64 array.
    unit            : (num_cols,) int array assigning each column to its unit
                      (band within a note, note within an octave, or column
                      within the whole width). The within-group position is
                      `unit % size`; the group is `col // size`.
    size            : int — number of units per group.
    rolloff_db      : float — edge roll-off in dB; <= 0 disables the bell.
    preserve_energy : bool — when True, rescale each group so its total energy
                      is unchanged by the bell (see the module docstring above).

    Returns a new (num_rows, num_cols) int64 array (0..MAX_SAMPLE).
    """
    rows = np.asarray(rows, dtype=np.float64)
    num_rows, num_cols = rows.shape
    if num_rows == 0 or num_cols == 0 or size < 1:
        return rows.astype(np.int64)

    col = np.arange(num_cols)
    group = col // size                       # (num_cols,)
    unit_in_group = unit % size               # position within its group
    max_group = int(group.max())

    # --- 1. Peak-normalize each group to full scale (static, from the file). ---
    target = float(MAX_SAMPLE)                 # 0 dB = full scale
    group_peak = _group_peaks(rows, group, max_group)
    norm_gain = np.zeros(max_group + 1)
    np.divide(target, group_peak, out=norm_gain, where=group_peak > 0.0)
    normalized = rows * norm_gain[group][None, :]   # (num_rows, num_cols)

    # --- 2. Raised-cosine bell: 1.0 at the center, 10^(-rolloff/20) at edges. ---
    # u in [-1, 1] from the unit's within-group position, 0.5 -> u=0 (bell peak).
    # The raised-cosine window W(u) = 0.5*(1+cos(pi*u)) is 1.0 at u=0 and 0.0
    # at u=±1; the +0.5 offset lands the *center unit* at u=0 for any unit count,
    # so the peak sits on a unit, not on a unit boundary.
    u = 2.0 * (unit_in_group + 0.5) / size - 1.0
    w = 0.5 * (1.0 + np.cos(np.pi * u))        # 1.0 at center, 0.0 at edges
    if rolloff_db <= 0.0:
        bell = np.ones(num_cols)               # disabled -> flat
    else:
        edge = 10.0 ** (-rolloff_db / 20.0)    # linear amplitude of the edge
        bell = 1.0 + (edge - 1.0) * w          # center->1, edges->`edge`

    shaped = normalized * bell[None, :]

    # --- 3. Optional energy-preserving rescale (a pure per-group scale). ---
    if preserve_energy:
        # Each group is rescaled so its *post-shape* total equals its
        # *peak-normalized* total (the bell only redistributes within a group,
        # so this restores the group's energy to exactly the peak-normalized
        # level). The peak weight is 1.0, so the factor is always <= 1.0 for a
        # non-flat bell and the group's new peak stays <= full scale.
        #
        # We pass the full 2-D arrays to _group_sums so the per-row loop sums
        # each row's columns into its group, giving (num_rows, max_group+1) —
        # NOT the 1-D per-row reductions, which would be misinterpreted by the
        # loop as one row per group and broadcast wrong.
        shaped_total_g = _group_sums(shaped, group, max_group)
        norm_total_g = _group_sums(normalized, group, max_group)
        ratio = np.ones((num_rows, max_group + 1))
        np.divide(norm_total_g, shaped_total_g, out=ratio,
                  where=shaped_total_g > 0.0)
        # `ratio` is already (num_rows, max_group+1); selecting `ratio[:, group]`
        # gathers each column's group ratio, giving (num_rows, num_cols) — one
        # scale factor per (row, column), matching `shaped`. No extra axis.
        shaped = shaped * ratio[:, group]

    return np.clip(shaped, 0.0, float(MAX_SAMPLE)).astype(np.int64)


def _group_sums(arr, group, max_group):
    """Per-group column sums: (num_rows, max_group+1) array of `arr` summed per
    group (a vectorized sibling of `_group_peaks` for the energy-preserving
    rescale in `_apply_bell`)."""
    num_rows = arr.shape[0]
    out = np.zeros((num_rows, max_group + 1), dtype=np.float64)
    # Accumulate each row's per-column sums into its group. A direct fancy-
    # index with a slice on axis 0 does not broadcast, so loop rows (a few
    # hundred at most) and use np.add.at on the 1-D group axis.
    for r in range(num_rows):
        np.add.at(out[r], group, arr[r])
    return out


def eq_per_octave(rows, bands_per_note, peak_db=EQ_PER_OCTAVE_DB):
    """Per-octave equalizer: peak-normalize each octave (12 notes), then bell.

    Balances the octaves against one another (the low octaves carry far more
    average energy) at a coarser grain than `eq_per_note`. Each column `c`
    belongs to MIDI note `c // bands_per_note`; an octave is 12 consecutive
    notes. `peak_db` sets the bell's edge roll-off (a positive value rolls the
    octave's edges down, lifting its center); 0 keeps the octave flat after
    peak-normalization (the raised-cosine at 0 dB is flat).
    """
    num_cols = rows.shape[1]
    return _apply_bell(
        rows,
        unit=np.arange(num_cols) // bands_per_note,   # note index (0..127)
        size=12 * bands_per_note,                    # 12 notes = one octave
        rolloff_db=peak_db,
        preserve_energy=False,
    )


def eq_per_note(rows, bands_per_note, rolloff_db=EQ_ROLLOFF_DB,
                preserve_energy=False):
    """Per-note equalizer: peak-normalize each note, then a raised-cosine bell.

    Each column `c` belongs to MIDI note `c // bands_per_note` and to band
    `c % bands_per_note`; 128 notes = the full MIDI range. The bell is 1.0 at
    the note's *center band* and rolls off by up to `rolloff_db` at the edges
    (a note's edge bands are where bleed from neighboring notes lives).

    `preserve_energy` restores each note's total energy to the peak-normalized
    level after shaping (see the module docstring); the pillow is preserved and
    nothing clips. Best with MIDI output, where columns map cleanly onto the
    128-note grid (for PCM output the note grouping is a linear-frequency
    sweep and less meaningful, but the transform still runs).
    """
    num_cols = rows.shape[1]
    return _apply_bell(
        rows,
        unit=np.arange(num_cols) % bands_per_note,         # band within the note
        size=bands_per_note,                               # one note = its bands
        rolloff_db=rolloff_db,
        preserve_energy=preserve_energy,
    )


def eq_over_all(rows, bands_per_note, db=EQ_OVERALL_DB):
    """Whole-width equalizer: peak-normalize the file, then a single bell.

    A coarse global balance across the *entire* width (one 128-note group): the
    loudest sample anywhere in the file reaches full scale, then a raised-
    cosine lifts the center of the width and rolls the two outer edges by up to
    `db`. `db = 0` (the default) leaves a flat peak-normalized file; a small
    positive `db` brightens the mids across the full range. This is the coarsest
    of the three transforms and is useful for a quick global lift/taper.

    `bands_per_note` is accepted for signature symmetry with the other
    transforms (so `apply_eq_stack` can call every transform uniformly) but is
    unused here: the whole-width group is the entire column range.
    """
    num_cols = rows.shape[1]
    return _apply_bell(
        rows,
        unit=np.arange(num_cols),                          # one unit per column
        size=num_cols,                                     # the whole width = one group
        rolloff_db=db,
        preserve_energy=False,
    )


# Registry mapping a CLI flag name to its transform, for apply_eq_stack.
_EQ_TRANSFORMS = {
    "per_octave": eq_per_octave,
    "per_note": eq_per_note,
    "over_all": eq_over_all,
}


def apply_eq(rows, bands_per_note, rolloff_db=EQ_ROLLOFF_DB):
    """Backward-compat: the per-note equalizer (now a stack of one transform)."""
    return eq_per_note(rows, bands_per_note, rolloff_db=rolloff_db,
                       preserve_energy=False)


def resolve_ref_db(header, rows):
    """Resolve the file's full-scale reference in dB (see `K_FLOOR_DB`).

    The C++ `waterfall` tool writes the *resolved* reference into the header as
    `refDb=...` (the auto-scaled file peak, or the `--ref-db` value). We read
    that so the `--floor` gate maps a dB floor onto the exact 16-bit integer
    the tool used when it wrote the rows (the same `K_FLOOR_DB..refDb` span
    that `quantizeTo16bit` normalizes over).

    Fallback for a header without a usable `refDb` (a hand-crafted or legacy
    text file): the file was normalized so its peak reached full scale (0 dB /
    `FFFF`), so we recover the reference as `20*log10(max/65535)` from the
    data. If the file's peak is already at full scale (typical after auto-scale)
    the recovered reference is ~0 dB; if the data is all zeros the gate is a
    no-op (there is nothing below a floor to zero). A warning is printed for any
    non-header reference so the user knows the map is approximate.

    Returns the reference in dB (a float; `K_FLOOR_DB` is the fixed low end).
    """
    try:
        ref = float(header.get("refDb"))
        if ref > K_FLOOR_DB:                  # finite, above the floor
            return ref
    except (TypeError, ValueError):
        pass

    # Defensive fallback: derive the reference from the file's own peak.
    mx = float(np.max(rows)) if rows.size else 0.0
    if mx <= 0.0:
        print("note: no refDb in header and the file has no energy; "
              "--floor has nothing to gate", file=sys.stderr)
        return K_FLOOR_DB                      # degenerate -> no-op gate
    ref = 20.0 * np.log10(mx / float(MAX_SAMPLE))
    print(f"warning: header has no usable refDb; deriving reference "
          f"{ref:.2f} dB from the file's peak sample ({int(mx)})", file=sys.stderr)
    return float(max(ref, K_FLOOR_DB))


def apply_eq_stack(rows, bands_per_note, transforms):
    """Compose a stack of equalizer transforms in order.

    `transforms` is a list of (name, kwargs) pairs, e.g.
    ``[("per_note", {"rolloff_db": 6.0}), ("per_octave", {"peak_db": 3.0})]``.
    Each name is a key of `_EQ_TRANSFORMS` (``per_note`` / ``per_octave`` /
    ``over_all``); a single ``"eq"`` entry is expanded to the per-note transform
    (the legacy ``--eq`` behavior). Each transform receives `bands_per_note`
    plus its kwargs; the transforms are applied in list order, so the last one
    listed is applied last (on top of the earlier ones' output).

    `bands_per_note` is passed to every transform; transforms that ignore it
    (e.g. `over_all`) simply don't read it. Returns the final int64 array.
    """
    out = np.asarray(rows, dtype=np.float64)
    for name, kwargs in transforms:
        if name == "eq":
            # Legacy preset: the per-note transform (the old --eq behavior).
            out = eq_per_note(out, bands_per_note)
        else:
            fn = _EQ_TRANSFORMS[name]
            out = fn(out, bands_per_note, **kwargs)
    return out


def apply_floor(rows, floor_db, ref_db):
    """Hard noise gate: zero every sample below a dB floor. (Display-only.)

    `floor_db` is a *signed* dB level, already resolved from the user's argument
    (`main` computes it as `-abs(args.floor)`, so `--floor 60` and `--floor -60`
    both read as −60 dB; `--floor 0` is 0 dB = full scale = `FFFF`). `ref_db` is
    the file's full-scale reference (0 dB = `FFFF`).

    The floor is mapped onto the 16-bit range with the *same* normalization
    `quantizeTo16bit` in main.cpp uses to write the file:

        int_val = round( clamp( (floor_db − K_FLOOR_DB) / (ref_db − K_FLOOR_DB), 0, 1 )
                         * 65535 )

    and every sample strictly below `int_val` is set to 0. The floor is the
    inverse of auto-scale: auto-scale lifted the file's peak to `FFFF`; the floor
    now gates the *low* end, removing the dim noise the equalizer's
    peak-normalization can surface. It is applied to the (possibly equalized)
    rows before color mapping; the waterfall text and data are untouched.

    rows      : (num_rows, num_cols) int array of 16-bit values.
    floor_db  : signed dB level of the gate (e.g. −60.0).
    ref_db    : the file's full-scale reference in dB.
    Returns an int64 array of the same shape with sub-floor samples zeroed.
    """
    rows = np.asarray(rows, dtype=np.int64)
    span = ref_db - K_FLOOR_DB
    if span <= 0.0:
        return rows                            # degenerate map -> no gate
    norm = (floor_db - K_FLOOR_DB) / span
    int_val = int(round(np.clip(norm, 0.0, 1.0) * float(MAX_SAMPLE)))
    if int_val <= 0:
        return rows                            # at/below the floor -> nothing to zero
    return np.where(rows < int_val, 0, rows)


def hue_for_value(t):
    """Map a normalized value t in [0,1] to an HSV hue in degrees.

    Linear sweep from HUE_START up by HUE_SWEEP degrees, wrapping at 360.
    t=0 -> blue (240), t=0.5 -> ~330 (red/magenta apex), t=1 -> 60 (yellow).
    """
    return (HUE_START + HUE_SWEEP * np.asarray(t)) % 360.0


def hsv_to_rgb(h, s, v):
    """Vectorized HSV->RGB. h in degrees, s and v in [0,1]. Returns (r,g,b) in [0,1]."""
    h = np.mod(h, 360.0) / 60.0          # sector scale: 0..6
    s = np.asarray(s, dtype=np.float64)
    v = np.asarray(v, dtype=np.float64)

    i = np.floor(h).astype(int)
    f = h - np.floor(h)

    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))

    r = np.empty_like(v)
    g = np.empty_like(v)
    b = np.empty_like(v)

    m = i == 0
    r[m], g[m], b[m] = v[m], t[m], p[m]
    m = i == 1
    r[m], g[m], b[m] = q[m], v[m], p[m]
    m = i == 2
    r[m], g[m], b[m] = p[m], v[m], t[m]
    m = i == 3
    r[m], g[m], b[m] = p[m], q[m], v[m]
    m = i == 4
    r[m], g[m], b[m] = t[m], p[m], v[m]
    m = i == 5
    r[m], g[m], b[m] = v[m], p[m], q[m]

    return r, g, b


def sample_to_rgb(samples):
    """Map an int array of 16-bit samples to an (N,3) float RGB array in [0,1]."""
    s = np.asarray(samples, dtype=np.int64)
    t = np.clip(s / float(MAX_SAMPLE), 0.0, 1.0)   # normalized value
    h = hue_for_value(t)
    r, g, b = hsv_to_rgb(h, np.ones_like(t), t)   # saturation = 1 (v=0 -> black)
    return np.stack([r, g, b], axis=1)


def render_waterfall_image(rows, reversed_):
    """Color-map the whole waterfall once.

    rows: (num_rows, num_cols) int array of 16-bit values.
    reversed_: when True, flip the row order so the array's row 0 is the
        *latest* sample (the original last row). The text file lists rows in
        recording order (row 0 = earliest); for a downward scroll we want the
        newest data to sit at the image top, hence the flip.

    Returns: (num_rows, num_cols, 3) uint8 RGB array (one pixel per cell).
    """
    if reversed_:
        rows = rows[::-1]
    flat = rows.ravel()
    rgb = sample_to_rgb(flat)                       # (N,3) float [0,1]
    return (rgb * 255.0).clip(0, 255).astype(np.uint8).reshape(rows.shape + (3,))


# ============================================================================
# Waterfall text parsing
# ============================================================================

def parse_header(line):
    """Parse the first `name=value` line into a dict (values as str)."""
    out = {}
    for tok in line.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            out[k] = v
    return out


def midi_note_frequency(note):
    """True equal-tempered frequency (Hz) of MIDI note `note`.

    440 * 2 ** ((note - 69) / 12), anchored at A4 = note 69 = 440 Hz. This is
    the physically correct pitch-to-frequency relationship and is what lets
    MIDI mode lay out a note's bands on a perceptually (logarithmically) even
    scale.
    """
    return 440.0 * (2.0 ** ((note - 69.0) / 12.0))


def extract_mode(header):
    """Resolve the display mode from the waterfall header.

    Returns ``"MIDI"`` or ``"PCM"``.

    The `waterfall` tool writes ``mode=MIDI`` or ``mode=PCM`` into the header.
    A consumer that does *not* find a ``mode`` key should assume **PCM** — the
    legacy linear behavior — so output produced before the mode was introduced
    stays interpretable (and renders exactly as it always did).
    """
    raw = str(header.get("mode", "")).strip().upper()
    if raw == "MIDI":
        return "MIDI"
    if raw == "PCM":
        return "PCM"
    # Missing / unknown value -> legacy PCM behavior.
    return "PCM"


def load_waterfall(path):
    """Load the waterfall text file.

    Returns (header, rows) where header is a dict and rows is a 2D int64 array
    of shape (num_rows, num_columns) of 16-bit sample values.
    """
    with open(path, "r") as f:
        first = f.readline()
        header = parse_header(first)

        rows = []
        ncols = int(header.get("numColumns", 0))
        for line in f:
            toks = line.split()
            if not toks:
                continue
            try:
                vals = [int(x, 16) for x in toks]
            except ValueError:
                break
            rows.append(vals)

    if not rows:
        raise SystemExit(f"error: no data rows found in {path}")

    ncols_actual = len(rows[0])
    if ncols and ncols != ncols_actual:
        print(f"warning: header says numColumns={ncols} but rows have {ncols_actual}; "
              f"using {ncols_actual}", file=sys.stderr)

    arr = np.asarray(rows, dtype=np.int64)
    if arr.ndim != 2:
        raise SystemExit("error: malformed waterfall data (non-rectangular rows)")
    return header, arr


def extract_col_freqs(header, num_cols):
    """Build the (num_cols,) float array of center frequencies from the header.

    The `waterfall` header carries one center frequency per column as
    `col0=... col1=... ... col{N-1}=...`, where N is `numColumns`. We pull
    those keys out of the parsed header dict and return them as a float array.
    If the header is missing any of the `col_*` keys (e.g. a hand-crafted or
    legacy text file), we fall back to a *linear* sweep from 0 to nyquist
    across the columns, so the log warp degrades gracefully to a plain linear
    spread rather than failing.

    Returns (col_freq_hz, nyquist_hz) where `nyquist_hz` is read from the
    header's `nyquistHz` key (or derived as `2 * max(col_freq)` if missing).
    """
    cols = []
    for c in range(num_cols):
        key = f"col{c}"
        if key in header:
            try:
                cols.append(float(header[key]))
            except (ValueError, TypeError):
                cols.append(0.0)
        else:
            cols.append(0.0)
    col_freq = np.asarray(cols, dtype=np.float64)

    ny = float(header.get("nyquistHz", 0)) or 0.0
    if ny <= 0.0:
        ny = 2.0 * float(col_freq.max()) if col_freq.size else 0.0
    if ny > 0.0:
        # If the header's col_* keys are all zero (fallback), build a linear
        # sweep across 0..nyquist so the log map has something to work with.
        if col_freq.max() <= 0.0:
            col_freq = np.linspace(0.0, ny, num_cols)
    return col_freq, ny


# ============================================================================
# Audio helpers
# ============================================================================

def audio_duration(path):
    """Return the audio file duration in seconds via ffprobe."""
    out = subprocess.check_output(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        stderr=subprocess.STDOUT)
    return float(out.decode().strip())


def has_audio_stream(path):
    """Return True if the media file contains at least one audio stream."""
    out = subprocess.check_output(
        ["ffprobe", "-v", "error", "-select_streams", "a",
         "-show_entries", "stream=index", "-of", "default=noprint_wrappers=1:nokey=1",
         path], stderr=subprocess.STDOUT)
    return len(out.strip()) > 0


# ============================================================================
# Frame synthesis
# ============================================================================

def log_xfrac(col_freq_hz, f_min, f_max):
    """Map each column's center frequency to a fractional x position in [0,1].

    `x = (ln(f) - ln(f_min)) / (ln(f_max) - ln(f_min))`, clipped to [0, 1].
    Columns below `f_min` map to 0 (left edge); columns above `f_max` map to 1
    (right edge). Frequencies at or below 0 Hz (which never occurs for a real
    FFT) are clamped to `f_min` to keep `ln` finite.

    This is the perceptual (human-hearing) spacing: equal *ratios* of frequency
    get equal screen width, so the low bands — where most musical energy lives —
    are wider than the high bands.

    col_freq_hz : (num_cols,) float array of center frequencies in Hz.
    """
    f = np.clip(np.asarray(col_freq_hz, dtype=np.float64), f_min, f_max)
    x = (np.log(f) - np.log(f_min)) / (np.log(f_max) - np.log(f_min))
    return np.clip(x, 0.0, 1.0)


def midi_band_left_edges(bands_per_note, num_cols):
    """Fractional left edge of each MIDI column, tiling [0, 1] in equal bands.

    The 128 MIDI notes are given *equal* horizontal screen width (a note spans a
    12th of an octave, so equal notes get equal width on a log axis). Within each
    note, the `bands_per_note` bands are placed at **equal pixel width**.

    Equal-width band edges are the right choice here: because the MIDI columns'
    center frequencies (from `waterfall`) are already *log-even* within each
    note, equal pixel widths reproduce the correct equal-tempered *frequency*
    spacing — a band that spans a 2× frequency ratio (the lowest band of an
    octave) is not over-widened the way a linear-Hz split would be. Equal-width
    bands also match the legacy PCM look, so familiar displays are unchanged.

    Column `c` is assigned to note `c // bands_per_note` and to band
    `c % bands_per_note`. Note `n` occupies the x-range
    `[n / 128, (n+1) / 128)`, and its bands tile that range in equal
    `bands_per_note` pieces, so the returned left edges monotonically tile
    [0, 1] (the 128th note extends to 1.0).

    bands_per_note : int — number of bands per MIDI note (from `bandsPerNote`).
    num_cols       : int — number of columns to lay out.
    Returns a float array of shape (num_cols,) with values in [0, 1).
    """
    if bands_per_note < 1:
        bands_per_note = 1
    col = np.arange(num_cols, dtype=np.float64)
    note = np.floor(col / bands_per_note)
    band = col - np.floor(col / bands_per_note) * bands_per_note
    # 128 notes tile [0, 1]; each note's bands tile its 1/128 share equally.
    left = (note + band / bands_per_note) / 128.0
    return np.clip(left, 0.0, 1.0)


def build_warped(image, left_edges, out_w, ss=1):
    """Horizontally warp the color-mapped waterfall onto the output-width grid.

    `image` is the color-mapped waterfall, shape `(n_rows, num_cols, 3)` uint8
    (one pixel per data column, 16-bit values mapped to RGB).

    `left_edges` is a `(num_cols,)` float array giving, for each column `c`, the
    left edge of its band as a fractional position in [0, 1]. Column `c`
    occupies the band from `left_edges[c]` to `left_edges[c + 1]` (the first
    starts at 0, the last extends to 1), so the bands tile [0, 1]. The caller
    supplies these edges to choose the layout:

      * **PCM**  — edges derived from each column's log-mapped *center frequency*
                   (a perceptual, irregular-width sweep; see `log_xfrac`).
      * **MIDI** — edges that tile [0, 1] in *equal* bands, one per column
                   (`midi_band_left_edges`); each of the 128 notes gets equal
                   width and each note's bands are equal-width within it.

    Each band is painted into a temporary buffer `ss` times wider than the
    output using the containing source column, then the buffer is downsampled to
    the output width by block **mean**. This keeps sub-pixel columns visible
    (their energy is averaged into the nearest output pixel) instead of
    vanishing.

    Returns: `(n_rows, out_w, 3)` uint8 — the warped waterfall ready for the
    vertical sampling in `render_frame`.

    left_edges : (num_cols,) fractional left edge of each column, in [0, 1].
    out_w      : output frame width in pixels (must be even for yuv420p).
    ss         : horizontal supersample factor (integer >= 1). 4 is a good
                 default; 8 is sharper. 1 disables anti-aliasing (fast, but
                 sub-pixel columns will flicker/vanish).
    """
    n_rows, num_cols, channels = image.shape
    left_edges = np.asarray(left_edges, dtype=np.float64)
    if left_edges.size != num_cols:
        raise SystemExit(
            f"error: {left_edges.size} layout edges for {num_cols} columns")
    left_edges = np.clip(left_edges, 0.0, 1.0)

    # For each temp-buffer x position, the containing source column. Bands are
    # contiguous over [0, 1]; searchsorted on the *left* edges finds the index
    # of the first left-edge > xpix, minus 1, i.e. the band containing xpix.
    ssbuf_w = max(out_w * ss, 1)
    xpix = np.arange(ssbuf_w, dtype=np.float64) / ssbuf_w   # [0, 1)
    src_col = np.searchsorted(left_edges, xpix, side='left') - 1
    src_col = np.clip(src_col, 0, num_cols - 1)

    # Gather the containing source column for every buffer pixel, for every row.
    warped = image[:, src_col, :]                    # (n_rows, ssbuf_w, 3)

    # Block-mean downsample to the output width: each output pixel is the mean
    # of the `ss` buffer pixels that map to it. A column in a sub-pixel band
    # still contributes its energy (averaged), so thin high-freq columns read
    # as dimmer pixels rather than disappearing.
    block = warped.reshape(n_rows, out_w, ss, channels)
    return block.mean(axis=2).astype(np.uint8)       # (n_rows, out_w, 3)


def render_frame(warped, img_top, height, n_rows, playhead_y, v_scale=1.0,
                 vss=1):
    """Compose one output frame (height x width x 3, uint8) for a given scroll.

    `warped` is the whole waterfall already warped horizontally onto the output
    width grid (shape `n_rows x width x 3`). Each data row occupies `v_scale *
    height / n_rows` output pixels. `img_top` is the y-coordinate (in output
    pixels) of the top of the image's row 0. The visible window is a single
    vectorized nearest-neighbor sample of that image along the vertical axis.
    (With a positive slope, `img_top` grows over time, so the image — and the
    newest rows at its top — scroll *downward*.)

    The horizontal log-frequency warp and mean anti-aliasing are applied once
    up front (see `build_warped`); `render_frame` does the vertical sampling
    (nearest-neighbor when `vss` is 1, or a block-mean of `vss` sub-rows when
    `vss` > 1 for a smoother scroll). `width` is carried in `warped.shape[1]`.

    warped     : (n_rows, width, 3) uint8 — color-mapped + log-warped waterfall.
    img_top    : float — y of the top of image row 0 in output pixels.
    v_scale    : vertical magnification factor (see DEFAULT_VSCALE).
    vss        : vertical supersample factor; 1 = nearest-neighbor, >1 = smooth.
    """
    img_h, width, _ = warped.shape
    fimg = warped.astype(np.float64)            # float for fractional sampling

    # Output pixel y -> image row coordinate (continuous).
    # `v_scale` magnifies the image vertically by that factor relative to the
    # frame; each row therefore occupies `v_scale * height / n_rows` pixels.
    # With v_scale=1 the image exactly fills the frame height.
    row_scale = n_rows / (float(height) * v_scale)  # image rows per output px

    if vss <= 1:
        # No vertical anti-aliasing: a single nearest-neighbor row sample.
        y_idx = (np.arange(height) - img_top) * row_scale   # (height,)
        row_i = np.clip(np.round(y_idx).astype(np.int64), 0, img_h - 1)
        frame = warped[row_i]                       # (height, width, 3)
    else:
        # Fractional vertical anti-aliasing: for each output row, straddle its
        # fractional position with `vss` sub-row samples and block-mean them.
        # The scroll is continuous (img_top is a float), so the sub-row grid
        # shifts smoothly between frames; averaging `vss` neighboring data rows
        # damps the 1-row stepping that nearest-neighbor would produce.
        # Edge rows (where the grid spills past the image) are clamped to the
        # boundary row, matching the horizontal mean behavior.
        base = (np.arange(height) - img_top) * row_scale   # (height,)
        offsets = np.linspace(0.0, 1.0, vss) - 0.5        # (vss,) within-row
        samples = base[:, None] + offsets[None, :]        # (height, vss)
        row_i = np.clip(np.round(samples).astype(np.int64), 0, img_h - 1)
        # Advanced index on axis 0 -> (height, vss, width, 3); mean over axis 1
        # (the vss sub-rows) collapses to (height, width, 3).
        frame = fimg[row_i, :, :].mean(axis=1)         # (height, width, 3)
        frame = frame.astype(np.uint8)

    # Playhead: a faint horizontal line at the vertical center.
    py = int(playhead_y)
    if 0 <= py < height:
        frame[py] = np.maximum(frame[py], 120)

    return frame


# ============================================================================
# Main
# ============================================================================

def main():
    ap = argparse.ArgumentParser(
        description="Render a waterfall text file into a scrolling MP4 video.")
    ap.add_argument("text_file", help="waterfall output text file")
    ap.add_argument("audio_file", help="audio file the waterfall was made from")
    ap.add_argument("height", type=int, help="output frame height (px)")
    ap.add_argument("width", type=int, help="output frame width (px)")
    ap.add_argument("-o", "--output", default=None,
                    help="output video path (default: <text_file>.mp4)")
    ap.add_argument("--vscale", type=float, default=1.0,
                    help=("vertical magnification of the waterfall image, "
                          "multiplied against DEFAULT_VSCALE. 1.0 (default) "
                          "yields DEFAULT_VSCALE; 0.5 halves it, 2.0 doubles it. "
                          "The scroll speed is scaled identically so that audio "
                          "stays in sync and the video still ends when the data "
                          "tail passes the playhead. Must be positive."))
    # --- Stackable equalizer flags (display-only; see apply_eq_stack). ---
    # Each flag takes an optional dB value (the constant at the top of the
    # file is the default). `--preserve-energy` rescales each group after
    # shaping so its total energy is unchanged (the bell pillow is preserved).
    # `--eq` is a shorthand for the legacy per-note behavior (--eq-per-note).
    ap.add_argument("--eq-per-note", nargs="?", type=float,
                    default=None, const=None,
                    metavar="dB",
                    help=("per-note equalizer: peak-normalize each note, then "
                          "apply a center bell (mids at 0 dB, edges down by up "
                          "to dB) so each note's core pops and its edge bands "
                          "recede. Optional dB value overrides EQ_ROLLOFF_DB. "
                          "Best with MIDI output. (default off; the optional dB "
                          "is EQ_ROLLOFF_DB)"))
    ap.add_argument("--eq-per-octave", nargs="?", type=float,
                    default=None, const=None,
                    metavar="dB",
                    help=("per-octave equalizer: peak-normalize each octave, "
                          "then a center bell across the octave. Balances the "
                          "octaves (the low ones carry far more energy). "
                          "Optional dB value overrides EQ_PER_OCTAVE_DB. "
                          "(default off)"))
    ap.add_argument("--eq-over-all", nargs="?", type=float,
                    default=None, const=None,
                    metavar="dB",
                    help=("whole-width equalizer: peak-normalize the file, then "
                          "a single bell across the full width. A coarse global "
                          "balance. Optional dB value overrides EQ_OVERALL_DB "
                          "(default 0, which leaves a flat peak-normalized "
                          "file). (default off)"))
    ap.add_argument("--preserve-energy", action="store_true", default=False,
                    help=("rescale each note after shaping so its total energy "
                          "is unchanged by the bell (the note's energy budget is "
                          "restored; nothing clips). Applies to --eq-per-note. "
                          "(default off)"))
    ap.add_argument("--eq", action="store_true", default=False,
                    help=("shorthand for --eq-per-note (the legacy per-note "
                          "equalizer). (default off)"))
    ap.add_argument("--floor", type=float, default=None,
                    metavar="dB",
                    help=("hard noise gate: zero out any sample whose level is "
                          "below this floor. The argument is a dB magnitude and "
                          "is interpreted as -abs(dB) (so --floor 60 and "
                          "--floor -60 both mean -60 dB); the floor is mapped to "
                          "a 16-bit integer using the file's refDb, and any "
                          "sample below that integer is zeroed. Display-only. "
                          "(default off; 0 disables the gate)"))
    args = ap.parse_args()

    if args.vscale <= 0:
        ap.error("--vscale must be positive")

    if args.height < 16 or args.width < 16:
        ap.error("height and width must each be at least 16")

    if shutil.which("ffmpeg") is None:
        sys.exit("error: ffmpeg not found on PATH")
    if shutil.which("ffprobe") is None:
        sys.exit("error: ffprobe not found on PATH")
    if not os.path.exists(args.text_file):
        sys.exit(f"error: text file not found: {args.text_file}")
    if not os.path.exists(args.audio_file):
        sys.exit(f"error: audio file not found: {args.audio_file}")

    header, rows = load_waterfall(args.text_file)
    num_rows, num_cols = rows.shape

    out_path = args.output
    if out_path is None:
        base, _ = os.path.splitext(args.text_file)
        out_path = base + ".mp4"

    # --- Dimensions: yuv420p requires even width and height. ---
    out_h = args.height if args.height % 2 == 0 else args.height - 1
    out_w = args.width if args.width % 2 == 0 else args.width - 1
    if out_h < 16 or out_w < 16:
        ap.error("even height/width must each be at least 16")
    if (out_h, out_w) != (args.height, args.width):
        print(f"note: rounding dimensions to even ({out_w}x{out_h}) for yuv420p",
              file=sys.stderr)

    # --- Timing. ---
    sample_rate = float(header.get("sampleRate", 0)) or 0.0
    hop_size    = float(header.get("hopSize", 0)) or 0.0

    # The C++ tool advances by one hop per row, so the REAL time each row
    # represents is hopSize / sampleRate seconds. The `timeSliceMs` header
    # value is only a user-facing label and may not equal the actual hop
    # duration, so we do NOT use it for scroll timing.
    row_dur_s = (hop_size / sample_rate) if (hop_size > 0 and sample_rate > 0) else 0.0

    try:
        audio_dur = audio_duration(args.audio_file)
    except Exception as e:
        print(f"warning: could not probe audio duration ({e}); using row sum",
              file=sys.stderr)
        audio_dur = num_rows * row_dur_s if row_dur_s > 0 else 0.0

    if row_dur_s <= 0:
        row_dur_s = audio_dur / num_rows if num_rows else 1.0 / FPS

    # Effective vertical magnification: the named default (tweakable at the
    # top of this file) times the user's --vscale multiplier. For --vscale 1.0
    # this is just DEFAULT_VSCALE.
    v_scale = DEFAULT_VSCALE * args.vscale

    row_h = out_h * v_scale / float(num_rows)
    playhead = out_h / 2.0

    # Linear scroll model. We reverse the rows so the image's row 0 is the
    # *latest* sample; the image then scrolls *downward* (positive slope) with
    # the newest data entering at the top and the oldest leaving at the bottom.
    # The head — the first row of the recording, i.e. the image's *last* row —
    # therefore sits at the bottom of the image. At t=0 we place that last row
    # on the playhead, so the *start of the recording* plays at the start of
    # the clip and the row under the playhead is always the audio heard at t.
    #
    # Scroll speed (px/s). The image travels its full height (num_rows * row_h)
    # over the audio's duration, so the first band's bottom edge reaches the
    # playhead at t=0 and the last band's top edge reaches it at the end.
    slope = (num_rows * row_h) / audio_dur if audio_dur > 0 else 1.0   # px/s

    # The playhead is the onset line: the bottom edge of the sounding band.
    # `img_top` is the frame-y of the image's row 0 (the *newest* data, at the
    # top). The first band is the image's *last* (bottom) row; we place its
    # bottom edge on the playhead (not its center) so the band sits entirely
    # above the line at t=0. The image may be taller than the frame (the normal
    # case) and is simply cropped.
    img_top_start = playhead - (num_rows - 1) * row_h - row_h / 2.0

    # The video ends when the *top edge of the final band* reaches the playhead,
    # i.e. when the last band has fully passed below the line and there is no
    # more audio. With the full-height slope above, that instant is exactly the
    # audio's duration.
    t_video_end = max(0.0, audio_dur)

    # Audio runs from 0 to the end of the clip (the audio's full duration): it
    # starts in sync with the head on the playhead and ends exactly when the
    # tail passes the playhead. `atrim=start=0:end=<dur>` keys on the *sample
    # count* — which sidesteps the unreliable (NaN) audio timestamp `t` in a
    # multi-input rawvideo pipe.
    audio_filter = f"atrim=start=0:end={t_video_end:.6f}"

    total_frames = max(1, int(round(t_video_end * FPS)))

    print(f"waterfall: {num_rows} rows x {num_cols} cols, "
          f"row_dur={row_dur_s:.4f}s, audio={audio_dur:.3f}s", file=sys.stderr)
    print(f"video: {out_w}x{out_h} @ {FPS}fps, {total_frames} frames, "
          f"~{t_video_end:.2f}s", file=sys.stderr)
    print(f"audio: plays from 0.000s to {t_video_end:.3f}s "
          f"(ends when the tail passes the playhead)", file=sys.stderr)

    # --- Pre-render the waterfall image once (the perf win). ---
    # Reversed so the newest sample is the image's row 0 (top). See the
    # scroll model above for why.
    display_rows = rows

    # --- Equalizer (display-only). ---
    # Build a stack of (name, kwargs) transforms from the CLI flags. The
    # transforms are composed in a fixed coarse-to-fine order (whole width ->
    # octaves -> notes) regardless of how the user ordered the flags; each is
    # a pure static transform, so the composition is deterministic. `None`
    # means the flag was not passed (argparse's default); a bare flag (no dB
    # value) yields None here, which is resolved to the named constant below.
    transforms = []
    if args.eq_over_all is not None:
        transforms.append(("over_all", {"db": args.eq_over_all}))
    if args.eq_per_octave is not None:
        transforms.append(("per_octave", {"peak_db": args.eq_per_octave}))
    if args.eq_per_note is not None or args.eq:
        # `--preserve-energy` rescales each note after shaping so its total energy
        # is unchanged (the bell redistributes energy; this restores the budget).
        # It applies to the per-note transform (the one the user tunes for a note's
        # core); per-octave keeps the bell-shape-only behavior.
        transforms.append(("per_note", {"rolloff_db": args.eq_per_note,
                                        "preserve_energy": args.preserve_energy}))

    if transforms:
        bands_per_note = int(header.get("bandsPerNote", 8))
        # Each transform carries only its own dB key (a bare flag is None -> the
        # named constant). Resolve just that key so a transform never receives
        # a stray keyword (e.g. `over_all` has no `rolloff_db`).
        _dB_KEY = {"per_note": ("rolloff_db", EQ_ROLLOFF_DB),
                   "per_octave": ("peak_db", EQ_PER_OCTAVE_DB),
                   "over_all": ("db", EQ_OVERALL_DB)}
        for name, kwargs in transforms:
            key, const = _dB_KEY[name]
            if kwargs.get(key) is None:
                kwargs[key] = const
            desc = {"per_note": "per-note", "per_octave": "per-octave",
                    "over_all": "over-all"}[name]
            extra = " (energy-preserving)" if kwargs.get("preserve_energy") else ""
            print(f"applying {desc} equalizer{extra} ...", file=sys.stderr)
        display_rows = apply_eq_stack(rows, bands_per_note, transforms)

    # --- Noise floor (display-only). ---
    # A hard gate: zero every sample below the user's dB floor. Applied to the
    # final (possibly equalized) rows, right before color mapping, so it cleans
    # the low-level noise the equalizer's peak-normalization can surface without
    # fighting the EQ's shaping. `--floor` is a dB magnitude read as -abs(dB)
    # (0 = full scale = FFFF = no gate). The floor maps onto the 16-bit range
    # with the same normalization the C++ tool used to write the file, so a dB
    # floor lands on exactly the integer the tool would have written.
    if args.floor is not None and abs(args.floor) > 0.0:
        ref_db = resolve_ref_db(header, rows)
        floor_db = -abs(args.floor)
        before = (display_rows > 0).sum()
        display_rows = apply_floor(display_rows, floor_db, ref_db)
        after = (display_rows > 0).sum()
        span = ref_db - K_FLOOR_DB
        int_val = (int(round(np.clip((floor_db - K_FLOOR_DB) / span, 0.0, 1.0)
                              * float(MAX_SAMPLE))) if span > 0.0 else 0)
        print(f"applying floor gate at {floor_db:.1f} dB (int 0x{int_val:04X} = {int_val}); "
              f"zeroed {int(before - after)} of {int(before)} non-zero samples",
              file=sys.stderr)

    print("color-mapping waterfall ...", file=sys.stderr)
    full_image = render_waterfall_image(display_rows, reversed_=True)

    # --- Horizontal log-frequency warp (display-only) ---
    # The waterfall's columns are linear in frequency; for a human-friendly
    # display we map each column's center frequency onto screen x on a log
    # axis (F_MIN_HZ..F_MAX_HZ) and mean-anti-alias onto the output-width
    # grid. This widens the low bands (where most musical energy lives) and
    # compresses the high bands. The result is `warped`, shape
    # (num_rows, out_w, 3) uint8 — the same row order as `full_image` (newest
    # at row 0) but with the horizontal axis warped. See `build_warped` and
    # the F_MIN_HZ / F_MAX_HZ / H_SUPERSAMPLE constants above.
    mode = extract_mode(header)
    if mode == "MIDI":
        # MIDI layout: 128 equal-width notes, each subdivided into equal-width
        # bands (log-even frequency centers within a note, from `waterfall`).
        bands_per_note = int(header.get("bandsPerNote", 8))
        left_edges = midi_band_left_edges(bands_per_note, num_cols)
        print(f"MIDI layout: 128 equal notes x {bands_per_note} bands "
              f"(equal-width bands, ss={H_SUPERSAMPLE})", file=sys.stderr)
    else:
        # PCM layout (legacy, default when `mode` is absent): map each column's
        # center frequency onto a log axis (F_MIN_HZ..F_MAX_HZ) for a
        # perceptually even sweep; band edges come from the log-mapped centers.
        col_freq, nyquist_hz = extract_col_freqs(header, num_cols)
        x = log_xfrac(col_freq, F_MIN_HZ, F_MAX_HZ)
        left_edges = np.concatenate(([0.0], x[:-1]))
        print(f"PCM log-frequency map: {F_MIN_HZ:.0f} Hz .. {F_MAX_HZ:.0f} Hz "
              f"(nyquist {nyquist_hz:.0f} Hz, ss={H_SUPERSAMPLE})", file=sys.stderr)
    warped = build_warped(full_image, left_edges, out_w,
                          ss=H_SUPERSAMPLE)
    # Vertical supersample factor for the scroll (fractional sub-row block-mean,
    # see render_frame). 1 disables it; 8 matches the horizontal factor.
    vss = V_SUPERSAMPLE

    # --- FFmpeg command line. ---
    has_audio = has_audio_stream(args.audio_file)
    if not has_audio:
        print("warning: no audio stream in input; producing a silent video",
              file=sys.stderr)

    ffmpeg = ["ffmpeg", "-y",
              # Video input: raw RGB from stdin.
              "-f", "rawvideo", "-pixel_format", "rgb24",
              "-video_size", f"{out_w}x{out_h}",
              "-framerate", str(FPS),
              "-i", "-",
              # Audio input.
              "-i", args.audio_file]

    if has_audio:
        ffmpeg += ["-af", audio_filter]
    # Map: video always; audio only if present.
    ffmpeg += ["-map", "0:v"]
    if has_audio:
        ffmpeg += ["-map", "1:a",
                   "-c:a", "aac", "-b:a", "192k"]
    ffmpeg += ["-c:v", "libx264", "-pix_fmt", "yuv420p",
               "-profile:v", "high", "-level", "4.0",
               "-preset", "medium", "-crf", "20",
               "-movflags", "+faststart",
               out_path]

    proc = subprocess.Popen(ffmpeg, stdin=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    written = 0
    try:
        for fidx in range(total_frames):
            img_top = img_top_start + slope * (fidx / FPS)
            frame = render_frame(warped, img_top, out_h,
                                 num_rows, playhead, v_scale=v_scale,
                                 vss=vss)
            proc.stdin.write(frame.tobytes())
            written += 1
            if fidx % (FPS * 2) == 0:
                print(f"  frame {fidx}/{total_frames} "
                      f"({fidx / FPS:.2f}s)", file=sys.stderr)
    except (BrokenPipeError, KeyboardInterrupt) as e:
        print(f"warning: stopped early ({e})", file=sys.stderr)

    try:
        proc.stdin.close()
    except BrokenPipeError:
        pass
    _, err = proc.communicate()
    rc = proc.wait()
    if rc != 0:
        sys.stderr.write(err.decode(errors="replace"))
        sys.exit(rc)

    print(f"wrote {out_path} ({written} frames, {t_video_end:.2f}s)", file=sys.stderr)


if __name__ == "__main__":
    main()

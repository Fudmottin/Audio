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
at the top and oldest leaving at the bottom. The clip starts with the head of
the waterfall — the first row of the recording — aligned with the playhead, so
the *start of the recording* plays at the start of the video. The video ends
when the *tail of the data* (the oldest row) passes the playhead, which by the
sync model is exactly the audio's duration.

Because the scroll speed is tied to the *real* per-row audio duration
(`hopSize / sampleRate`), the row sitting on the playhead at time t is exactly
the audio you hear at time t.

Rendering:
    Each frame is a color-mapped grid built with numpy (no Pillow, no
    ImageMagick). The whole waterfall is pre-rendered to a small (num_rows x
    num_cols x 3) RGB image ONCE, color-mapped. For each output frame we slice
    the visible row band out of that pre-rendered image and stretch it to the
    requested (width, height) with numpy.repeat (nearest-neighbor), then pipe
    the raw bytes to FFmpeg's stdin. FFmpeg encodes H.264 / MP4 at 30 fps and
    muxes the audio.

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
# The waterfall's columns are LINEAR in frequency (128 MIDI notes x bands-per-
# note, sweeping 0..nyquist), so most audible content sits on the left of the
# frame. To spread it the way human hearing does, the display maps each
# column's center frequency to screen x on a LOGARITHMIC axis:
#
#     x_frac = (ln(f) - ln(F_MIN)) / (ln(F_MAX) - ln(F_MIN))
#
# This widens the low bands (where most musical energy lives) and compresses
# the high bands, matching perceptual spacing. The two bounds are the only
# tunables here.
#
# F_MIN: the low end of the audible map. Columns below this collapse to the
#       left edge (e.g. 20 Hz means sub-20 Hz bass is squished off-screen).
# F_MAX: the high end of the audible map (option "b": audible window).
#       The default is a constant 20 kHz; if the audio is 96 kHz+ (nyquist
#       above 20 kHz) we clamp to F_MAX = 20 kHz and drop ultrasonic content.
#       For 48 kHz audio (nyquist 24 kHz) we similarly clamp to 20 kHz, so
#       the 20-24 kHz ultrasonic strip (aliasing) is also dropped.
F_MIN_HZ = 16.0      # Hz — low end of the log frequency map
F_MAX_HZ = 16000.0   # Hz — high end of the log frequency map (audible window)
# H_SUPERSAMPLE: supersample factor applied when down-sampling the temporary
#       horizontal buffer to the output width. Higher values reduce aliasing
#       artifacts in the high-frequency (compressed) region at the cost of
#       a few extra per-frame operations. 4 is a good default; 8 is sharper.
H_SUPERSAMPLE = 8
# V_SUPERSAMPLE: supersample factor for the vertical scroll. The scroll offset
# is continuous (sub-pixel) but the source has one data row per pixel, so a
# plain nearest-neighbor sample steps in 1-row increments and looks blocky.
# Each output row instead samples `V_SUPERSAMPLE` sub-rows straddling its
# fractional position and block-means them, smoothing the vertical stepping
# the same way `H_SUPERSAMPLE` smooths the compressed high-frequency end.
# 8 matches the horizontal factor; raise for a smoother look at the cost of
# a little more per-frame work.
V_SUPERSAMPLE = 8


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


def build_warped(image, col_freq_hz, out_w, f_min=None, f_max=None,
                 ss=1):
    """Horizontally warp the color-mapped waterfall onto the output-width grid.

    `image` is the color-mapped waterfall, shape `(n_rows, num_cols, 3)` uint8
    (one pixel per data column, 16-bit values mapped to RGB).

    Each column `c` occupies a band on the horizontal axis from `x_{c-1}` to
    `x_c` (its own log-mapped center, bounded by its neighbors), giving the
    column an *irregular* width that widens in the low end and narrows in the
    high end. The band is painted into a temporary buffer `ss` times wider than
    the output using the containing source column, then the buffer is
    downsampled to the output width by block **mean**. This keeps sub-pixel
    columns visible (their energy is averaged into the nearest output pixel)
    instead of vanishing.

    Returns: `(n_rows, out_w, 3)` uint8 — the warped waterfall ready for the
    vertical sampling in `render_frame`.

    col_freq_hz : (num_cols,) center frequency of each column, Hz.
    out_w       : output frame width in pixels (must be even for yuv420p).
    ss          : horizontal supersample factor (integer >= 1). 4 is a good
                  default; 8 is sharper. 1 disables anti-aliasing (fast, but
                  sub-pixel columns will flicker/vanish).
    """
    n_rows, num_cols, channels = image.shape
    if f_min is None:
        f_min = F_MIN_HZ
    if f_max is None:
        f_max = F_MAX_HZ

    x = log_xfrac(col_freq_hz, f_min, f_max)         # (num_cols,)

    # Band edges: column c spans [left[c], right[c]) where left is the
    # log-mapped center of the *previous* column and right is column c's own
    # center. The first column starts at 0 and the last extends to 1, so the
    # bands tile [0, 1].
    left = np.concatenate(([0.0], x[:-1]))           # (num_cols,)

    # For each temp-buffer x position, the containing source column. Bands are
    # contiguous over [0, 1]; searchsorted on the *left* edges finds the index
    # of the first left-edge > xpix, minus 1, i.e. the band containing xpix.
    ssbuf_w = max(out_w * ss, 1)
    xpix = np.arange(ssbuf_w, dtype=np.float64) / ssbuf_w   # [0, 1)
    src_col = np.searchsorted(left, xpix, side='left') - 1
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
    # Scroll speed (px/s) keeps one row passing a point every `row_dur_s`, so
    # the waterfall stays in real-time sync with the audio.
    slope = row_h / row_dur_s if row_dur_s > 0 else 1.0   # px/s (positive = down)

    # The image's last row (recording head) lands exactly on the playhead.
    #
    # `img_top` is the y-coordinate of the top of the image's row 0 (the
    # *newest* data, at the top). The head is the image's *last* (bottom) row;
    # placing its center on the playhead gives the start position below. The
    # image may be far taller than the frame (the normal case) and is simply
    # cropped to the frame; the end position is derived from `t_video_end` so
    # the tail (top row) lands on the playhead at the end. No clamping is done
    # here: even when the image is taller than the frame the head stays exactly
    # on the playhead at t=0 (most of the image sits below the frame bottom,
    # outside the visible crop) and the newest data above the playhead fills the
    # top of the frame.
    img_top_start = playhead - (num_rows - 1) * row_h

    # The video ends when the *tail of the data* passes the playhead. At that
    # instant the tail row sits exactly on the playhead and everything below it
    # is already dark (no data rows remain), so this is the last interesting
    # frame — we do not wait for the empty space below the tail to clear the
    # bottom of the frame. By the sync model (row at the playhead = audio at t)
    # that instant is exactly t = audio duration.
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
    print("color-mapping waterfall ...", file=sys.stderr)
    full_image = render_waterfall_image(rows, reversed_=True)

    # --- Horizontal log-frequency warp (display-only) ---
    # The waterfall's columns are linear in frequency; for a human-friendly
    # display we map each column's center frequency onto screen x on a log
    # axis (F_MIN_HZ..F_MAX_HZ) and mean-anti-alias onto the output-width
    # grid. This widens the low bands (where most musical energy lives) and
    # compresses the high bands. The result is `warped`, shape
    # (num_rows, out_w, 3) uint8 — the same row order as `full_image` (newest
    # at row 0) but with the horizontal axis warped. See `build_warped` and
    # the F_MIN_HZ / F_MAX_HZ / H_SUPERSAMPLE constants above.
    col_freq, nyquist_hz = extract_col_freqs(header, num_cols)
    print(f"log-frequency map: {F_MIN_HZ:.0f} Hz .. {F_MAX_HZ:.0f} Hz "
          f"(nyquist {nyquist_hz:.0f} Hz, ss={H_SUPERSAMPLE})", file=sys.stderr)
    warped = build_warped(full_image, col_freq, out_w,
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

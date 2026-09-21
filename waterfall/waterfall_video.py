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
once the last (oldest) row clears the bottom of the frame.

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
# At t=1.0: hue = (240 + 180) % 360 = 60deg = yellow. At t~0.5: hue~330 (red apex).


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

def render_frame(full_image, img_top, width, height, n_rows, playhead_y):
    """Compose one output frame (height x width x 3, uint8) for a given scroll.

    The whole waterfall is treated as a continuous image whose full height is
    `n_rows` rows. `img_top` is the y-coordinate (in output pixels) of the top
    of the image's row 0; each row spans `height / n_rows` pixels, so the full
    image spans `height` pixels. The visible window is a single vectorized
    nearest-neighbor sample of that image, then stretched horizontally to the
    output width. (With a positive slope, `img_top` grows over time, so the
    image — and the newest rows at its top — scroll *downward*.)

    full_image : (n_rows, n_cols, 3) uint8 pre-colored waterfall.
    img_top    : float — y of the top of image row 0 in output pixels.
    """
    img_h, img_w, _ = full_image.shape

    # Output pixel y -> image row coordinate (continuous).
    row_scale = n_rows / float(height)        # image rows per output pixel
    y_idx = (np.arange(height) - img_top) * row_scale   # (height,)
    row_i = np.clip(np.round(y_idx).astype(np.int64), 0, img_h - 1)
    band = full_image[row_i]                 # (height, img_w, 3)

    # Nearest-neighbor horizontal stretch to `width`.
    col_i = (np.arange(width) * img_w / float(width)).astype(np.int64)
    col_i = np.clip(col_i, 0, img_w - 1)
    frame = band[:, col_i]                   # (height, width, 3)

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
    args = ap.parse_args()

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

    row_h = out_h / float(num_rows)
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
    img_top_start = playhead - (num_rows - 1) * row_h
    # The video ends when the image's oldest row (recording tail, last row of
    # the file) clears the bottom of the frame.
    img_top_end = out_h
    t_video_end = (img_top_end - img_top_start) / slope

    # Audio runs from 0 to t_video_end (the full clip): it starts in sync with
    # the head on the playhead and ends exactly when the last row clears the
    # bottom. Because the sample rate is constant, this is just a duration,
    # and `atrim=start=0:end=<dur>` keys on the *sample count* — which sidesteps
    # the unreliable (NaN) audio timestamp `t` in a multi-input rawvideo pipe.
    a_end_s = max(0.0, t_video_end)
    audio_filter = f"atrim=start=0:end={a_end_s:.6f}"

    total_frames = max(1, int(round(t_video_end * FPS)))

    print(f"waterfall: {num_rows} rows x {num_cols} cols, "
          f"row_dur={row_dur_s:.4f}s, audio={audio_dur:.3f}s", file=sys.stderr)
    print(f"video: {out_w}x{out_h} @ {FPS}fps, {total_frames} frames, "
          f"~{t_video_end:.2f}s", file=sys.stderr)
    print(f"audio: plays from 0.000s to video end "
          f"({t_video_end:.3f}s)", file=sys.stderr)

    # --- Pre-render the waterfall image once (the perf win). ---
    # Reversed so the newest sample is the image's row 0 (top). See the
    # scroll model above for why.
    print("color-mapping waterfall ...", file=sys.stderr)
    full_image = render_waterfall_image(rows, reversed_=True)

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
            frame = render_frame(full_image, img_top, out_w, out_h,
                                 num_rows, playhead)
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

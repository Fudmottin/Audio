#!/usr/bin/env python3
"""
waterfall_video.py — render a waterfall text file into a scrolling MP4 video.

Reads the text output produced by the `waterfall` C++ tool (one `name=value`
header line, then one line per time slice of space-separated 16-bit hex values)
and renders an animated, color waterfall that scrolls downward with an imaginary
playhead at the vertical center of the frame. The audio is muxed in sync: it
starts when the head of the waterfall reaches the playhead and plays through to
the end of the clip. The video ends once the last row has fully scrolled off the
bottom of the frame (which is when the audio ends, because the full waterfall
spans exactly one frame height).

Because the scroll speed is tied to the *real* per-row audio duration
(`hopSize / sampleRate`), the row that is at the playhead at time t is exactly
the audio you hear at time t when the audio start is aligned to the head
crossing the playhead.

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


def render_waterfall_image(rows):
    """Color-map the whole waterfall once.

    rows: (num_rows, num_cols) int array of 16-bit values.
    Returns: (num_rows, num_cols, 3) uint8 RGB array (one pixel per cell).
    """
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

def render_frame(full_image, row_top, width, height, n_rows, playhead_y):
    """Compose one output frame (height x width x 3, uint8) for a given scroll.

    The whole waterfall is treated as a continuous image whose full height is
    `n_rows` rows; at scroll `row_top` the top of row 0 sits at y = row_top
    (each row spans `height / n_rows` pixels, so the full image spans `height`
    pixels). The visible window is a single vectorized nearest-neighbor sample
    of that image, then stretched horizontally to the output width.

    full_image : (n_rows, n_cols, 3) uint8 pre-colored waterfall.
    row_top    : float — y of the top of row 0 in output pixels.
    """
    img_h, img_w, _ = full_image.shape

    # Output pixel y -> image row coordinate (continuous).
    row_scale = n_rows / float(height)        # image rows per output pixel
    y_idx = (np.arange(height) - row_top) * row_scale   # (height,)
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

    # Linear scroll model. The full waterfall spans exactly one frame height,
    # and each row spans row_h pixels. The scroll speed (px/s) is chosen so
    # that one row passes a point on screen every `row_dur_s` seconds, keeping
    # the waterfall's spectral content in real-time sync with the audio.
    slope = -row_h / row_dur_s if row_dur_s > 0 else -1.0   # px/s (negative = down)

    # Keyframes expressed as the required row_top (y of row 0's top).
    rt_clip_start = -0.5 * row_h              # head entering the top edge
    rt_audio_start = playhead - 0.5 * row_h   # row 0's center at the playhead
    rt_video_end = 0.0                        # last row's bottom clears the frame

    # Invert the linear model: t = (rt_clip_start - rt_target) / (-slope).
    t_audio_start = (rt_clip_start - rt_audio_start) / (-slope)
    t_video_end = (rt_clip_start - rt_video_end) / (-slope)

    if t_audio_start < 0.0:
        # The head can only start playing once it has entered the frame.
        t_audio_start = 0.0

    # Safety: ensure the clip is at least as long as the audio needs to play.
    if t_video_end <= t_audio_start:
        t_video_end = t_audio_start + (num_rows * row_dur_s)

    total_frames = max(1, int(round(t_video_end * FPS)))

    print(f"waterfall: {num_rows} rows x {num_cols} cols, "
          f"row_dur={row_dur_s:.4f}s, audio={audio_dur:.3f}s", file=sys.stderr)
    print(f"video: {out_w}x{out_h} @ {FPS}fps, {total_frames} frames, "
          f"~{t_video_end:.2f}s", file=sys.stderr)
    print(f"audio: starts @ {t_audio_start:.3f}s, runs to video end "
          f"({t_video_end:.3f}s)", file=sys.stderr)

    # --- Pre-render the waterfall image once (the perf win). ---
    print("color-mapping waterfall ...", file=sys.stderr)
    full_image = render_waterfall_image(rows)        # (num_rows, num_cols, 3) uint8

    # --- Audio filter: start the audio when the head reaches the playhead. ---
    # The C++ waterfall advances by one hop per row and the scroll is tied to
    # the real per-row audio duration (hopSize/sampleRate), so the audio and
    # the waterfall are already in sync: the row at the playhead at time t is
    # the audio you hear at time t once the audio start is aligned to the head
    # crossing the playhead. We therefore delay the audio by `t_audio_start`;
    # it then runs for the rest of the clip (the tail leaving the playhead is
    # also the video's end, since the full waterfall spans one frame height).
    #
    # `adelay` is used because it does not depend on the audio timestamp `t`,
    # which is unreliable (NaN) in a multi-input pipeline that feeds rawvideo
    # frames on stdin. `-shortest` (added below) ends the video when the audio
    # stream does, which lands exactly on the tail clearing the bottom.
    a_delay_ms = max(0, int(round(t_audio_start * 1000.0)))
    audio_filter = f"adelay={a_delay_ms}|{a_delay_ms}"

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
                   "-c:a", "aac", "-b:a", "192k",
                   "-shortest"]
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
            row_top = rt_clip_start + slope * (fidx / FPS)
            frame = render_frame(full_image, row_top, out_w, out_h,
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

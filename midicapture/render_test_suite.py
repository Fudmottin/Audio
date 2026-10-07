#!/usr/bin/env python3
"""
render_test_suite.py — build and evaluate the midicapture test suite.

The C++ and the Python split the work so each owns exactly one concern:

  C++ (the midicapture binary):
    --generate-test-midi-files   writes the .mid ground-truth files.
    --run-corpus DIR             evaluates: it reads the rendered .mp3 plus
                                 the ground-truth .mid beside it and prints
                                 the per-file recall / precision / Δonset /
                                 Δdur / Δvel / Δoct / Δchroma table + summary.

  Python (this script):
    renders each .mid to audio with timidity (a *real* synthesizer, so the
    sustain-pedal and pitch-bend cases produce a sounding signature) and
    re-encodes to MP3. This is the *voice* — the only place audio is made.
    Keeping the ground truth and its render in one place means they cannot
    drift apart, and no audio-synthesis logic lives in the C++ anymore.

What it does, end to end:
  1. MIDI:   midicapture --generate-test-midi-files --output-dir DIR
  2. VOICE:  for each .mid: timidity -OwM -s 48000 -> .wav, then ffmpeg
             -> .mp3 192 kbps (the .wav is deleted).
  3. EVAL:   midicapture --model basic --run-corpus DIR

Running this script is a *clean* build: the output directory is wiped and
rebuilt from scratch, so any edit to the corpus (or to the voice) is picked
up on the next run. There is no separate --clean flag.

The voice is timidity, which has a *different* instrument personality than
what basic-pitch was trained on, so these metrics are for *relative*
comparison (is a setting better or worse?), not absolute quality. MAESTRO's
real-piano recordings are the absolute-quality data.

Exit codes: 0 = ok; 1 = a required external tool is missing; 2 = a failure
mid-suite (nothing rendered or evaluated).

Usage:
  python3 render_test_suite.py                # build + eval into ./test-midi
  python3 render_test_suite.py --out DIR      # a custom output directory
  python3 render_test_suite.py -v             # also print the render commands
  python3 render_test_suite.py --midicapture PATH
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Locations + render toolchain.
# ---------------------------------------------------------------------------
HERE = Path(__file__).resolve().parent                # midicapture/
DEFAULT_OUTDIR = HERE / "test-midi"
# Tier-2 build: the corpus evaluator and the basic-pitch engine live there.
MIDICAPTURE_BIN = HERE / "build-tier2" / "bin" / "midicapture"

# The render tools: prefer PATH, fall back to the Homebrew install location.
FFMPEG = shutil.which("ffmpeg") or "/opt/homebrew/bin/ffmpeg"
TIMIDITY = shutil.which("timidity") or "/opt/homebrew/bin/timidity"

# Render format: 48 kHz mono matches midicapture's analysis path; 192 k MP3
# is proven transparent to transcription in the round-trip experiment.
SR = 48000
MP3_BITRATE = "192k"


def run(cmd, verbose=False):
    """Run `cmd` (never raises on a non-zero exit); print it if verbose."""
    if verbose:
        print("    $ " + " ".join(str(c) for c in cmd))
    return subprocess.run([str(c) for c in cmd], check=False,
                          capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--out", type=Path, default=DEFAULT_OUTDIR,
                    help=f"Output directory (default: {DEFAULT_OUTDIR})")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="Print the render commands (timidity / ffmpeg).")
    ap.add_argument("--midicapture", type=Path, default=MIDICAPTURE_BIN,
                    help=f"Path to the midicapture binary "
                         f"(default: {MIDICAPTURE_BIN})")
    args = ap.parse_args()

    mid = args.midicapture
    if not mid.exists():
        print(f"Error: midicapture binary not found at {mid}. Build it first.",
              file=sys.stderr)
        return 1
    if not (shutil.which("timidity") or Path(TIMIDITY).exists()):
        print("Error: timidity not found on PATH. Install it "
              "(e.g. `brew install timidity`).", file=sys.stderr)
        return 1
    if not (shutil.which("ffmpeg") or Path(FFMPEG).exists()):
        print("Error: ffmpeg not found on PATH. Install it "
              "(e.g. `brew install ffmpeg`).", file=sys.stderr)
        return 1

    outdir = args.out
    # A clean build: wipe and recreate the output directory.
    if outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    print("midicapture test-suite renderer + evaluator")
    print("=" * 60)
    print(f"  Output dir:    {outdir}")
    print(f"  midicapture:   {mid}")
    print(f"  timidity:      {TIMIDITY}")
    print(f"  ffmpeg:        {FFMPEG}")
    print()

    # 1. Generate the .mid ground truth (the C++ owns the MIDI).
    print("→ Generating .mid ground truth...")
    gen = run([mid, "--generate-test-midi-files", "--output-dir", outdir],
              verbose=args.verbose)
    if gen.returncode != 0:
        print("  ERROR: generator failed:\n" + gen.stderr, file=sys.stderr)
        return 2
    mids = sorted(outdir.glob("*.mid"))
    # Drop any detected-notes output left by a prior direct run.
    mids = [m for m in mids if not m.name.endswith(".mid.detected")]
    if not mids:
        print("  No .mid files generated.", file=sys.stderr)
        return 2
    print(f"  {len(mids)} .mid files.\n")

    # 2. Render each to audio with timidity, then re-encode to MP3 192 k.
    print("→ Rendering each .mid with timidity → MP3 ...")
    rendered = 0
    for m in mids:
        stem = m.name[:-4]
        wav = outdir / (stem + ".wav")
        mp3 = outdir / (stem + ".mp3")
        print(f"  {m.name}  ", end="", flush=True)
        ok = True
        try:
            # timidity: .mid -> 48 kHz mono .wav (a real piano voice).
            t = run([TIMIDITY, "-OwM", "-s", str(SR), "-o", wav, m],
                    verbose=args.verbose)
            if t.returncode != 0:
                ok = False
            else:
                # ffmpeg: .wav -> .mp3 192 k, then drop the wav.
                run([FFMPEG, "-y", "-loglevel", "error", "-i", wav,
                     "-c:a", "libmp3lame", "-b:a", MP3_BITRATE, mp3],
                    verbose=args.verbose)
                wav.unlink()
        except OSError as e:
            ok = False
            print(f"FAILED: {e}", end="")
        if ok and mp3.exists():
            kb = mp3.stat().st_size / 1024.0
            print(f"OK ({kb:.0f} KB MP3)")
            rendered += 1
        else:
            print("FAILED")

    print()
    if rendered == 0:
        print("No MP3 files were rendered.", file=sys.stderr)
        return 2

    # 3. Evaluate with the C++ evaluator (it owns the matching + metrics).
    print(f"→ Evaluating {rendered} file(s) with --run-corpus ...")
    print("-" * 60)
    ev = run([mid, "--model", "basic", "--run-corpus", outdir],
             verbose=args.verbose)
    if ev.stdout:
        sys.stdout.write(ev.stdout)
    if ev.returncode != 0:
        if ev.stderr:
            sys.stderr.write(ev.stderr)
        return ev.returncode if ev.returncode in (1, 2) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

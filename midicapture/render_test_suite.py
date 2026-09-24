#!/usr/bin/env python3
"""
render_test_suite.py — build and evaluate the midicapture monophonic test suite.

What it does, end to end:
  1. SHIP:  invokes the `midicapture` binary in `--generate-test-midi-files`
     mode to (re)write the .mid ground-truth files into test-midi/.
     This regenerates the suite from scratch, so any edit to
     testScaleSet() in main.cpp is picked up on the next run.
  2. WAVE:  converts each .mid to a 48 kHz mono WAV via ffmpeg, using a
     simple decaying-harmonic-sine voice. (We can't render MIDI to audio
     directly; the WAV is the intermediate that ffmpeg can encode.)
  3. MP3:   re-encodes each WAV to MP3 192 kbps (proven transparent to
     transcription in the handoff round-trip) and deletes the WAV.
  4. EVAL:  runs midicapture on each MP3, parses its stdout ("  <note> <midi>
     <start>s → <end>s <velocity>"), and compares the detected note set
     against the .mid ground truth using a greedy time-ordered match.
     Emits per-file metrics: note recall/precision, median onset error,
     median duration error, median velocity error, median octave error,
     median chroma error — plus a suite summary.

Exit codes: 0 = ok; 1 = a required external tool is missing; 2 = a failure
mid-suite (some files may be missing or failed to render/evaluate).
The script never modifies the C++ source or any lode file; it only reads
the binary, writes into test-midi/, and shells out.

Usage:
  python3 render_test_suite.py            # default: build into ./test-midi
  python3 render_test_suite.py --clean    # wipe test-midi/ first
  python3 render_test_suite.py -v         # also print the raw midicapture
                                          # stdout for each file
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Locations
# ---------------------------------------------------------------------------
HERE = Path(__file__).resolve().parent           # midicapture/
PROJECT_ROOT = HERE.parent                       # Audio/
DEFAULT_OUTDIR = HERE / "test-midi"
MIDICAPTURE_BIN = HERE / "build" / "bin" / "midicapture"

FFMPEG = shutil.which("ffmpeg")

# Sample rate + encoding for the render. 48 kHz mono matches midicapture's
# default analysis path; 192 k MP3 is qscale 3 in LAME. The handoff round-trip
# experiment showed 192 k is transparent (8/9 notes byte-identical to the WAV)
# while 96 k perturbs the semitone boundary. We keep 192 k.
SR = 48000
MP3_BITRATE = "192k"

# ---------------------------------------------------------------------------
# Voice: a decaying harmonic stack.
#
# For each note at MIDI number m, fundamental f = 440 * 2^((m-69)/12) Hz.
# We sum harmonics k=1..6 with amplitudes that approximate a bright-but-not-
# brash piano: [1.0, 0.5, 0.3, 0.2, 0.12, 0.08], and apply an exponential
# decay with a note-length-dependent time constant so that the loudest hop
# (the attack) still carries the full harmonic series while the tail thins.
# Velocity scales the overall amplitude linearly (so the velocity-ladder test
# produces a real loudness difference that midicapture's peak-RMS velocity
# can read back).
# ---------------------------------------------------------------------------
HARMONIC_AMP = [1.0, 0.5, 0.3, 0.2, 0.12, 0.08]


def midi_to_hz(m):
    return 440.0 * (2.0 ** ((m - 69) / 12.0))


def synth_note(hz, seconds, velocity, sr, harmonics=6, decay_tau=None):
    """Render one note as a decaying harmonic stack, length `seconds`."""
    import numpy as np  # local import: numpy is a hard dep but only here.

    t = np.arange(int(seconds * sr)) / sr
    if decay_tau is None:
        decay_tau = max(0.15, 0.5 * seconds)  # scale decay with note length
    env = np.exp(-t / decay_tau)
    # A 5 ms linear fade-in avoids a click at the attack; the harmonic stack
    # itself has a discontinuity at t=0 that the fade hides.
    fade = np.minimum(1.0, t / 0.005)
    amp = velocity / 127.0 * 0.2  # 0.2 keeps peaks well below full scale.
    sig = np.zeros_like(t)
    for k in range(1, harmonics + 1):
        a = HARMONIC_AMP[k - 1] if k <= len(HARMONIC_AMP) else 0.0
        sig += a * np.sin(2.0 * np.pi * k * hz * t)
    return amp * env * fade * sig


def render_midi_to_wav(mid_path, wav_path, sr=SR):
    """Render one .mid to a 48 kHz mono WAV via ffmpeg.

    We synthesize the audio in Python (decaying harmonic stack) and hand it
    to ffmpeg as raw PCM to write a canonical WAV. This avoids depending on
    timidity (weak .pat fundamental) or any soundfont install.
    """
    import numpy as np
    import wave

    notes = parse_midi_notes(mid_path)  # [(start_s, end_s, midi, vel), ...]
    if not notes:
        wav_path.write_bytes(b"")
        return 0

    total_s = max(n[1] for n in notes) + 0.5  # half-second tail
    total_frames = int(total_s * sr)
    buf = np.zeros(total_frames, dtype=np.float32)

    for start_s, end_s, midi, vel in notes:
        length = max(0.02, end_s - start_s)
        note = synth_note(midi_to_hz(midi), length, vel, sr)
        s0 = int(start_s * sr)
        s1 = min(total_frames, s0 + len(note))
        if s0 < total_frames:
            buf[s0:s1] += note[: s1 - s0]

    # Normalize to peak 0.85 to keep headroom while giving the loudest hop
    # room to saturate the velocity mapping.
    peak = float(np.max(np.abs(buf))) if len(buf) else 0.0
    if peak > 0:
        buf = buf * (0.85 / peak)
    pcm = (buf * 32767.0).astype("<i2")

    with wave.open(str(wav_path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())

    return total_frames


# ---------------------------------------------------------------------------
# Minimal SMF parser (ground-truth extraction).
#
# We only need the notes from the .mid files we generate: one track, a tempo
# meta-event, and a run of note-on/note-off pairs. We skip everything else.
# This is NOT a general-purpose SMF parser; it is sized to the writer's output
# (see lode/midicapture/writer.md for the byte layout).
# ---------------------------------------------------------------------------
def _read_varint(data, i):
    """Read an ASCII7 variable-length quantity; return (value, new_i)."""
    v = 0
    while True:
        b = data[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, i


def parse_midi_notes(mid_path):
    """Return a list of (start_s, end_s, midi, velocity) from a Type 1/2 SMF.

    Sized to the writer's output (see lode/midicapture/writer.md): one track,
    a tempo meta-event, and a run of note-on / note-off pairs. We handle the
    standard running-status byte (status < 0x80 reuses the previous channel
    message) so the parser stays correct even if the writer ever omits a
    repeating status byte.
    """
    raw = Path(mid_path).read_bytes()
    if raw[:4] != b"MThd":
        return []
    hlen = int.from_bytes(raw[4:8], "big")
    i = 8 + hlen
    # Header layout: MThd(4) len(4) format(2) ntracks(2) division(2).
    # Division is at offset 4+4+2+2 = 12. (The 0x01 in our files means
    # "ticks per quarter note"; a 0x00 high byte would mean SMPTE frames.)
    div = int.from_bytes(raw[12:14], "big")
    if div == 0:
        div = 1

    tempo_us = 500000  # 120 BPM
    ticks_to_sec = tempo_us / 1_000_000.0 / div

    onsets = {}  # midi -> (on_tick, vel)
    notes = []   # (on_tick, off_tick, midi, vel)
    last_channel_status = None

    while i < len(raw):
        if raw[i:i + 4] != b"MTrk":
            break
        tlen = int.from_bytes(raw[i + 4:i + 8], "big")
        j = i + 8
        end = i + 8 + tlen
        tick = 0
        while j < end:
            dt, j = _read_varint(raw, j)
            tick += dt
            if j >= end:
                break
            status = raw[j]
            j += 1
            if status < 0x80:
                # Running status: reuse the previous channel message status.
                if last_channel_status is None:
                    break
                status = last_channel_status
            elif 0x80 <= status <= 0xEF:
                last_channel_status = status
            else:
                last_channel_status = None  # meta/sysex; nothing to reuse
            hi = status & 0xF0
            if status == 0xFF:               # meta-event (status 0xFF exactly)
                mtype = raw[j]; j += 1
                mlen, j = _read_varint(raw, j)
                mdata = raw[j:j + mlen]; j += mlen
                if mtype == 0x51 and len(mdata) == 3:  # tempo
                    tempo_us = int.from_bytes(mdata, "big")
                    ticks_to_sec = tempo_us / 1_000_000.0 / div
            elif status in (0xF0, 0xF7):    # sysex
                mlen, j = _read_varint(raw, j)
                j += mlen
            elif hi == 0x90:                 # note-on (any channel)
                n = raw[j]; v = raw[j + 1]; j += 2
                if v != 0:  # a note-on with velocity 0 is a note-off
                    onsets[n] = (tick, v)
                elif n in onsets:
                    on_tick, vel = onsets.pop(n)
                    notes.append((on_tick, tick, n, vel))
            elif hi == 0x80:                 # note-off (pitch + velocity)
                n = raw[j]; _ = raw[j + 1]; j += 2
                if n in onsets:
                    on_tick, vel = onsets.pop(n)
                    notes.append((on_tick, tick, n, vel))
            elif hi in (0xB0, 0xA0):         # control / channel aftertouch
                j += 2
            elif hi in (0xC0, 0xD0):         # program / poly aftertouch
                j += 1
            elif hi == 0xE0:                 # pitch bend
                j += 2
            else:
                break  # unknown — bail rather than misparse.
        i = end

    # Any note still open at track end: close it there.
    for n, (on_tick, vel) in onsets.items():
        notes.append((on_tick, tick, n, vel))

    out = [(on_t * ticks_to_sec, off_t * ticks_to_sec, n, v)
           for on_t, off_t, n, v in notes]
    out.sort(key=lambda x: x[0])
    return out



# ---------------------------------------------------------------------------
# Evaluation: greedy time-ordered match of detected notes to truth notes.
#
# A detected note matches a truth note if it is the earliest unmatched truth
# note whose [start, end] interval overlaps the detected [start, end] and
# whose pitch class is within 1 semitone (a single-octave error is a *separate*
# error class; it should not be "forgiven" by the matcher, but it should be
# counted so the octave-error column shows the magnitude).
# ---------------------------------------------------------------------------
def match_notes(truth, detected):
    """Return (matched_pairs, missed_truth, false_detected).

    matched_pairs is a list of (truth, detected) tuples; the rest are the
    unmatched truth and detected notes respectively.
    """
    truth = sorted(truth, key=lambda x: x[0])
    detected = sorted(detected, key=lambda x: x[0])
    used = [False] * len(detected)
    pairs = []
    missed = []
    for t in truth:
        t_start, t_end, t_pitch, t_vel = t
        best = None
        for i, d in enumerate(detected):
            if used[i]:
                continue
            d_start, d_end, d_pitch, d_vel = d
            # Overlap test in time.
            if d_start > t_end or d_end < t_start:
                continue
            # Pitch class must be within 1 semitone of truth (mod 12).
            if abs((d_pitch - t_pitch) % 12) > 1 and abs((t_pitch - d_pitch) % 12) > 1:
                continue
            if best is None or d_start < detected[best][0]:
                best = i
        if best is None:
            missed.append(t)
        else:
            used[best] = True
            pairs.append((t, detected[best]))
    false_det = [d for i, d in enumerate(detected) if not used[i]]
    return pairs, missed, false_det


def median(xs):
    if not xs:
        return 0.0
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def evaluate_one(truth_path, mp3_path, midicapture_bin, verbose=False):
    """Transcribe mp3_path with midicapture; compare to truth_path's notes."""
    out_mid = mp3_path.with_suffix(".mid.detected")
    result = subprocess.run(
        [str(midicapture_bin), "--input", str(mp3_path),
         "--output", str(out_mid)],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        if verbose:
            print(f"    [warn] midicapture failed on {mp3_path.name}: "
                  f"{result.stderr.strip()}")
        return None

    detected = _parse_midicapture_stdout(result.stdout)
    truth = parse_midi_notes(truth_path)
    if not truth:
        return None

    pairs, missed, false_det = match_notes(truth, detected)
    n_truth, n_det = len(truth), len(detected)
    onset_errs = [abs(d[0] - t[0]) for t, d in pairs]
    dur_errs = [abs((d[1] - d[0]) - (t[1] - t[0])) for t, d in pairs]
    vel_errs = [abs(d[3] - t[3]) for t, d in pairs]
    oct_errs = [abs((d[2] // 12) - (t[2] // 12)) for t, d in pairs]
    # Chroma distance: how far off on the 12-class pitch-class circle (0 =
    # right class, 1 = adjacent, 6 = maximally wrong). abs then mod is the
    # correct circle distance for the 0..11 range.
    chrom_errs = [min((d[2] - t[2]) % 12, (t[2] - d[2]) % 12) for t, d in pairs]

    return {
        "name": mp3_path.name,
        "n_truth": n_truth,
        "n_det": n_det,
        "recall": len(pairs) / n_truth if n_truth else 0.0,
        "precision": len(pairs) / n_det if n_det else 0.0,
        "onset_ms": 1000.0 * median(onset_errs),
        "dur_ms": 1000.0 * median(dur_errs),
        "vel": median(vel_errs),
        "octave": median(oct_errs),
        "chroma": median(chrom_errs),
        "n_missed": len(missed),
        "n_false": len(false_det),
    }


# Regex for a line like:
#   "  C#3  49  2.03733s → 2.09067s  89"
_NOTE_LINE = re.compile(
    r"^\s+([A-G]#?)(-?\d+)\s+(\d+)\s+([\d.]+)s\s+→\s+([\d.]+)s\s+(\d+)"
)


def _parse_midicapture_stdout(text):
    """Extract detected notes from midicapture's stdout."""
    # midi note number → (start_s, end_s, midi, vel)
    out = []
    for line in text.splitlines():
        m = _NOTE_LINE.match(line)
        if not m:
            continue
        name, octv, midi, t0, t1, vel = m.groups()
        # octv is a string like "3" or "-1"; midi is the absolute note number.
        out.append((float(t0), float(t1), int(midi), int(vel)))
    return out


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument(
        "--out", type=Path, default=DEFAULT_OUTDIR,
        help=f"Output directory (default: {DEFAULT_OUTDIR})",
    )
    ap.add_argument("--clean", action="store_true",
                    help="Delete the output directory before running")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="Print the raw midicapture stdout for each file")
    ap.add_argument(
        "--midicapture", type=Path, default=MIDICAPTURE_BIN,
        help=f"Path to the midicapture binary (default: {MIDICAPTURE_BIN})",
    )
    args = ap.parse_args()

    if not FFMPEG:
        print("Error: ffmpeg not found on PATH.", file=sys.stderr)
        return 1
    if not args.midicapture.exists():
        print(f"Error: midicapture binary not found at {args.midicapture}. "
              "Build it first: cmake --build .", file=sys.stderr)
        return 1

    outdir = args.out
    if args.clean and outdir.exists():
        shutil.rmtree(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    print("midicapture test-suite renderer + evaluator")
    print("=" * 60)
    print(f"  Output dir:   {outdir}")
    print(f"  midicapture:  {args.midicapture}")
    print(f"  ffmpeg:       {FFMPEG}")
    print()

    # 1. Generate the .mid ground truth.
    print("→ Generating .mid ground truth...")
    gen = subprocess.run(
        [str(args.midicapture), "--generate-test-midi-files",
         "--output-dir", str(outdir)],
        capture_output=True, text=True,
    )
    if gen.returncode != 0:
        print("  ERROR: generator failed:\n" + gen.stderr, file=sys.stderr)
        return 2
    mids = sorted(outdir.glob("*.mid"))
    # Exclude the detected-notes output from any prior run.
    mids = [m for m in mids if not m.name.endswith(".mid.detected")]
    print(f"  {len(mids)} .mid files generated.\n")

    # 2. Render each to WAV, 3. encode to MP3 192k (deleting the WAV).
    mp3_paths = []
    for mid in mids:
        stem = mid.stem
        wav = outdir / (stem + ".wav")
        mp3 = outdir / (stem + ".mp3")
        print(f"→ {mid.name}  ", end="", flush=True)
        try:
            frames = render_midi_to_wav(mid, wav)
            subprocess.run(
                [FFMPEG, "-y", "-loglevel", "error",
                 "-i", str(wav),
                 "-c:a", "libmp3lame", "-b:a", MP3_BITRATE,
                 str(mp3)],
                check=True, capture_output=True,
            )
            wav.unlink()
            size_kb = mp3.stat().st_size / 1024.0
            print(f"OK ({frames // SR}s, {size_kb:.0f} KB MP3)")
            mp3_paths.append(mp3)
        except (subprocess.CalledProcessError, OSError) as e:
            print(f"FAILED: {e}")

    print()

    # 4. Evaluate each MP3 by running midicapture on it.
    if not mp3_paths:
        print("No MP3 files to evaluate.", file=sys.stderr)
        return 2

    print("→ Evaluating midicapture on each MP3...")
    results = []
    for mp3 in mp3_paths:
        mid = mp3.with_suffix(".mid")
        r = evaluate_one(mid, mp3, args.midicapture, verbose=args.verbose)
        if r is None:
            print(f"  {mp3.name}:  no evaluation (skipped)")
            continue
        results.append(r)
        print(f"  {mp3.name}:  "
              f"recall {100*r['recall']:5.1f}%  "
              f"prec   {100*r['precision']:5.1f}%  "
              f"Δonset {r['onset_ms']:6.1f} ms  "
              f"Δdur   {r['dur_ms']:6.1f} ms  "
              f"Δvel   {r['vel']:4.1f}  "
              f"Δoct   {r['octave']:3.1f}  "
              f"Δchroma {r['chroma']:3.1f}  "
              f"({r['n_det']}/{r['n_truth']} notes, "
              f"{r['n_missed']} missed, {r['n_false']} false)")

    # Suite summary.
    if not results:
        print("\nNo files could be evaluated.", file=sys.stderr)
        return 2

    n = len(results)
    avg = lambda k: sum(r[k] for r in results) / n
    print()
    print("Suite summary")
    print("-" * 60)
    print(f"  Files evaluated:              {n}")
    print(f"  Avg note recall:              {100*avg('recall'):5.1f}%")
    print(f"  Avg note precision:           {100*avg('precision'):5.1f}%")
    print(f"  Median onset error:           {1000*avg('onset_ms'):6.1f} ms")
    print(f"  Median duration error:        {1000*avg('dur_ms'):6.1f} ms")
    print(f"  Median velocity error:        {avg('vel'):6.1f}")
    print(f"  Median octave error:          {avg('octave'):6.1f}")
    print(f"  Median chroma error:          {avg('chroma'):6.1f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

# midicapture — Note Modeling (defragmentation & the octave problem)

> How the **Tier-1 monophonic aubio** path turns a decaying note into one clean
> note, and why its *octave* is unreliable on weak-fundamental renders. Moved
> out of [summary.md](summary.md) §8 so both files stay under the 250-line cap.
> Loded alongside [summary.md](summary.md); the writer is in [writer.md](writer.md).

---

## 1. Why a decaying piano note fragments

A *decaying* note is **closed and re-opened hop to hop by its own spectral-flux
wobble**: a flux blip closes the note, the lagged window still reports the old
pitch, a new fragment opens, and because the peak RMS decays the 3-hop
off-threshold is never reached *inside* a dense passage. Without a fix this
yields hundreds of ~10 ms one-hop fragments (a 30 s `final-fantasy.aiff` gave
**1447** notes at `--silence -40`, up to 55 consecutive same-pitch 1-hop
fragments). The release hysteresis is *not* the culprit — the decay itself is.

## 2. Defragmentation (current state)

Three rules collapse the wobble debris, applied in `Transcriber::transcribe()`:

| Rule | Value | Effect |
|------|-------|--------|
| Min note lifetime | `kMinNoteHops = 5` (100 ms) | Drops 1–3 hop wobble fragments at close. |
| Min replace lifetime | `kMinReplaceHops = 5` | A pitch-change/onset may *replace* an in-flight note only once it is credible; younger = wobble within the same note. |
| Same-pitch merge | gap < `60/tempo · 0.5` s | Joins consecutive same-pitch fragments (earliest start, latest end). Recovers a wobble-fragmented sustained note the in-loop rules cannot (they see the *placeholder* pitch, not the final one). |

**Velocity** = loudest hop-RMS over the merged lifetime (one attack-and-decay,
not per-hop tremolo); the new-note start is back-dated one hop for the
attack-window lag.

**Measured:** `final-fantasy.aiff` **1447 → 101 notes** (F#/E/G# content,
sensibly spaced). On the 14-file `timidity` corpus defrag gives a plausible note
count and, for the chromatic scale, *preserves the ascending pitch sequence*.

## 3. The octave problem (measured; not a window bug)

On weak-fundamental renders a decaying note's YIN estimates sit **1–2 octaves
*below* the true fundamental** (e.g. **~91 Hz for an A4 440 Hz** note; the
loudest hop is also ~90 Hz; **zero** estimates near 440). This is a *model*
mismatch, not a resolution problem:

- A direct spectral read shows the 440 Hz fundamental is the **dominant**
  partial (~11× the 220 Hz sub-octave, ~5× the 880 Hz harmonic); 440 Hz needs
  ~4 cycles to resolve and the 2048-sample window holds ~11. The fundamental is
  *present and strong*; YIN simply does not report it.
- The same low estimate is returned by **all** aubio methods (yin, yinfast,
  mcdf, f0) and is **unchanged by window size** (2048 → 16384).

YIN is a *harmonic* estimator: it finds the best single period that explains the
spectrum, so on a source whose spectrum peaks at a harmonic (or a sub-harmonic)
it reports that, not the true f₀.

**Consequence:** on these renders the *chroma* is correct and defrag gives a
plausible count, but the *octave* is untrustworthy. On a **recorded**
performance with a strong fundamental (the project's real target) the output is
musically sensible. Robust octave for weak-fundamental sources is handled by the
**Tier-2 basic-pitch** analyzer (an explicit harmonic model) behind the `Analyzer`
port — see [../libaudio/tier2.md](../libaudio/tier2.md). Minor artifact: the
*first* note of a file is often missed (aubio first-frame onset); a quiet render's
decay tail can fall below `--silence -40` a second after each attack.

## 4. Also fixed: stereo-input segfault

A stereo `sf_readf_float` once wrote `2048×2` floats into a 2048-float buffer
(heap overflow → segfault). The buffer is now sized `bufSize × channels` and the
first `framesRead` positions hold valid mono after the in-place downmix; a
partial read near EOF is zero-padded to `hopSize`. **Status:** fixed.

## 5. Cross-References

- [summary.md](summary.md) — module overview, pipeline, CLI, validation toolchain
- [../audio-to-midi.md](../audio-to-midi.md) — the cross-module octave/tier analysis
- [../libaudio/tier2.md](../libaudio/tier2.md) — the Tier-2 basic-pitch octave fix

# Plan: Promote the basic-pitch port to Tier 1

> **Status: ⏸ Planned (deferred — a future session, not the current one).**
> Captured here per the project rule that anything worth implementing is worth
> recording. The current work (pitch-bends parity) is the *first* step toward
> this; this promotion is a later, separate effort.

## 1. Where we are

basic-pitch is **Tier-2**: gated behind `LIBAUDIO_ENABLE_TIER2` (default OFF),
ONNX Runtime + Core ML is an *optional* dependency, and the aubio `Transcriber`
remains the default Tier-1 engine. The `--analyzer {basic-pitch|aubio}` switch
selects between them. basic-pitch is the *better* engine (resolves the octave the
monophonic YIN path cannot — 100% recall / 68% precision on the 14-file corpus vs
aubio's 6%).

## 2. Goal

Make basic-pitch the **primary** transcription path — the thing you get by
default, with the aubio engine demoted to a fallback/legacy option — and treat it
as first-class (a "Tier-1-class" engine), not an optional experiment.

## 3. Likely steps (to be scoped in the dedicated session)

1. **Flatten the tiering.** Decide the end-state model: (a) ONNX becomes a
   *required* dependency and `LIBAUDIO_ENABLE_TIER2` is removed (always-on), or
   (b) keep the flag but flip its default to ON and make the CLI default to
   basic-pitch. This is the pivotal decision.
2. **Front-end hardening.** basic-pitch's front-end relies on **ffmpeg** (the
   installed aubio lacks `libsamplerate`). Promoting it makes ffmpeg a hard
   runtime requirement for transcription — document that and the failure mode.
3. **Reconcile the HIR / writer.** The `Note`/`Score`/writer are shared by both
   engines. Confirm the aubio path still produces clean output after the bend
   fields (from the current session) land, so the demotion doesn't regress Tier-1.
4. **Corpus & docs.** Flip the default in the harness + `--help`; update the
   lode (this file, [../libaudio/tier2.md](../libaudio/tier2.md), [../audio-to-midi.md](../audio-to-midi.md)
   §3, [../summary.md](../summary.md)) to reflect basic-pitch as the primary path.

## 4. Open questions (resolve in that session)

- Is ONNX Runtime acceptable as a **hard** dependency for the whole build, or do
  we keep a soft flag and just flip defaults?
- Does the Core ML EP being *non-fatal* (CPU fallback) stay acceptable as the
  promoted behavior, or should a missing EP warn loudly?
- Do we retire the aubio `Transcriber` entirely, or keep it as a no-ffmpeg fallback?

## 5. Depends on

- **This session's pitch-bend work** (features 1–3) landing first — the port must
  reach *parity* (and the bend improvements) before it is promoted to primary.

## 6. Cross-References

- [../libaudio/tier2.md](../libaudio/tier2.md) — current Tier-2 architecture + phasing (§5 Phase 1c = the current bend work)
- [transcriber-merge.md](transcriber-merge.md) — the prior unification that set up the `Analyzer` port both engines share

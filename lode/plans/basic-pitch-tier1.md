# Plan: Fully promote basic-pitch

> **Status: ⚠️ Partially done.** The **midicapture-side** promotion is complete:
> `basic` (basic-pitch) is the **default `--model`** in a Tier-2 build, `--analyzer`
> is a deprecated alias, and it works for direct transcription (not just
> `--run-corpus`). The **libaudio-side** decision — whether to *flatten the tiering*
> (make ONNX a required dep / always-on) — is **still open** and the user has chosen
> to **keep both Tier-1 and Tier-2 builds as-is** for now. This file tracks that
> remaining decision. See [../libaudio/tier2.md](../libaudio/tier2.md) and
> [../midicapture/summary.md](../midicapture/summary.md) for the current state.

## 1. Where we are (current state)

basic-pitch is **Tier-2**: gated behind `LIBAUDIO_ENABLE_TIER2` (default OFF),
ONNX Runtime + Core ML is an *optional* dependency, and the aubio `Transcriber`
remains available as the `aubio` model. **But** in a Tier-2 build the midicapture
CLI now **defaults to `basic`** (the better engine — resolves the octave the
monophonic YIN path cannot: 100% recall / 68% precision on the 14-file corpus vs
aubio's 6%). `--model {basic|basic-pitch|aubio}` selects the engine for both the
direct path and `--run-corpus`; `--analyzer` is a deprecated alias. The two build
flavors (Tier-1 aubio-only, Tier-2 with basic-pitch) are unchanged.

## 2. The remaining open question (not yet decided)

Whether to take the promotion the last step, at the **library** level:

1. **Flatten the tiering.** Either (a) make ONNX Runtime a *required* dependency and
   remove `LIBAUDIO_ENABLE_TIER2` (always-on), or (b) keep the flag but flip its
   default to ON. Today we deliberately keep the flag defaulting OFF so the Tier-1
   aubio path stays byte-for-byte unaffected and dependency-light. This is the
   pivotal, not-yet-taken decision.
2. ~~**ffmpeg as a hard requirement.**~~ **Resolved by [ffmpeg-link.md](ffmpeg-link.md):**
   the front-end now decodes **in-process** via the linked FFmpeg *libraries*
   (Tier-2, gated) — no ffmpeg binary, no shell-out, no temp file. The aubio
   engine's container fallback still uses the ffmpeg binary (Tier-1 behavior,
   unchanged). The superseded miniaua variant: [ffmpeg-in-memory.md](ffmpeg-in-memory.md).

## 3. Done (the midicapture-side promotion)

1. ✅ **`--model` for direct transcription** (not just `--run-corpus`): `basic`
   (default) / `basic-pitch` (synonym) → `BasicPitch`; `aubio` → `Transcriber`.
2. ✅ **`--analyzer` as a deprecated alias** (warns, still works; bound to its own
   var to avoid a Boost default-clobber).
3. ✅ **`basic` is the default** engine in a Tier-2 build.
4. ✅ **Per-engine knob gating** (aubio-only / basic-only knobs; inert on the other
   engine, with a warning) and a uniform `--tempo` across both engines.
5. ✅ **Docs** (midicapture/README, root README, lode) updated to the two-engine,
   default-basic reality.

## 4. Open questions (resolve before flattening)

- Is ONNX Runtime acceptable as a **hard** dependency for the whole build, or do
  we keep a soft flag and just flip defaults?
- Does the Core ML EP being *non-fatal* (CPU fallback) stay acceptable as the
  promoted behavior, or should a missing EP warn loudly?
- Do we retire the aubio `Transcriber` entirely, or keep it as the no-ONNX
  fallback?

## 5. Depends on

- **Pitch-bend parity** (already landed — features 1–3 of the port) — the port
  reached parity before it was made the default.
- The **in-process FFmpeg front-end** ([ffmpeg-link.md](ffmpeg-link.md) — implemented)
  removed the ffmpeg *binary* dependency for basic-pitch.

## 6. Cross-References

- [../libaudio/tier2.md](../libaudio/tier2.md) — Tier-2 architecture + phasing (basic-pitch is the decided path)
- [../midicapture/summary.md](../midicapture/summary.md) — the current two-engine CLI
- [ffmpeg-link.md](ffmpeg-link.md) — the in-process FFmpeg front-end (implemented; supersedes ffmpeg-in-memory.md)
- [transcriber-merge.md](transcriber-merge.md) — the prior unification that set up the `Analyzer` port both engines share

# Terminology

## Audio Concepts

| Term | Definition |
|------|------------|
| **AIFF** | Audio Interchange File Format. Uncompressed PCM audio container used by Apple. Chunk-based format: FORM → COMM (metadata) → SSND (data). |
| **BlackHole** | Open-source virtual audio device for macOS. Routes audio from any application to another. Used here as the capture target. |
| **PCM** | Pulse Code Modulation. Raw, uncompressed audio samples. |
| **32-bit float PCM** | Each sample is a 32-bit IEEE 754 single-precision floating-point number. Range: [-1.0, 1.0]. |
| **Sample rate** | Number of audio samples per second (e.g., 48000 Hz = 48 kHz). |
| **Channel count** | Number of audio channels (1 = mono, 2 = stereo). |
| **Frame** | One set of samples across all channels. For stereo 32-bit float: 8 bytes per frame (2 channels × 4 bytes). |
| **Interleaved** | Stereo samples stored as L, R, L, R... (alternating). |

## Core Audio API

| Term | Definition |
|------|------------|
| **AudioObjectID** | Unique identifier for a Core Audio object (device, hardware, etc.). |
| **IO proc** | Input/Output callback procedure. Called by Core Audio when audio data is available. Replaces deprecated synchronous `AudioDeviceRead`. |
| **AudioDeviceCreateIOProcID** | Creates an IO proc with a unique ID. Used instead of deprecated `AudioDeviceCreate`. |
| **kAudioHardwarePropertyDevices** | Returns list of all available audio devices. Replaces deprecated `kAudioObjectPropertyList`. |
| **kAudioObjectPropertyElementMain** | Specifies the main element. Replaces deprecated `kAudioObjectPropertyElementMaster`. |

## AIFF Format

| Chunk | Description |
|-------|-------------|
| **FORM** | Top-level container. Contains "FORM" magic bytes, size, and "AIFF" identifier. |
| **COMM** | Common metadata: channels, sample count, sample size, sample rate (80-bit extended float). |
| **SSND** | Sound data: contains the raw PCM samples with offset and block size fields. |

### 80-bit Extended Float (IEEE 754)

Used in the COMM chunk for sample rate:

| Field | Size | Description |
|-------|------|-------------|
| Sign | 1 bit | 0 = positive, 1 = negative |
| Exponent | 15 bits | Biased by 16383 |
| Significand | 64 bits | Explicit leading 1 (unlike IEEE 754 double which hides it) |

**Layout (big-endian, 10 bytes):**
- Byte 0: sign (bit 7) + 7 high bits of exponent (bits 0-6)
- Byte 1: 8 low bits of exponent (bits 7-14)
- Bytes 2-9: 64-bit significand (high byte first)

**Value formula:** `(-1)^sign × 2^(exponent - 16383) × (1 + significand / 2^64)`

## Lode Coding

| Term | Definition |
|------|------------|
| **Lode** | A structured folder of markdown files in a repository that preserves project knowledge for AI-assisted development. |
| **summary.md** | Top-level project overview. |
| **terminology.md** | Shared glossary of terms used across the project. |
| **practices.md** | Repository-wide coding practices, constraints, and conventions. |
| **lode-map.md** | Index/overview of all lode files, organized by subsystem. |
| **Subsystem summary** | Focused documentation for a specific module or component. |
| **Cognitive ownership** | The principle that the human developer retains understanding and accountability for all code, even when AI assists. |

## Project-Specific

| Term | Definition |
|------|------------|
| **aiffcapture** | Phase 1 utility: captures audio from BlackHole 2ch and writes to AIFF files. |
| **DRM-free AIFF** | Unencrypted AIFF file produced by stripping DRM from Apple Music content. |
| **Audio → MIDI** | The transcription pipeline: audio file → DSP analysis → AI inference → MIDI file. |
| **MIDI file** | Musical Instrument Digital Interface file. Standard format for representing musical performance data. |
| **DAW** | Digital Audio Workstation (e.g., Logic Pro, GarageBand). |
| **DRM** | Digital Rights Management. Apple Music's encryption layer. |

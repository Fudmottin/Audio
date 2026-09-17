// audio_types.h — Shared types for aiffcapture
// Core Guidelines: this file defines the data interfaces between modules.
// It contains no implementation, only declarations and data structures.
// This separation makes the code easier to reason about and test.

#ifndef AIFFCAPTURE_AUDIO_TYPES_H
#define AIFFCAPTURE_AUDIO_TYPES_H

#include <cstdint>
#include <string>

// ============================================================================
// AudioFormat — Describes the raw audio stream format from Core Audio.
// Core Guidelines: use explicit types, not implicit conversions.
// This struct mirrors AudioStreamBasicDescription but only captures the
// fields we actually need. We avoid the full struct to reduce coupling
// to Core Audio internals.
// ============================================================================
struct AudioFormat {
   // Sample rate in Hz (e.g., 44100, 48000).
   // Core Guidelines: explicit units in the name.
   uint32_t sampleRate = 0;

   // Number of interleaved channels (1 = mono, 2 = stereo).
   uint32_t channels = 0;

   // Bits per sample (16 = 16-bit signed integer, CDDA standard).
   uint32_t bitsPerSample = 0;

   // Bytes per frame (channels * bitsPerSample / 8).
   uint32_t bytesPerFrame = 0;

   // Bytes per packet (for compressed formats; always equals bytesPerFrame
   // for uncompressed formats like AIFF).
   uint32_t bytesPerPacket = 0;

   // Frames per packet (for compressed formats; always 1 for uncompressed).
   uint32_t framesPerPacket = 0;

   // True if the format is interleaved (stereo: LRLRLR...).
   // Core Guidelines: explicit boolean semantics.
   bool interleaved = false;

   // Convert to a human-readable string for logging/debugging.
   [[nodiscard]] std::string toString() const;
};

// ============================================================================
// CaptureConfig — Configuration for a capture session.
// Core Guidelines: aggregate type with no hidden state or side effects.
// This is the single source of truth for how the capture behaves.
// ============================================================================
struct CaptureConfig {
   // Target output file path (e.g., "output.aiff").
   std::string outputPath;

   // Duration of capture in seconds (0 = indefinite, stop on Ctrl-C).
   // Core Guidelines: explicit 0 means "no limit" rather than "error."
   double durationSeconds = 0.0;

   // Name of the BlackHole device to capture from.
   // Core Guidelines: default to the 2-channel variant.
   std::string deviceName = "BlackHole 2ch";

   // True if the program should print verbose progress to stderr.
   // Core Guidelines: verbose mode is a boolean flag, not a global.
   bool verbose = false;
};

#endif // AIFFCAPTURE_AUDIO_TYPES_H

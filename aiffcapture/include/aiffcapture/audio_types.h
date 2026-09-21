/**
 * @file audio_types.h
 * @brief Shared data types for aiffcapture — describes audio formats and
 *        capture configuration.
 *
 * This header defines the two fundamental data types used throughout
 * aiffcapture:
 *
 * - `AudioFormat` — describes the raw audio stream format from Core Audio.
 *   It mirrors `AudioStreamBasicDescription` but only captures the fields
 *   we actually need. We avoid the full Core Audio struct to reduce coupling
 *   to Core Audio internals, making future portability (e.g., to ALSA on
 *   Linux) easier.
 *
 * - `CaptureConfig` — configuration for a capture session. This is the
 *   single source of truth for how the capture behaves: output path,
 *   duration, device name, and verbose flag.
 *
 */

#ifndef AIFFCAPTURE_AUDIO_TYPES_H
#define AIFFCAPTURE_AUDIO_TYPES_H

#include <cstdint>
#include <string>

// ============================================================================
// AudioFormat — Describes the raw audio stream format from Core Audio.
//
// Domain context: Core Audio reports audio format as an
// AudioStreamBasicDescription, which is a large struct with fields for
// compressed formats (packet counts, frame counts, etc.). For our use
// case — capturing uncompressed PCM for AIFF — we only need the fields
// that describe raw sample layout.
//
// Key format decisions:
// - We capture as 16-bit signed integer PCM (CDDA standard), not 32-bit
//   float. Core Audio always outputs 32-bit float, so main.cpp converts.
// - We use 32-bit integer sample rate encoding (not 80-bit extended float)
//   because macOS tools (afinfo, ffprobe) always try to parse 80-bit
//   extended float regardless of COMM chunk size, producing garbage values.
// - We store channels, sample rate, bits per sample, bytes per frame, and
//   interleaving flag. These are the fields needed to write a valid AIFF
//   COMM chunk.
//
// Use explicit types, not implicit conversions.
// This struct mirrors AudioStreamBasicDescription but only captures the
// fields we actually need. We avoid the full struct to reduce coupling
// to Core Audio internals.
// ============================================================================
struct AudioFormat {
   /** Sample rate in Hz (e.g., 44100, 48000). */
   uint32_t sampleRate = 0;

   /** Number of interleaved channels (1 = mono, 2 = stereo). */
   uint32_t channels = 0;

   /** Bits per sample (16 = 16-bit signed integer, CDDA standard). */
   uint32_t bitsPerSample = 0;

   /**
    * Bytes per frame (channels * bitsPerSample / 8).
    *
    * For stereo 16-bit: 4 bytes/frame (2 channels × 2 bytes).
    * For stereo 32-bit float: 8 bytes/frame (Core Audio native format).
    */
   uint32_t bytesPerFrame = 0;

   /**
    * Bytes per packet (for compressed formats; always equals bytesPerFrame
    * for uncompressed formats like AIFF).
    */
   uint32_t bytesPerPacket = 0;

   /**
    * Frames per packet (for compressed formats; always 1 for uncompressed).
    */
   uint32_t framesPerPacket = 0;

   /**
    * True if the format is interleaved (stereo: LRLRLR...).
    *
    * Explicit boolean semantics.
    *
    * Note: `mIsInterleaved` was removed from `AudioStreamBasicDescription`
    * in macOS 12+. For uncompressed formats (AIFF), we always assume
    * interleaved, which is the standard for music production.
    */
   bool interleaved = false;

   // Convert to a human-readable string for logging/debugging.
   [[nodiscard]] std::string toString() const;
};

// ============================================================================
// CaptureConfig — Configuration for a capture session.
// Aggregate type with no hidden state or side effects.
// This is the single source of truth for how the capture behaves.
// ============================================================================
struct CaptureConfig {
   // Target output file path (e.g., "output.aiff").
   std::string outputPath;

   // Duration of capture in seconds (0 = indefinite, stop on Ctrl-C).
   // Explicit 0 means "no limit" rather than "error."
   double durationSeconds = 0.0;

   // Name of the BlackHole device to capture from.
   // Default to the 2-channel variant.
   std::string deviceName = "BlackHole 2ch";

   // True if the program should print verbose progress to stderr.
   // Verbose mode is a boolean flag, not a global.
   bool verbose = false;
};

#endif // AIFFCAPTURE_AUDIO_TYPES_H

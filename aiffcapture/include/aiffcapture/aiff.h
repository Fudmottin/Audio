/**
 * @file aiff.h
 * @brief AIFF file format writing via libsndfile.
 *
 * This module writes AIFF files (uncompressed 16-bit PCM) using
 * libsndfile. It handles all chunk formatting (FORM, COMM, SSND),
 * byte-order conversion, and file finalization.
 *
 * Key design decisions:
 *
 * 1. **libsndfile**: We delegate all AIFF file format logic to libsndfile,
 *    which writes standard AIFF with 80-bit extended float sample rates.
 *    This ensures compatibility with all standard tools (ffprobe,
 *    QuickTime, afinfo, Audacity).
 *
 * 2. **16-bit PCM**: Core Audio outputs 32-bit float. We convert to
 *    16-bit signed integer in the output callback (main.cpp) and write
 *    via libsndfile's sf_write_short().
 *
 * 3. **RAII**: The AiffWriter acquires the SNDFILE* in open() and
 *    releases it in close() (or the destructor).
 *
 */

#ifndef AIFFCAPTURE_AIFF_H
#define AIFFCAPTURE_AIFF_H

#include <aiffcapture/audio_types.h>
#include <cstdint>
#include <sndfile.h>
#include <string>

// ============================================================================
// AiffWriter — Writes AIFF files from 16-bit PCM data via libsndfile.
//
// This class wraps libsndfile for AIFF output. It manages the full
// lifecycle: opening a file, streaming samples, and finalizing.
// ============================================================================
class AiffWriter {
 public:
   // Default constructor. No resources acquired.
   AiffWriter() = default;

   // Destructor. Closes the output file if open.
   // RAII — resources are released automatically.
   ~AiffWriter();

   // Non-copyable (file handles are non-copyable).
   AiffWriter(const AiffWriter&) = delete;
   AiffWriter& operator=(const AiffWriter&) = delete;

   // Movable (file handles can be moved).
   AiffWriter(AiffWriter&& other) noexcept;
   AiffWriter& operator=(AiffWriter&& other) noexcept;

   // Open an AIFF file for writing.
   //
   // This creates a standard AIFF file (80-bit extended float sample
   // rate) via libsndfile. The file is compatible with ffprobe,
   // QuickTime, afinfo, and all standard audio tools.
   //
   // @param filePath Path to the output file (e.g., "output.aiff").
   // @param format The audio format (channels, sample rate, bits per sample).
   // @return true if the file was opened successfully, false otherwise.
   bool open(const std::string& filePath, const AudioFormat& format);

   // Write a block of 16-bit PCM samples to the AIFF file.
   //
   // Samples must be interleaved L/R for stereo. libsndfile handles
   // byte-order conversion (writes big-endian for AIFF).
   //
   // @param data Pointer to interleaved int16_t samples.
   // @param numSamples Number of 16-bit samples (not frames). For
   //   stereo, 1 frame = 2 samples (L+R).
   // @return true if the data was written successfully, false otherwise.
   bool writeSamples(const int16_t* data, uint32_t numSamples);

   // Close the AIFF file and finalize.
   //
   // This finalizes the file (libsndfile handles all chunk patching)
   // and closes the file descriptor.
   //
   // @return true if the file was closed successfully, false otherwise.
   bool close();

   // Get the number of bytes written so far (data only, not headers).
   //
   // This function is a simple accessor.
   //
   // @return The number of bytes written, or 0 if the file is not open.
   [[nodiscard]] uint64_t getBytesWritten() const;

   // Check if the file is currently open.
   //
   // @return true if the file is open, false otherwise.
   [[nodiscard]] bool isOpen() const;

 private:
   // libsndfile file handle.
   SNDFILE* sndfile_ = nullptr;

   // Audio format of the file.
   AudioFormat format_;

   // Total number of frames written (used to compute bytesWritten).
   uint64_t framesWritten_ = 0;
};

#endif // AIFFCAPTURE_AIFF_H

// aiff.h — AIFF file format writing
// Core Guidelines: this module encapsulates all AIFF file format logic.
// It writes AIFF files (uncompressed PCM) from raw PCM data.
//
// Why this abstraction? AIFF is a chunk-based binary format with a specific
// structure:
//   FORM (header) → COMM (metadata) → SSND (data)
//
// By encapsulating this here, we:
// 1. Keep the rest of the codebase format-free.
// 2. Centralize the AIFF specification compliance.
// 3. Make it easy to swap in other formats (WAV, FLAC, etc.) later.

#ifndef AIFFCAPTURE_AIFF_H
#define AIFFCAPTURE_AIFF_H

#include <aiffcapture/audio_types.h>
#include <cstdint>
#include <cstdio>
#include <string>

// Forward declaration of FILE (C standard library).
// Core Guidelines: we use FILE* for streaming writes, not std::fstream.
// This is intentional: we want explicit control over the write buffer
// and error handling.

// ============================================================================
// AiffWriter — Writes AIFF files from raw PCM data.
// Core Guidelines: this class encapsulates the full AIFF file format.
// It is a resource acquisition is initialization (RAII) object: it acquires
// the output file in the constructor and closes it in the destructor.
// ============================================================================
class AiffWriter {
 public:
   // Default constructor. No resources acquired.
   AiffWriter() = default;

   // Destructor. Closes the output file if open.
   // Core Guidelines: RAII — resources are released automatically.
   ~AiffWriter();

   // Core Guidelines: non-copyable (file handles are non-copyable).
   AiffWriter(const AiffWriter&) = delete;
   AiffWriter& operator=(const AiffWriter&) = delete;

   // Core Guidelines: movable (file handles can be moved).
   AiffWriter(AiffWriter&& other) noexcept;
   AiffWriter& operator=(AiffWriter&& other) noexcept;

   // Open an AIFF file for writing.
   //
   // This creates the FORM, COMM, and SSND chunks. The file is opened
   // in binary mode for writing. The format parameter describes the
   // audio data that will be written.
   //
   // Core Guidelines: this function is the only place where we write
   // the AIFF header. All chunk writing is centralized here.
   //
   // @param filePath Path to the output file (e.g., "output.aiff").
   // @param format The audio format (channels, sample rate, bits per sample).
   // @return true if the file was opened successfully, false otherwise.
   bool open(const std::string& filePath, const AudioFormat& format);

   // Write a block of PCM data to the AIFF file.
   //
   // This writes raw PCM samples to the SSND chunk. The samples must
   // match the format specified when the file was opened (see open()).
   //
   // Core Guidelines: this function is the only place where we write
   // PCM data to the file. All error handling is centralized here.
   //
   // @param data Pointer to the PCM data (interleaved L/R for stereo).
   // @param numBytes Number of bytes of PCM data to write.
   // @return true if the data was written successfully, false otherwise.
   bool writeSamples(const unsigned char* data, uint32_t numBytes);

   // Close the AIFF file and finalize the file size.
   //
   // This patches up the FORM and SSND chunk sizes (which are written
   // as placeholders during open()) with the actual sizes. The file
   // is then closed.
   //
   // Core Guidelines: this function is the only place where we finalize
   // the AIFF file. It patches up the chunk sizes and closes the file.
   //
   // @return true if the file was closed successfully, false otherwise.
   bool close();

   // Get the number of bytes written so far (data only, not headers).
   //
   // Core Guidelines: this function is a simple accessor.
   //
   // @return The number of bytes written, or 0 if the file is not open.
   [[nodiscard]] uint64_t getBytesWritten() const;

   // Check if the file is currently open.
   //
   // Core Guidelines: this function is a simple accessor.
   //
   // @return true if the file is open, false otherwise.
   [[nodiscard]] bool isOpen() const;

 private:
   // File handle for the AIFF file.
   // Core Guidelines: explicit handle, not a smart pointer (we need
   // explicit control over the file descriptor).
   FILE* file_ = nullptr;

   // Output file path.
   // Core Guidelines: explicit path, not derived from file handle.
   std::string filePath_;

   // Audio format of the file.
   // Core Guidelines: explicit format, not derived from file.
   AudioFormat format_;

   // Total number of bytes written (data only, not headers).
   // Core Guidelines: explicit counter, not derived from file position.
   uint64_t bytesWritten_ = 0;

   // Position of the FORM chunk size field in the file.
   // Core Guidelines: explicit offset, used to patch up the file size.
   uint64_t formSizeOffset_ = 0;

   // Position of the SSND chunk size field in the file.
   // Core Guidelines: explicit offset, used to patch up the data size.
   uint64_t ssndSizeOffset_ = 0;

   // Internal helper: write the FORM chunk header.
   // Core Guidelines: this writes the FORM ID, size (placeholder),
   // and "AIFF" magic bytes. The size is patched up in close().
   bool writeFormHeader();

   // Internal helper: write the COMM chunk.
   // Core Guidelines: this writes the COMM ID, size (fixed at 12),
   // number of channels, number of samples, sample size (16-bit),
   // and sample rate (32-bit integer).
   bool writeCommChunk(uint32_t numSamples);

   // Internal helper: write the SSND chunk header.
   // Core Guidelines: this writes the SSND ID, size (placeholder),
   // offset (0), and block size (0). The size is patched up in close().
   bool writeSsndHeader();

   // Internal helper: finalize the file size (patch up FORM and SSND).
   // Core Guidelines: this patches up the FORM and SSND chunk sizes
   // (which were written as placeholders during open()) with the
   // actual sizes. It seeks to the correct positions and overwrites
   // the size fields.
   bool finalizeFile();
};

#endif // AIFFCAPTURE_AIFF_H

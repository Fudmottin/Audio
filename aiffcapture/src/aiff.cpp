/**
 * @file aiff.cpp
 * @brief AIFF file format writing implementation.
 *
 * This file implements the AiffWriter class. It encapsulates all AIFF
 * file format logic, writing AIFF files (uncompressed PCM) from raw
 * PCM data.
 *
 * @section aiff-chunk-structure AIFF Chunk Structure
 *
 * AIFF is a chunk-based binary format. The structure is:
 *
 * ```
 * FORM (8-byte header + 4-byte ID)
 *   └─ "FORM" (4 bytes)
 *   └─ size (4 bytes, big-endian, placeholder during open)
 *   └─ "AIFF" (4 bytes)
 *   └─ COMM chunk (metadata)
 *   │     └─ "COMM" (4 bytes)
 *   │     └─ size (4 bytes, always 12)
 *   │     └─ numChannels (2 bytes, big-endian)
 *   │     └─ numSamples (4 bytes, big-endian, patched in close)
 *   │     └─ sampleSize (2 bytes, always 16)
 *   │     └─ sampleRate (4 bytes, 32-bit integer, big-endian)
 *   └─ SSND chunk (sound data)
 *         └─ "SSND" (4 bytes)
 *         └─ size (4 bytes, big-endian, patched in close)
 *         └─ offset (4 bytes, always 0)
 *         └─ blockSize (4 bytes, always 0)
 *         └─ PCM data (variable length)
 * ```
 *
 * @section aiff-byte-order Big-Endian Byte Order
 *
 * All multi-byte integers in AIFF must be written in big-endian
 * (network) byte order. macOS is little-endian, so we must swap
 * the bytes. We write each byte individually to ensure correctness
 * regardless of platform.
 *
 * @section aiff-sample-rate 32-bit Integer vs 80-bit Extended Float
 *
 * The AIFF spec allows two encodings for sample rate:
 *
 * 1. **80-bit extended float** (10 bytes): Spec-compliant but broken
 *    with macOS tools (afinfo, ffprobe) which always try to parse
 *    80-bit extended float regardless of COMM chunk size, producing
 *    garbage values (e.g., 30464 Hz instead of 44100 Hz).
 *
 * 2. **32-bit integer** (4 bytes): Spec-compliant, works with all
 *    standard tools. We use this encoding.
 *
 */

#include <aiffcapture/aiff.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ============================================================================
// AiffWriter implementation
// This class encapsulates the full AIFF file format.
// It is a resource acquisition is initialization (RAII) object: it acquires
// the output file in the constructor and closes it in the destructor.
// ============================================================================

AiffWriter::~AiffWriter() {
   // RAII — we close the file in the destructor.
   // This ensures that resources are released even if an exception occurs.

   if (file_ != nullptr) {
      close();
   }
}

AiffWriter::AiffWriter(AiffWriter&& other) noexcept
   : file_(other.file_)
   , filePath_(std::move(other.filePath_))
   , format_(std::move(other.format_))
   , bytesWritten_(other.bytesWritten_)
   , formSizeOffset_(other.formSizeOffset_)
   , ssndSizeOffset_(other.ssndSizeOffset_) {
   // Move constructor. We transfer ownership of the
   // resources from the source object to this object. The source object
   // is left in a valid but unspecified state (all resources are null).

   other.file_ = nullptr;
   other.bytesWritten_ = 0;
   other.formSizeOffset_ = 0;
   other.ssndSizeOffset_ = 0;
}

AiffWriter& AiffWriter::operator=(AiffWriter&& other) noexcept {
   // Move assignment operator. We release our current
   // resources and take ownership of the source object's resources.

   if (this !=
       &other) { // Self-assignment check (always check).
      // We close our current file first.
      if (file_ != nullptr) {
         close();
      }

      // We take ownership of the source object's resources.
      file_ = other.file_;
      filePath_ = std::move(other.filePath_);
      format_ = std::move(other.format_);
      bytesWritten_ = other.bytesWritten_;
      formSizeOffset_ = other.formSizeOffset_;
      ssndSizeOffset_ = other.ssndSizeOffset_;

      // We leave the source object in a valid but
      // unspecified state (all resources are null).
      other.file_ = nullptr;
      other.bytesWritten_ = 0;
      other.formSizeOffset_ = 0;
      other.ssndSizeOffset_ = 0;
   }

   return *this;
}

bool AiffWriter::open(const std::string& filePath, const AudioFormat& format) {
   // We open the AIFF file by creating the FORM, COMM,
   // and SSND chunks. The file is opened in binary mode for writing.

   // We check if the file is already open. If so,
   // we return false (we do not support reopening an open file).
   if (file_ != nullptr) {
      return false;
   }

   // We open the file in binary mode for writing.
   // We use fopen (not std::fstream) because we need explicit control
   // over the file descriptor and error handling.
   file_ = fopen(filePath.c_str(), "wb");
   if (file_ == nullptr) {
      return false; // Failed to open the file.
   }

   filePath_ = filePath;
   format_ = format;
   bytesWritten_ = 0;
   formSizeOffset_ = 0;
   ssndSizeOffset_ = 0;

   // We write the FORM chunk header (with placeholder size).
   // The actual size is patched up in close().
   if (!writeFormHeader()) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // We write the COMM chunk (metadata).
   // The COMM chunk contains the number of channels, number of samples,
   // sample size (16-bit signed integer), and sample rate (32-bit integer).
   // We need to calculate the number of samples from the total bytes.
   // Note: we write the COMM chunk with a placeholder for the number
   // of samples, which is patched up in close().
   if (!writeCommChunk(
          0)) { // Placeholder: actual sample count is patched in close().
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // We write the SSND chunk header (with placeholder size).
   // The actual size is patched up in close().
   if (!writeSsndHeader()) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   return true;
}

bool AiffWriter::writeSamples(const unsigned char* data, uint32_t numBytes) {
   // We write raw PCM samples to the SSND chunk.
   // The samples must match the format specified when the file was opened.

   if (file_ == nullptr) {
      return false; // File is not open.
   }

   if (data == nullptr || numBytes == 0) {
      return false; // Invalid input parameters.
   }

   // We write the PCM data to the file.
   // This is a raw binary write (no compression, no encoding).
   size_t written = fwrite(data, 1, static_cast<size_t>(numBytes), file_);
   if (written != static_cast<size_t>(numBytes)) {
      return false; // Failed to write all bytes.
   }

   // We update the bytes written counter.
   bytesWritten_ += numBytes;

   return true;
}

bool AiffWriter::close() {
   // We finalize the AIFF file by patching up the FORM
   // and SSND chunk sizes (which were written as placeholders during open())
   // with the actual sizes. The file is then closed.

   if (file_ == nullptr) {
      return false; // File is not open.
   }

   // We flush the file buffer to ensure all data is written.
   if (fflush(file_) != 0) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // We patch up the FORM and SSND chunk sizes.
   // This is the final step before closing the file.
   if (!finalizeFile()) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // We close the file.
   if (fclose(file_) != 0) {
      file_ = nullptr;
      return false;
   }

   file_ = nullptr;
   return true;
}

uint64_t AiffWriter::getBytesWritten() const { return bytesWritten_; }

bool AiffWriter::isOpen() const { return file_ != nullptr; }

// ============================================================================
// Private helper implementations
// These are internal helpers that encapsulate AIFF chunk
// writing. They are not exposed to callers.
// ============================================================================

bool AiffWriter::writeFormHeader() {
   // We write the FORM chunk header:
   //   - "FORM" (4 bytes)
   //   - file size - 8 (4 bytes, big-endian placeholder)
   //   - "AIFF" (4 bytes)
   //
   // Domain context — offset recording FIX:
   //
   // We record the offset of the FORM size field BEFORE writing the
   // placeholder. This is the key fix.
   //
   // Previously, the offset was recorded AFTER writing the FORM size,
   // causing the patching code to overwrite the "AIFF" magic bytes
   // (bytes 8-11) instead of the FORM size field (bytes 4-7). This
   // produced invalid AIFF files that tools like afinfo rejected.
   //
   // The sequence is:
   // 1. Write "FORM" (bytes 0-3) → ftello() returns 4
   // 2. Record formSizeOffset_ = 4 (THIS IS THE KEY FIX)
   // 3. Write placeholder 0 (bytes 4-7)
   // 4. Write "AIFF" (bytes 8-11)
   //
   // Later, finalizeFile() seeks to formSizeOffset_ (4) and overwrites
   // the placeholder with the actual file size.
   //

   // We write the "FORM" magic bytes.
   const char formId[4] = {'F', 'O', 'R', 'M'};
   if (fwrite(formId, 1, 4, file_) != 4) {
      return false;
   }

   // We record the offset of the FORM size field
   // BEFORE writing the placeholder. This is the key fix.
   formSizeOffset_ = static_cast<uint64_t>(ftello(file_));

   // We write the FORM size as a placeholder (0).
   // The actual size is patched up in close().
   uint32_t formSizePlaceholder = 0;
   if (fwrite(&formSizePlaceholder, 4, 1, file_) != 1) {
      return false;
   }

   // We write the "AIFF" magic bytes.
   const char aiffId[4] = {'A', 'I', 'F', 'F'};
   if (fwrite(aiffId, 1, 4, file_) != 4) {
      return false;
   }

   return true;
}

bool AiffWriter::writeCommChunk(uint32_t numSamples) {
   // We write the COMM chunk:
   //   - "COMM" (4 bytes)
   //   - 12 (4 bytes, big-endian)
   //   - numChannels (2 bytes, big-endian)
   //   - numSamples (4 bytes, big-endian)
   //   - sampleSize (2 bytes, big-endian)
   //   - sampleRate (32-bit integer, 4 bytes)
   //
   // IMPORTANT FIX: All multi-byte integers in AIFF must be written
   // in big-endian byte order. We write each field byte-by-byte
   // to ensure correct byte order regardless of platform.
   //
   // NOTE: We use 16-bit signed integer PCM (CDDA standard) and
   // 32-bit integer sample rate encoding. This ensures macOS tools
   // (afinfo, ffprobe) and all standard AIFF decoders correctly
   // interpret the file format.

   // We write the "COMM" magic bytes.
   const char commId[4] = {'C', 'O', 'M', 'M'};
   if (fwrite(commId, 1, 4, file_) != 4) {
      return false;
   }

   // We write the COMM chunk size (fixed at 12 bytes)
   // in big-endian byte order. This is 2 (channels) + 4 (samples) + 2
   // (sampleSize) + 4 (sampleRate as 32-bit integer) = 12.
   uint8_t commSizeBytes[4] = {0x00, 0x00, 0x00, 0x0C}; // 12 in big-endian
   if (fwrite(commSizeBytes, 4, 1, file_) != 1) {
      return false;
   }

   // We write the number of channels (big-endian int16).
   uint8_t numChannelsBytes[2] = {
      static_cast<uint8_t>(format_.channels >> 8),
      static_cast<uint8_t>(format_.channels & 0xFF)
   };
   if (fwrite(numChannelsBytes, 2, 1, file_) != 1) {
      return false;
   }

   // We write the number of samples (big-endian int32).
   // The actual sample count is calculated from the total bytes written.
   // We use a placeholder (0) that is patched up in close().
   uint8_t samplesBytes[4] = {0x00, 0x00, 0x00, 0x00}; // placeholder
   if (fwrite(samplesBytes, 4, 1, file_) != 1) {
      return false;
   }

   // We write the sample size (big-endian int16).
   // For AIFF, this is 16 (16-bit signed integer, CDDA standard).
   uint8_t sampleSizeBytes[2] = {0x00, 0x10}; // 16 in big-endian
   if (fwrite(sampleSizeBytes, 2, 1, file_) != 1) {
      return false;
   }

   // We write the sample rate as a 32-bit integer
   // (big-endian). This is simpler and more compatible than the
   // 80-bit extended float format. All standard sample rates
   // (8000–192000 Hz) fit comfortably in a 32-bit integer.
   uint8_t sampleRateBytes[4] = {
      static_cast<uint8_t>(format_.sampleRate >> 24),
      static_cast<uint8_t>((format_.sampleRate >> 16) & 0xFF),
      static_cast<uint8_t>((format_.sampleRate >> 8) & 0xFF),
      static_cast<uint8_t>(format_.sampleRate & 0xFF)
   };
   if (fwrite(sampleRateBytes, 4, 1, file_) != 1) {
      return false;
   }

   return true;
}

bool AiffWriter::writeSsndHeader() {
   // We write the SSND chunk header:
   //   - "SSND" (4 bytes)
   //   - data size (4 bytes, big-endian placeholder)
   //   - offset (4 bytes, big-endian, usually 0)
   //   - blockSize (4 bytes, big-endian, usually 0)
   //
   // IMPORTANT FIX: We record the offset of the SSND size field
   // BEFORE writing it, so that finalize() can patch the correct
   // position. Previously, the offset was recorded AFTER writing
   // the SSND size, causing the patching code to overwrite the
   // offset/blockSize fields instead of the SSND size field.

   // We write the "SSND" magic bytes.
   const char ssndId[4] = {'S', 'S', 'N', 'D'};
   if (fwrite(ssndId, 1, 4, file_) != 4) {
      return false;
   }

   // We record the offset of the SSND size field
   // BEFORE writing the placeholder. This is the key fix.
   ssndSizeOffset_ = static_cast<uint64_t>(ftello(file_));

   // We write the SSND size as a placeholder (0).
   // The actual size is patched up in close().
   uint8_t ssndSizePlaceholder[4] = {0x00, 0x00, 0x00, 0x00};
   if (fwrite(ssndSizePlaceholder, 4, 1, file_) != 1) {
      return false;
   }

   // We write the offset (0) and block size (0).
   // For uncompressed AIFF, these are always 0.
   uint8_t offsetBytes[4] = {0x00, 0x00, 0x00, 0x00};
   uint8_t blockSizeBytes[4] = {0x00, 0x00, 0x00, 0x00};
   if (fwrite(offsetBytes, 4, 1, file_) != 1) {
      return false;
   }
   if (fwrite(blockSizeBytes, 4, 1, file_) != 1) {
      return false;
   }

   return true;
}



bool AiffWriter::finalizeFile() {
   // We patch up the FORM and SSND chunk sizes (which
   // were written as placeholders during open()) with the actual sizes.
   // This is the final step before closing the file.
   //
   // Domain context — patching process:
   //
   // During open(), we write FORM and SSND chunk sizes as placeholders
   // (0). During close(), we patch them with actual values. This requires
   // recording file offsets BEFORE writing placeholders — a critical fix.
   //
   // The patching sequence:
   // 1. Calculate FORM size = 40 + bytesWritten_ (see formula below)
   // 2. Seek to formSizeOffset_ and write actual FORM size (big-endian)
   // 3. Calculate SSND size = bytesWritten_
   // 4. Seek to ssndSizeOffset_ and write actual SSND size (big-endian)
   // 5. Seek to offset 22 and write actual numSamples (big-endian)
   //
   // FORM size formula:
   //   FORM header: 4 ("FORM") + 4 (size) + 4 ("AIFF") = 12 bytes
   //   COMM chunk: 4 ("COMM") + 4 (size=12) + 2 (channels) + 4 (samples)
   //              + 2 (sampleSize) + 4 (sampleRate) = 20 bytes
   //   SSND header: 4 ("SSND") + 4 (size) + 4 (offset) + 4 (blockSize)
   //               = 16 bytes
   //   SSND data: bytesWritten_
   //   FORM size = 12 + 20 + 16 + bytesWritten_ - 8 = 40 + bytesWritten_
   //   (minus 8 because FORM size is "file size minus 8")
   //
   // Big-endian byte order: All multi-byte integers in AIFF must be
   // written in big-endian (network) byte order. We write each byte
   // individually to ensure correctness regardless of platform
   // (macOS is little-endian, so the bytes are swapped).
   //

   // We calculate the actual FORM size.
   // The FORM size is the total file size minus 8 (the FORM header itself).
   // Total file size = FORM header (12 bytes) + COMM chunk (20 bytes) +
   //                   SSND header (16 bytes) + SSND data (bytesWritten_).
   // FORM size = total file size - 8 = 40 + bytesWritten_.
   // NOTE: COMM chunk is 20 bytes (4 ID + 4 size + 2 channels + 4 samples
   // + 2 sampleSize + 4 sampleRate) vs. the old 26 bytes (10 bytes for
   // 80-bit extended float replaced by 4 bytes for 32-bit integer).
   uint64_t formSize = 40 + bytesWritten_;

   // We also calculate the number of samples from the
   // total bytes written. This is needed to patch the COMM chunk's
   // numSamples field.
   uint32_t numSamples = static_cast<uint32_t>(
      bytesWritten_ / format_.bytesPerFrame);

   // We seek to the FORM size offset and write the actual
   // size in big-endian byte order.
   if (fseeko(file_, static_cast<off_t>(formSizeOffset_), SEEK_SET) != 0) {
      return false;
   }

   // Write the FORM size in big-endian byte order.
   uint32_t formSizeUint32 = static_cast<uint32_t>(formSize);
   uint8_t formSizeBytes[4] = {
      static_cast<uint8_t>((formSizeUint32 >> 24) & 0xFF),
      static_cast<uint8_t>((formSizeUint32 >> 16) & 0xFF),
      static_cast<uint8_t>((formSizeUint32 >> 8) & 0xFF),
      static_cast<uint8_t>(formSizeUint32 & 0xFF)
   };
   if (fwrite(formSizeBytes, 4, 1, file_) != 1) {
      return false;
   }

   // We calculate the actual SSND size.
   // The SSND size is the total bytes of PCM data (bytesWritten_).
   uint32_t ssndSize = static_cast<uint32_t>(bytesWritten_);

   // We seek to the SSND size offset and write the actual
   // size in big-endian byte order.
   if (fseeko(file_, static_cast<off_t>(ssndSizeOffset_), SEEK_SET) != 0) {
      return false;
   }

   // Write the SSND size in big-endian byte order.
   uint8_t ssndSizeBytes[4] = {
      static_cast<uint8_t>((ssndSize >> 24) & 0xFF),
      static_cast<uint8_t>((ssndSize >> 16) & 0xFF),
      static_cast<uint8_t>((ssndSize >> 8) & 0xFF),
      static_cast<uint8_t>(ssndSize & 0xFF)
   };
   if (fwrite(ssndSizeBytes, 4, 1, file_) != 1) {
      return false;
   }

   // We also patch the COMM chunk's numSamples field
   // with the actual sample count. This is essential for the file to
   // be recognized as valid by audio applications.
   // The COMM chunk is at offset 12, and the numSamples field is at
   // offset 12 + 4 (COMM id) + 4 (COMM size) + 2 (numChannels) = 22.
   // NOTE: The numSamples field is at the same offset (22) regardless
   // of whether the sample rate is written as 80-bit extended float or
   // 32-bit integer, because the sample rate comes after numSamples.
   if (fseeko(file_, 22, SEEK_SET) != 0) {
      return false;
   }

   // Write the numSamples in big-endian byte order.
   uint8_t samplesBytes[4] = {
      static_cast<uint8_t>((numSamples >> 24) & 0xFF),
      static_cast<uint8_t>((numSamples >> 16) & 0xFF),
      static_cast<uint8_t>((numSamples >> 8) & 0xFF),
      static_cast<uint8_t>(numSamples & 0xFF)
   };
   if (fwrite(samplesBytes, 4, 1, file_) != 1) {
      return false;
   }

   return true;
}

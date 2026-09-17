// aiff.cpp — AIFF file format writing
// Core Guidelines: this file implements the AiffWriter class.
// It encapsulates all AIFF file format logic, writing AIFF files
// (uncompressed PCM) from raw PCM data.

#include <aiffcapture/aiff.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ============================================================================
// AiffWriter implementation
// Core Guidelines: this class encapsulates the full AIFF file format.
// It is a resource acquisition is initialization (RAII) object: it acquires
// the output file in the constructor and closes it in the destructor.
// ============================================================================

AiffWriter::~AiffWriter() {
   // Core Guidelines: RAII — we close the file in the destructor.
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
   // Core Guidelines: move constructor. We transfer ownership of the
   // resources from the source object to this object. The source object
   // is left in a valid but unspecified state (all resources are null).

   other.file_ = nullptr;
   other.bytesWritten_ = 0;
   other.formSizeOffset_ = 0;
   other.ssndSizeOffset_ = 0;
}

AiffWriter& AiffWriter::operator=(AiffWriter&& other) noexcept {
   // Core Guidelines: move assignment operator. We release our current
   // resources and take ownership of the source object's resources.

   if (this !=
       &other) { // Self-assignment check (Core Guidelines: always check).
      // Core Guidelines: we close our current file first.
      if (file_ != nullptr) {
         close();
      }

      // Core Guidelines: we take ownership of the source object's resources.
      file_ = other.file_;
      filePath_ = std::move(other.filePath_);
      format_ = std::move(other.format_);
      bytesWritten_ = other.bytesWritten_;
      formSizeOffset_ = other.formSizeOffset_;
      ssndSizeOffset_ = other.ssndSizeOffset_;

      // Core Guidelines: we leave the source object in a valid but
      // unspecified state (all resources are null).
      other.file_ = nullptr;
      other.bytesWritten_ = 0;
      other.formSizeOffset_ = 0;
      other.ssndSizeOffset_ = 0;
   }

   return *this;
}

bool AiffWriter::open(const std::string& filePath, const AudioFormat& format) {
   // Core Guidelines: we open the AIFF file by creating the FORM, COMM,
   // and SSND chunks. The file is opened in binary mode for writing.

   // Core Guidelines: we check if the file is already open. If so,
   // we return false (we do not support reopening an open file).
   if (file_ != nullptr) {
      return false;
   }

   // Core Guidelines: we open the file in binary mode for writing.
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

   // Core Guidelines: we write the FORM chunk header (with placeholder size).
   // The actual size is patched up in close().
   if (!writeFormHeader()) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // Core Guidelines: we write the COMM chunk (metadata).
   // The COMM chunk contains the number of channels, number of samples,
   // sample size, and sample rate (as an 80-bit extended float).
   // We need to calculate the number of samples from the total bytes.
   // Note: we write the COMM chunk with a placeholder for the number
   // of samples, which is patched up in close().
   if (!writeCommChunk(
          0)) { // Placeholder: actual sample count is patched in close().
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // Core Guidelines: we write the SSND chunk header (with placeholder size).
   // The actual size is patched up in close().
   if (!writeSsndHeader()) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   return true;
}

bool AiffWriter::writeSamples(const unsigned char* data, uint32_t numBytes) {
   // Core Guidelines: we write raw PCM samples to the SSND chunk.
   // The samples must match the format specified when the file was opened.

   if (file_ == nullptr) {
      return false; // File is not open.
   }

   if (data == nullptr || numBytes == 0) {
      return false; // Invalid input parameters.
   }

   // Core Guidelines: we write the PCM data to the file.
   // This is a raw binary write (no compression, no encoding).
   size_t written = fwrite(data, 1, static_cast<size_t>(numBytes), file_);
   if (written != static_cast<size_t>(numBytes)) {
      return false; // Failed to write all bytes.
   }

   // Core Guidelines: we update the bytes written counter.
   bytesWritten_ += numBytes;

   return true;
}

bool AiffWriter::close() {
   // Core Guidelines: we finalize the AIFF file by patching up the FORM
   // and SSND chunk sizes (which were written as placeholders during open())
   // with the actual sizes. The file is then closed.

   if (file_ == nullptr) {
      return false; // File is not open.
   }

   // Core Guidelines: we flush the file buffer to ensure all data is written.
   if (fflush(file_) != 0) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // Core Guidelines: we patch up the FORM and SSND chunk sizes.
   // This is the final step before closing the file.
   if (!finalizeFile()) {
      fclose(file_);
      file_ = nullptr;
      return false;
   }

   // Core Guidelines: we close the file.
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
// Core Guidelines: these are internal helpers that encapsulate AIFF chunk
// writing. They are not exposed to callers.
// ============================================================================

bool AiffWriter::writeFormHeader() {
   // Core Guidelines: we write the FORM chunk header:
   //   - "FORM" (4 bytes)
   //   - file size - 8 (4 bytes, big-endian placeholder)
   //   - "AIFF" (4 bytes)
   //
   // IMPORTANT FIX: We record the offset of the FORM size field
   // BEFORE writing it, so that finalize() can patch the correct
   // position. Previously, the offset was recorded AFTER writing
   // the FORM size, causing the patching code to overwrite the
   // "AIFF" magic bytes instead of the FORM size field.

   // Core Guidelines: we write the "FORM" magic bytes.
   const char formId[4] = {'F', 'O', 'R', 'M'};
   if (fwrite(formId, 1, 4, file_) != 4) {
      return false;
   }

   // Core Guidelines: we record the offset of the FORM size field
   // BEFORE writing the placeholder. This is the key fix.
   formSizeOffset_ = static_cast<uint64_t>(ftello(file_));

   // Core Guidelines: we write the FORM size as a placeholder (0).
   // The actual size is patched up in close().
   uint32_t formSizePlaceholder = 0;
   if (fwrite(&formSizePlaceholder, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the "AIFF" magic bytes.
   const char aiffId[4] = {'A', 'I', 'F', 'F'};
   if (fwrite(aiffId, 1, 4, file_) != 4) {
      return false;
   }

   return true;
}

bool AiffWriter::writeCommChunk(uint32_t numSamples) {
   // Core Guidelines: we write the COMM chunk:
   //   - "COMM" (4 bytes)
   //   - 18 (4 bytes, big-endian)
   //   - numChannels (2 bytes, big-endian)
   //   - numSamples (4 bytes, big-endian)
   //   - sampleSize (2 bytes, big-endian)
   //   - sampleRate (80-bit extended float, 10 bytes)
   //
   // IMPORTANT FIX: All multi-byte integers in AIFF must be written
   // in big-endian byte order. We write each field byte-by-byte
   // to ensure correct byte order regardless of platform.

   // Core Guidelines: we write the "COMM" magic bytes.
   const char commId[4] = {'C', 'O', 'M', 'M'};
   if (fwrite(commId, 1, 4, file_) != 4) {
      return false;
   }

   // Core Guidelines: we write the COMM chunk size (fixed at 18 bytes)
   // in big-endian byte order. This is 2 (channels) + 4 (samples) + 2
   // (sampleSize) + 10 (sampleRate as 80-bit extended float) = 18.
   uint8_t commSizeBytes[4] = {0x00, 0x00, 0x00, 0x12}; // 18 in big-endian
   if (fwrite(commSizeBytes, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the number of channels (big-endian int16).
   uint8_t numChannelsBytes[2] = {
      static_cast<uint8_t>(format_.channels >> 8),
      static_cast<uint8_t>(format_.channels & 0xFF)
   };
   if (fwrite(numChannelsBytes, 2, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the number of samples (big-endian int32).
   // The actual sample count is calculated from the total bytes written.
   // We use a placeholder (0) that is patched up in close().
   uint8_t samplesBytes[4] = {0x00, 0x00, 0x00, 0x00}; // placeholder
   if (fwrite(samplesBytes, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the sample size (big-endian int16).
   // For AIFF, this is always 32 (32-bit float).
   uint8_t sampleSizeBytes[2] = {0x00, 0x20}; // 32 in big-endian
   if (fwrite(sampleSizeBytes, 2, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the sample rate as an 80-bit extended float.
   // This is the most complex part of the AIFF format.
   if (!writeExtendedFloat(static_cast<double>(format_.sampleRate))) {
      return false;
   }

   return true;
}

bool AiffWriter::writeSsndHeader() {
   // Core Guidelines: we write the SSND chunk header:
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

   // Core Guidelines: we write the "SSND" magic bytes.
   const char ssndId[4] = {'S', 'S', 'N', 'D'};
   if (fwrite(ssndId, 1, 4, file_) != 4) {
      return false;
   }

   // Core Guidelines: we record the offset of the SSND size field
   // BEFORE writing the placeholder. This is the key fix.
   ssndSizeOffset_ = static_cast<uint64_t>(ftello(file_));

   // Core Guidelines: we write the SSND size as a placeholder (0).
   // The actual size is patched up in close().
   uint8_t ssndSizePlaceholder[4] = {0x00, 0x00, 0x00, 0x00};
   if (fwrite(ssndSizePlaceholder, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the offset (0) and block size (0).
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

bool AiffWriter::writeExtendedFloat(double value) {
   // Core Guidelines: we write the sample rate as an 80-bit IEEE 754
   // extended precision float (80 bits = 10 bytes).
   //
   // Format layout (big-endian):
   //   Byte 0: sign bit (bit 7) + 7 bits of exponent (bits 0-6)
   //   Byte 1: 8 bits of exponent (bits 7-14)
   //   Byte 2: 1 bit of exponent (bit 0) + 7 bits of significand (bits 1-7)
   //   Bytes 3-9: 56 bits of significand
   //
   // The 80-bit extended float stores:
   //   - 1 bit sign (0 = positive, 1 = negative)
   //   - 15 bits exponent (biased by 16383)
   //   - 64 bits significand with explicit leading 1
   //
   // The value represented is: (-1)^sign × 2^(exponent - 16383) × (1 + frac)
   // where frac is the 64-bit significand field interpreted as a fraction
   // in [0, 1). The leading 1 is explicit (unlike IEEE 754 which hides it).
   //
   // IMPORTANT: frexp() returns value = sig × 2^exp where sig ∈ [0.5, 1.0).
   // To convert to 80-bit format (where sig ∈ [1.0, 2.0)):
   //   80-bit exponent = 16383 + (frexp_exponent - 1)
   //   64-bit significand = floor((frexp_sig × 2.0) × 2^63)
   //
   // NOTE: double only has 53 bits of mantissa, but we need 64 bits.
   // For integer sample rates (which is all we encounter), we compute
   // the 64-bit significand exactly using integer arithmetic.

   // Core Guidelines: handle the sign bit.
   uint8_t sign = (value < 0) ? 0x80 : 0x00;
   double absValue = (value < 0) ? -value : value;

   // Core Guidelines: handle the special case of zero.
   if (absValue == 0.0) {
      uint8_t extendedFloat[10] = {};
      return fwrite(extendedFloat, 1, 10, file_) == 10;
   }

   // Core Guidelines: for integer values (sample rates), compute the
   // 64-bit significand exactly using integer arithmetic. This avoids
   // the 53-bit precision limit of double.
   uint64_t intVal = static_cast<uint64_t>(absValue);
   double absValueDouble = absValue;
   if (static_cast<double>(intVal) == absValueDouble && intVal > 0) {
      // The value is an exact integer (like 48000 Hz sample rate).
      // Find the position of the most significant bit (0-indexed).
      int msbPos = 63;
      while (msbPos > 0 && (intVal & (static_cast<uint64_t>(1) << msbPos)) == 0) {
         --msbPos;
      }

      // The 64-bit significand field stores the FRACTIONAL part (without the
      // leading 1 bit). The value is: 2^(biasedExp - 16383) × (1 + frac).
      //
      // For an integer value like 48000:
      //   value = 2^msbPos × (1 + (value - 2^msbPos)/2^msbPos)
      //   frac = (value - 2^msbPos)/2^msbPos
      //   64-bit field value = frac × 2^64 = (value - 2^msbPos) × 2^(64 - msbPos)
      //
      // NOTE: the 80-bit format's 64-bit significand field stores the
      // fractional part (NOT including the leading 1 bit). The value
      // formula is: 2^(exponent - 16383) × (1 + significand/2^64).
      //
      // For 48000 (msbPos=15):
      //   (48000 - 32768) × 2^49 = 15232 × 2^49 = 0x7700000000000000.
      //   Verify: 2^15 × (1 + 0x7700000000000000/2^64)
      //          = 32768 × (1 + 30464/65536) = 32768 × 1.46484375 = 48000.
      uint64_t significandInt =
         (intVal - (static_cast<uint64_t>(1) << msbPos)) *
         (static_cast<uint64_t>(1) << (64 - msbPos));

      // The biased exponent is 16383 + msbPos (since the value is
      // intVal = 2^msbPos × (1 + frac), where frac = (intVal - 2^msbPos)/2^msbPos).
      int biasedExp = 16383 + msbPos;

      // Build the 10-byte 80-bit extended float.
      // The 80-bit format stores the 15-bit exponent as 7(high)+8(low) bits
      // across bytes 0-1, and the 64-bit significand as 8 bytes (indices 2-9).
      // NOTE: the previous version wrote the exponent as 8(high)+7(low) and
      // wrote the significand starting at index 3, which caused an out-of-
      // bounds write to extendedFloat[10], corrupting byte 2.
      uint8_t extendedFloat[10] = {};
      extendedFloat[0] = sign | static_cast<uint8_t>((biasedExp >> 8) & 0x7F);
      extendedFloat[1] = static_cast<uint8_t>(biasedExp & 0xFF);
      for (int i = 0; i < 8; ++i) {
         extendedFloat[2 + i] =
            static_cast<uint8_t>((significandInt >> (56 - i * 8)) & 0xFF);
      }

      return fwrite(extendedFloat, 1, 10, file_) == 10;
   }

   // Core Guidelines: for non-integer values, fall back to double-based
   // computation. This has limited precision (53 bits) but is the best
   // we can do without arbitrary precision arithmetic.
   //
   // frexp(value, &exp) returns: value = sig × 2^exp, where sig ∈ [0.5, 1.0)
   //
   // For 80-bit format, we normalize to [1.0, 2.0):
   //   normalized_sig = sig × 2.0  (now in [1.0, 2.0))
   //   80-bit exponent = 16383 + (exp - 1)
   //   64-bit significand = floor(normalized_sig × 2^63)
   //
   // The key difference from the integer path:
   //   - We subtract 1 from frexp's exponent because frexp normalizes to
   //     [0.5, 1.0) but 80-bit format normalizes to [1.0, 2.0).
   //   - We multiply by 2^63 (not 2^64) because the 64-bit significand
   //     field includes the leading 1 bit at position 63.

   int frexpExp = 0;
   double frexpSig = frexp(absValue, &frexpExp);

   // Core Guidelines: compute the biased exponent.
   // frexp returns value = frexpSig × 2^frexpExp
   // where frexpSig ∈ [0.5, 1.0).
   // For 80-bit format: value = normalizedSig × 2^(biasedExp - 16383)
   // where normalizedSig = frexpSig × 2.0 ∈ [1.0, 2.0).
   // So: biasedExp = 16383 + (frexpExp - 1) = 16382 + frexpExp.
   int biasedExp = 16382 + frexpExp;

   // Core Guidelines: compute the 64-bit significand.
   // The 64-bit significand field stores the integer representation of
   // normalizedSig (which is in [1.0, 2.0)), with the leading 1 bit
   // at position 63.
   //
   // NOTE: double has only 53 bits of mantissa, so this loses precision
   // for values with more than 53 significant bits. For sample rates
   // (small integers), the integer path above handles them exactly.
   uint64_t significandInt = static_cast<uint64_t>(frexpSig * 2.0 * 9223372036854775808.0);

   // Build the 10-byte 80-bit extended float (big-endian).
   // The 15-bit exponent is split as 7(high) + 8(low) bytes 0-1.
   // The 64-bit significand is written as 8 bytes (indices 2-9).
   uint8_t extendedFloat[10] = {};
   extendedFloat[0] = sign | static_cast<uint8_t>((biasedExp >> 8) & 0x7F);
   extendedFloat[1] = static_cast<uint8_t>(biasedExp & 0xFF);
   for (int i = 0; i < 8; ++i) {
      extendedFloat[2 + i] =
         static_cast<uint8_t>((significandInt >> (56 - i * 8)) & 0xFF);
   }

   return fwrite(extendedFloat, 1, 10, file_) == 10;
}

bool AiffWriter::finalizeFile() {
   // Core Guidelines: we patch up the FORM and SSND chunk sizes (which
   // were written as placeholders during open()) with the actual sizes.
   // This is the final step before closing the file.
   //
   // IMPORTANT FIX: All multi-byte integers in AIFF must be written
   // in big-endian byte order. We write each byte individually to
   // ensure correct byte order regardless of platform.

   // Core Guidelines: we calculate the actual FORM size.
   // The FORM size is the total file size minus 8 (the FORM header itself).
   // Total file size = FORM header (12 bytes) + COMM chunk (26 bytes) +
   //                   SSND header (16 bytes) + SSND data (bytesWritten_).
   // FORM size = total file size - 8 = 46 + bytesWritten_.
   uint64_t formSize = 46 + bytesWritten_;

   // Core Guidelines: we also calculate the number of samples from the
   // total bytes written. This is needed to patch the COMM chunk's
   // numSamples field.
   uint32_t numSamples = static_cast<uint32_t>(
      bytesWritten_ / format_.bytesPerFrame);

   // Core Guidelines: we seek to the FORM size offset and write the actual
   // size in big-endian byte order.
   if (fseeko(file_, static_cast<off_t>(formSizeOffset_), SEEK_SET) != 0) {
      return false;
   }

   // Core Guidelines: write the FORM size in big-endian byte order.
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

   // Core Guidelines: we calculate the actual SSND size.
   // The SSND size is the total bytes of PCM data (bytesWritten_).
   uint32_t ssndSize = static_cast<uint32_t>(bytesWritten_);

   // Core Guidelines: we seek to the SSND size offset and write the actual
   // size in big-endian byte order.
   if (fseeko(file_, static_cast<off_t>(ssndSizeOffset_), SEEK_SET) != 0) {
      return false;
   }

   // Core Guidelines: write the SSND size in big-endian byte order.
   uint8_t ssndSizeBytes[4] = {
      static_cast<uint8_t>((ssndSize >> 24) & 0xFF),
      static_cast<uint8_t>((ssndSize >> 16) & 0xFF),
      static_cast<uint8_t>((ssndSize >> 8) & 0xFF),
      static_cast<uint8_t>(ssndSize & 0xFF)
   };
   if (fwrite(ssndSizeBytes, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we also patch the COMM chunk's numSamples field
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

   // Core Guidelines: write the numSamples in big-endian byte order.
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

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
   //   - "FORM" (4 bytes, big-endian)
   //   - file size - 8 (4 bytes, big-endian, placeholder)
   //   - "AIFF" (4 bytes, big-endian)

   // Core Guidelines: we write the "FORM" magic bytes.
   const char formId[4] = {'F', 'O', 'R', 'M'};
   if (fwrite(formId, 1, 4, file_) != 4) {
      return false;
   }

   // Core Guidelines: we write the FORM size as a placeholder (0).
   // The actual size is patched up in close().
   uint32_t formSizePlaceholder = 0;
   if (fwrite(&formSizePlaceholder, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we record the offset of the FORM size field.
   // This is used in close() to patch up the file size.
   formSizeOffset_ = static_cast<uint64_t>(ftello(file_));

   // Core Guidelines: we write the "AIFF" magic bytes.
   const char aiffId[4] = {'A', 'I', 'F', 'F'};
   if (fwrite(aiffId, 1, 4, file_) != 4) {
      return false;
   }

   return true;
}

bool AiffWriter::writeCommChunk(uint32_t numSamples) {
   // Core Guidelines: we write the COMM chunk:
   //   - "COMM" (4 bytes, big-endian)
   //   - 18 (4 bytes, fixed size)
   //   - numChannels (2 bytes, big-endian)
   //   - numSamples (4 bytes, big-endian)
   //   - sampleSize (2 bytes, big-endian)
   //   - sampleRate (80-bit extended float, 10 bytes)

   // Core Guidelines: we write the "COMM" magic bytes.
   const char commId[4] = {'C', 'O', 'M', 'M'};
   if (fwrite(commId, 1, 4, file_) != 4) {
      return false;
   }

   // Core Guidelines: we write the COMM chunk size (fixed at 18 bytes).
   uint32_t commSize = 18;
   if (fwrite(&commSize, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the number of channels (big-endian int16).
   int16_t numChannels = static_cast<int16_t>(format_.channels);
   if (fwrite(&numChannels, 2, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the number of samples (big-endian int32).
   // Note: we use the placeholder value (0) if numSamples is 0.
   // The actual value is patched up in close().
   uint32_t samples = (numSamples == 0) ? 0 : numSamples;
   if (fwrite(&samples, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we write the sample size (big-endian int16).
   // For AIFF, this is always 32 (32-bit float).
   int16_t sampleSize = 32;
   if (fwrite(&sampleSize, 2, 1, file_) != 1) {
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
   //   - "SSND" (4 bytes, big-endian)
   //   - data size (4 bytes, big-endian, placeholder)
   //   - offset (4 bytes, big-endian, usually 0)
   //   - blockSize (4 bytes, big-endian, usually 0)

   // Core Guidelines: we write the "SSND" magic bytes.
   const char ssndId[4] = {'S', 'S', 'N', 'D'};
   if (fwrite(ssndId, 1, 4, file_) != 4) {
      return false;
   }

   // Core Guidelines: we write the SSND size as a placeholder (0).
   // The actual size is patched up in close().
   uint32_t ssndSizePlaceholder = 0;
   if (fwrite(&ssndSizePlaceholder, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we record the offset of the SSND size field.
   // This is used in close() to patch up the data size.
   ssndSizeOffset_ = static_cast<uint64_t>(ftello(file_));

   // Core Guidelines: we write the offset (0) and block size (0).
   // For uncompressed AIFF, these are always 0.
   uint32_t offset = 0;
   uint32_t blockSize = 0;
   if (fwrite(&offset, 4, 1, file_) != 1) {
      return false;
   }
   if (fwrite(&blockSize, 4, 1, file_) != 1) {
      return false;
   }

   return true;
}

bool AiffWriter::writeExtendedFloat(double value) {
   // Core Guidelines: we write the sample rate as an 80-bit extended float.
   // This is the most complex part of the AIFF format.
   //
   // The 80-bit extended float format is:
   //   - 1 bit: sign (0 = positive, 1 = negative)
   //   - 15 bits: exponent (biased by 16383)
   //   - 64 bits: significand (implicit leading 1)
   //
   // Total: 10 bytes (80 bits).

   // Core Guidelines: we convert the double to an 80-bit extended float.
   // This is done by manually encoding the sign, exponent, and significand.

   // Core Guidelines: we handle the sign bit.
   uint8_t sign = (value < 0) ? 0x80 : 0x00;

   // Core Guidelines: we handle the absolute value.
   double absValue = (value < 0) ? -value : value;

   // Core Guidelines: we handle the special case of zero.
   if (absValue == 0.0) {
      uint8_t extendedFloat[10] = {0};
      if (fwrite(extendedFloat, 1, 10, file_) != 10) {
         return false;
      }
      return true;
   }

   // Core Guidelines: we calculate the exponent and significand.
   // We use frexp to get the exponent and significand in base 2.
   int exponent = 0;
   double significand = frexp(absValue, &exponent);

   // Core Guidelines: we adjust the exponent for the 80-bit format.
   // The 80-bit format uses a bias of 16383 (not 1023 like IEEE 754).
   exponent += 16383;

   // Core Guidelines: we encode the 80-bit extended float.
   // The first byte is the sign bit (high bit).
   // The next two bytes are the exponent (high byte first).
   // The remaining 8 bytes are the significand (high byte first).

   uint8_t extendedFloat[10] = {};

   // Core Guidelines: we set the sign bit (high bit of the first byte).
   extendedFloat[0] = sign;

   // Core Guidelines: we set the exponent (15 bits, biased by 16383).
   // We split the exponent into two bytes (high and low).
   extendedFloat[1] = static_cast<uint8_t>((exponent >> 8) & 0xFF);
   extendedFloat[2] = static_cast<uint8_t>(exponent & 0xFF);

   // Core Guidelines: we set the significand (64 bits, implicit leading 1).
   // The significand is stored as a 64-bit integer (high 64 bits of the 80-bit
   // float). We normalize the significand to the range [0.5, 1.0) and encode
   // it.
   significand *= 2.0; // Normalize to [1.0, 2.0).
   significand -= 1.0; // Remove the implicit leading 1.

   // Core Guidelines: we encode the significand as 8 bytes (64 bits).
   // We multiply by 2^64 to get the integer representation.
   uint64_t significandInt =
      static_cast<uint64_t>(significand * 18446744073709551616.0);

   // Core Guidelines: we write the significand bytes (high byte first).
   for (int i = 0; i < 8; ++i) {
      extendedFloat[3 + i] =
         static_cast<uint8_t>((significandInt >> (56 - i * 8)) & 0xFF);
   }

   // Core Guidelines: we write the 80-bit extended float to the file.
   if (fwrite(extendedFloat, 1, 10, file_) != 10) {
      return false;
   }

   return true;
}

bool AiffWriter::finalizeFile() {
   // Core Guidelines: we patch up the FORM and SSND chunk sizes (which
   // were written as placeholders during open()) with the actual sizes.
   // This is the final step before closing the file.

   // Core Guidelines: we calculate the actual FORM size.
   // The FORM size is the total file size minus 8 (the FORM header itself).
   // Total file size = FORM header (12 bytes) + COMM chunk (26 bytes) +
   //                   SSND header (16 bytes) + SSND data (bytesWritten_).
   uint64_t formSize = 12 + 26 + 16 + bytesWritten_;

   // Core Guidelines: we seek to the FORM size offset and write the actual
   // size.
   if (fseeko(file_, static_cast<off_t>(formSizeOffset_), SEEK_SET) != 0) {
      return false;
   }

   uint32_t formSizeUint32 = static_cast<uint32_t>(formSize);
   if (fwrite(&formSizeUint32, 4, 1, file_) != 1) {
      return false;
   }

   // Core Guidelines: we calculate the actual SSND size.
   // The SSND size is the total bytes of PCM data (bytesWritten_).
   uint32_t ssndSize = static_cast<uint32_t>(bytesWritten_);

   // Core Guidelines: we seek to the SSND size offset and write the actual
   // size.
   if (fseeko(file_, static_cast<off_t>(ssndSizeOffset_), SEEK_SET) != 0) {
      return false;
   }

   if (fwrite(&ssndSize, 4, 1, file_) != 1) {
      return false;
   }

   return true;
}

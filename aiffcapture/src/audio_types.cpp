// audio_types.cpp — Implementation of AudioFormat and CaptureConfig
// This file implements the AudioFormat and CaptureConfig
// classes. It is the only place where these classes are implemented.

#include <aiffcapture/audio_types.h>
#include <sstream>

// ============================================================================
// AudioFormat implementation
// These are simple accessor implementations.
// ============================================================================

std::string AudioFormat::toString() const {
   // We convert the AudioFormat to a human-readable string.
   // This is useful for logging and debugging.

   std::ostringstream oss;
   oss << "AudioFormat{"
       << "sampleRate=" << sampleRate << ", channels=" << channels
       << ", bitsPerSample=" << bitsPerSample
       << ", bytesPerFrame=" << bytesPerFrame
       << ", bytesPerPacket=" << bytesPerPacket
       << ", framesPerPacket=" << framesPerPacket
       << ", interleaved=" << (interleaved ? "true" : "false") << "}";

   return oss.str();
}

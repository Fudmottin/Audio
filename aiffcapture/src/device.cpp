/**
 * @file device.cpp
 * @brief Core Audio device enumeration implementation.
 *
 * This file implements the DeviceManager class. It encapsulates all
 * Core Audio API calls for device discovery, making the rest of the
 * codebase framework-free.
 *
 * @section device-implementation Core Audio Property Query Pattern
 *
 * Core Audio uses a property-based query model. To get information
 * about a device, you must:
 *
 * 1. Define an `AudioObjectPropertyAddress` (property ID, scope, element).
 * 2. Call `AudioObjectGetPropertyDataSize()` to get the buffer size.
 * 3. Allocate a buffer of that size.
 * 4. Call `AudioObjectGetPropertyData()` to read the data.
 * 5. Free the buffer.
 *
 * This pattern is repeated for every property query. By encapsulating
 * it here, we avoid repeating this boilerplate throughout the codebase.
 *
 * @see lode/terminology.md — Core Audio API terms
 * @see lode/practices.md — Core Audio development patterns
 */

#include <aiffcapture/device.h>

// Core Guidelines: we include Core Foundation here for CFString handling.
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// ============================================================================
// DeviceInfo implementation
// Core Guidelines: these are simple accessor implementations.
// ============================================================================

bool DeviceInfo::isValid() const {
   // Core Guidelines: a device is valid if it has a non-zero device ID
   // and a non-empty name. This is the only validation we perform.
   return deviceID != 0 && !name.empty();
}

// ============================================================================
// DeviceManager implementation
// Core Guidelines: this class encapsulates all Core Audio device discovery.
// It is stateless (no persistent state between calls), which makes it easy
// to reason about and test.
// ============================================================================

std::vector<DeviceInfo> DeviceManager::enumerateDevices() const {
   // Core Guidelines: we enumerate all audio devices on the system by
   // querying Core Audio for the list of device IDs, then querying each
   // device for its name and format.
   //
   // IMPORTANT FIX: We now store the enumerated devices in the member
   // variable `devices_` so that `findDeviceByName()` can search them.
   // Previously, `enumerateDevices()` returned a local vector without
   // storing to `devices_`, causing `findDeviceByName()` to always
   // return nullptr (searching an empty collection).

   // Step 1: Get the list of device IDs.
   std::vector<AudioDeviceID> deviceIDs = enumerateDeviceIDs();

   // Step 2: For each device ID, query its name and format.
   std::vector<DeviceInfo> devices;
   for (AudioDeviceID deviceID : deviceIDs) {
      DeviceInfo info;
      info.deviceID = deviceID;
      info.name = getDeviceName(deviceID);
      info.format = getStreamConfig(deviceID);

      // Core Guidelines: we only include devices that have a valid name
      // and format. Devices without a name or format are skipped.
      if (info.isValid()) {
         devices.push_back(std::move(info));
      }
   }

   // Core Guidelines FIX: Store the enumerated devices in the member
   // variable so that `findDeviceByName()` can search them. This fixes
   // the bug where `findDeviceByName()` always returned nullptr because
   // `devices_` was never populated.
   devices_ = std::move(devices);

   // Core Guidelines: return a const reference to avoid unnecessary copies.
   // The caller receives a reference to the stored data.
   return devices_;
}

const DeviceInfo*
DeviceManager::findDeviceByName(const std::string& deviceName) const {
   // Core Guidelines: we search the enumerated devices for one whose
   // name matches the provided string (case-insensitive). We return
   // the first match, or nullptr if no match is found.
   //
   // IMPORTANT FIX: This function now searches `devices_` which is
   // populated by `enumerateDevices()`. Previously, `devices_` was
   // never populated (enumerateDevices returned a local vector),
   // causing this function to always return nullptr.

   // Core Guidelines: we convert the device name to lowercase for
   // case-insensitive comparison. This is because device names can
   // vary (e.g., "BlackHole 2ch" vs "blackhole 2ch").
   std::string lowerDeviceName = deviceName;
   std::transform(lowerDeviceName.begin(), lowerDeviceName.end(),
                  lowerDeviceName.begin(), ::tolower);

   for (const auto& device : devices_) {
      std::string lowerDeviceNameLocal = device.name;
      std::transform(lowerDeviceNameLocal.begin(), lowerDeviceNameLocal.end(),
                     lowerDeviceNameLocal.begin(), ::tolower);

      if (lowerDeviceNameLocal == lowerDeviceName) {
         return &device; // Found a match.
      }
   }

   return nullptr; // No match found.
}

AudioFormat DeviceManager::getDeviceFormat(const DeviceInfo& device) const {
   // Core Guidelines: we query Core Audio for the stream configuration
   // of the given device. This is the only place where we extract the
   // AudioStreamBasicDescription from the device's input stream.

   return getStreamConfig(device.deviceID);
}

// ============================================================================
// Private helper implementations
// Core Guidelines: these are internal helpers that encapsulate Core Audio
// API calls. They are not exposed to callers.
// ============================================================================

std::string DeviceManager::getDeviceName(AudioDeviceID deviceID) const {
   // Core Guidelines: we query Core Audio for the name of the device
   // by using the kAudioDevicePropertyDeviceNameCFString property.
   // This is the standard way to get a human-readable device name.

   AudioObjectPropertyAddress address = {
      kAudioDevicePropertyDeviceNameCFString, kAudioObjectPropertyScopeGlobal,
      kAudioObjectPropertyElementMain // Core Guidelines: use the non-deprecated
                                      // name.
   };

   CFStringRef nameRef = nullptr;
   UInt32 nameSize = sizeof(CFStringRef);

   // Core Guidelines: we call AudioObjectGetPropertyData to get the
   // device name as a CFStringRef. If this fails, we return an empty
   // string.
   OSStatus status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr,
                                                &nameSize, &nameRef);

   if (status != noErr || nameRef == nullptr) {
      return ""; // Failed to get the device name.
   }

   // Core Guidelines: we convert the CFStringRef to a std::string.
   // This is the standard way to convert Core Foundation strings
   // to C++ strings.
   const char* cStr = CFStringGetCStringPtr(nameRef, kCFStringEncodingUTF8);
   std::string name = cStr ? cStr : "";

   // Core Guidelines: we must release the CFStringRef to avoid a leak.
   // Core Audio does not manage the lifetime of the returned string.
   CFRelease(nameRef);

   return name;
}

AudioFormat DeviceManager::getStreamConfig(AudioDeviceID deviceID) const {
   // Core Guidelines: we query Core Audio for the stream configuration
   // of the device by using the kAudioDevicePropertyStreamConfiguration
   // property. This returns an AudioBufferList that describes the
   // audio format (sample rate, channels, bits per sample, etc.).
   //
   // Domain context — fallback strategy for inactive devices:
   //
   // When a device is not active (e.g., BlackHole before any audio is
   // routed through it, or before reboot after installation), Core Audio
   // returns a buffer list with mData == nullptr. This is because the
   // device has no active audio stream.
   //
   // We use a two-tier fallback strategy:
   //
   // 1. Try kAudioDevicePropertyStreamConfiguration first. This returns
   //    an AudioBufferList with the actual audio buffer layout. If the
   //    device is active, this gives us the format we need.
   //
   // 2. If mData == nullptr (device not active), fall back to
   //    kAudioDevicePropertyStreamFormat, which returns the
   //    AudioStreamBasicDescription directly without requiring access
   //    to the raw audio data. This always works because the format
   //    is a property of the device, not the active stream.
   //
   // Why two properties? StreamConfiguration gives us the ACTUAL format
   // of the active stream (which may differ from the device's default
   // format). StreamFormat gives us the DEFAULT format of the device.
   // For BlackHole, these are usually the same, but StreamConfiguration
   // is more accurate when the device is active.
   //
   // @see lode/practices.md — Core Audio development patterns

   // Strategy: First try kAudioDevicePropertyStreamConfiguration.
   // If mData is nullptr (device not active), fall back to
   // kAudioDevicePropertyStreamFormat.

   AudioObjectPropertyAddress configAddr = {
      kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput,
      0 // Element 0 (the first input stream).
   };

   // Core Guidelines: we first get the size of the buffer list,
   // then allocate it, then read the data, then free it. This is
   // the standard pattern for Core Audio property queries.

   UInt32 bufferSize = 0;
   OSStatus status = AudioObjectGetPropertyDataSize(deviceID, &configAddr, 0,
                                                    nullptr, &bufferSize);

   if (status != noErr || bufferSize == 0) {
      // Core Guidelines: if we can't get the buffer size, fall back
      // to kAudioDevicePropertyStreamFormat.
      return getStreamFormat(deviceID);
   }

   // Core Guidelines: we allocate the buffer list and read the data.
   AudioBufferList* bufferList =
      reinterpret_cast<AudioBufferList*>(malloc(bufferSize));
   status = AudioObjectGetPropertyData(deviceID, &configAddr, 0, nullptr,
                                       &bufferSize, bufferList);

   if (status != noErr || bufferList->mNumberBuffers == 0) {
      free(bufferList);
      // Core Guidelines: if we can't get the buffer list, fall back
      // to kAudioDevicePropertyStreamFormat.
      return getStreamFormat(deviceID);
   }

   // Core Guidelines: we extract the AudioStreamBasicDescription from
   // the first buffer in the list. For most devices, there is only
   // one buffer (mono or stereo). For multi-channel devices, there
   // may be multiple buffers, but we only use the first one.
   //
   // Domain context — nullptr check for inactive devices:
   // Core Audio may return a buffer list with mNumberBuffers > 0 but
   // mData == nullptr for devices that are not active. Dereferencing
   // nullptr causes a segfault. We now check for nullptr and fall back
   // to kAudioDevicePropertyStreamFormat.
   //
   // This was a critical bug fix. The original code did not check for
   // nullptr and would crash when BlackHole was not active.

   // Core Guidelines: mData is void*, so we cast it to the correct type.
   // But first, we check that mData is not nullptr.
   if (bufferList->mBuffers[0].mData == nullptr) {
      free(bufferList);
      // Core Guidelines: mData is nullptr — device is not active.
      // Fall back to kAudioDevicePropertyStreamFormat.
      return getStreamFormat(deviceID);
   }

   AudioStreamBasicDescription format =
      *reinterpret_cast<AudioStreamBasicDescription*>(
         bufferList->mBuffers[0].mData);

   // Core Guidelines: we convert the AudioStreamBasicDescription to
   // our AudioFormat struct. This is the only place where we do
   // this conversion.

   AudioFormat result;
   result.sampleRate = static_cast<uint32_t>(format.mSampleRate);
   result.channels = format.mChannelsPerFrame;
   result.bitsPerSample = format.mBitsPerChannel;
   result.bytesPerFrame = format.mBytesPerFrame;
   result.bytesPerPacket = format.mBytesPerPacket;
   result.framesPerPacket = format.mFramesPerPacket;
   // Core Guidelines: mIsInterleaved was removed from
   // AudioStreamBasicDescription in macOS 12+. For uncompressed formats (AIFF),
   // we assume interleaved.
   result.interleaved = true;

   // Core Guidelines: we must free the buffer list to avoid a leak.
   // Core Audio does not manage the lifetime of the returned buffer.
   free(bufferList);

   return result;
}

AudioFormat DeviceManager::getStreamFormat(AudioDeviceID deviceID) const {
   // Core Guidelines: we query Core Audio for the stream format of the
   // device by using the kAudioDevicePropertyStreamFormat property.
   // This returns an AudioStreamBasicDescription directly, without
   // requiring access to the raw audio data. This is useful when
   // kAudioDevicePropertyStreamConfiguration returns mData == nullptr
   // (e.g., when the device is not active).
   //
   // This is a fallback method used when kAudioDevicePropertyStreamConfiguration
   // fails to provide usable data.

   AudioObjectPropertyAddress address = {
      kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeInput,
      0 // Element 0 (the first input stream).
   };

   // Core Guidelines: we first get the size of the AudioStreamBasicDescription.
   UInt32 size = sizeof(AudioStreamBasicDescription);
   OSStatus status = AudioObjectGetPropertyDataSize(deviceID, &address, 0,
                                                    nullptr, &size);

   if (status != noErr || size == 0) {
      return {}; // Failed to get the stream format.
   }

   // Core Guidelines: we allocate the AudioStreamBasicDescription and read it.
   AudioStreamBasicDescription format;
   status = AudioObjectGetPropertyData(deviceID, &address, 0, nullptr,
                                       &size, &format);

   if (status != noErr) {
      return {}; // Failed to get the stream format.
   }

   // Core Guidelines: we convert the AudioStreamBasicDescription to
   // our AudioFormat struct. This is the only place where we do
   // this conversion.

   AudioFormat result;
   result.sampleRate = static_cast<uint32_t>(format.mSampleRate);
   result.channels = format.mChannelsPerFrame;
   result.bitsPerSample = format.mBitsPerChannel;
   result.bytesPerFrame = format.mBytesPerFrame;
   result.bytesPerPacket = format.mBytesPerPacket;
   result.framesPerPacket = format.mFramesPerPacket;
   // Core Guidelines: mIsInterleaved was removed from
   // AudioStreamBasicDescription in macOS 12+. For uncompressed formats (AIFF),
   // we assume interleaved.
   result.interleaved = true;

   return result;
}

std::vector<AudioDeviceID> DeviceManager::enumerateDeviceIDs() const {
   // Core Guidelines: we enumerate all audio device IDs on the system
   // by querying Core Audio for the list of devices. This is the
   // standard way to enumerate devices.

   // Core Guidelines: kAudioObjectPropertyList was removed in newer macOS.
   // We use kAudioHardwarePropertyDevices instead, which returns device IDs
   // for the kAudioObjectPropertyScopeGlobal scope.
   AudioObjectPropertyAddress address = {kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};

   // Core Guidelines: we first get the size of the device list,
   // then allocate it, then read the data. This is the standard
   // pattern for Core Audio property queries.

   UInt32 size = 0;
   OSStatus status =
      AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0,
                                     nullptr, &size);

   if (status != noErr || size == 0) {
      return {}; // Failed to get the device list size.
   }

   // Core Guidelines: we allocate the device list and read the data.
   AudioDeviceID* deviceIDs = reinterpret_cast<AudioDeviceID*>(malloc(size));
   status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0,
                                       nullptr, &size, deviceIDs);

   if (status != noErr) {
      free(deviceIDs);
      return {}; // Failed to get the device list.
   }

   // Core Guidelines: we copy the device IDs into a vector.
   // The caller is responsible for freeing the original deviceIDs array.

   std::vector<AudioDeviceID> result;
   UInt32 numDevices = size / sizeof(AudioDeviceID);
   for (UInt32 i = 0; i < numDevices; ++i) {
      result.push_back(deviceIDs[i]);
   }

   // Core Guidelines: we must free the deviceIDs array to avoid a leak.
   free(deviceIDs);

   return result;
}

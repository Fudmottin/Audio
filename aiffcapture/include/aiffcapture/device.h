// device.h — Core Audio device enumeration and BlackHole detection
// Core Guidelines: this module encapsulates all Core Audio API calls for
// device discovery. The rest of the program never calls Core Audio directly,
// which makes this module easy to test and replace.
//
// Why this abstraction? Core Audio uses property-based queries and opaque
// handles. By isolating these calls here, we:
// 1. Keep the rest of the codebase framework-free (except for AudioDeviceID).
// 2. Make it easy to swap in a different capture target (e.g., a microphone).
// 3. Centralize error handling for device enumeration.

#ifndef AIFFCAPTURE_DEVICE_H
#define AIFFCAPTURE_DEVICE_H

#include <aiffcapture/audio_types.h>
#include <string>
#include <vector>

// Core Guidelines: we include Core Audio headers here because AudioDeviceID
// is a simple typedef (uint32_t) that is safe to expose in a public header.
// This avoids the complexity of wrapping it in void*.
#include <CoreAudio/CoreAudio.h>

// ============================================================================
// DeviceInfo — Describes a single audio device found during enumeration.
// Core Guidelines: aggregate type, no hidden state.
// ============================================================================
struct DeviceInfo {
   // Unique identifier for this device (Core Audio device ID).
   // Core Guidelines: AudioDeviceID is a simple uint32_t typedef.
   // It is safe to expose in a public header.
   AudioDeviceID deviceID = 0;

   // Human-readable device name (e.g., "BlackHole 2ch").
   std::string name;

   // The audio format this device supports for recording.
   AudioFormat format;

   // True if this device is a valid recording target.
   [[nodiscard]] bool isValid() const;
};

// ============================================================================
// DeviceManager — Enumerate and locate Core Audio devices.
// Core Guidelines: this class encapsulates all Core Audio device discovery.
// It is stateless (no persistent state between calls), which makes it easy
// to reason about and test.
// ============================================================================
class DeviceManager {
 public:
   // Default constructor. No resources to acquire.
   DeviceManager() = default;

   // Destructor. No resources to release (handles are managed internally).
   ~DeviceManager() = default;

   // Core Guidelines: non-copyable (handles are non-copyable).
   DeviceManager(const DeviceManager&) = delete;
   DeviceManager& operator=(const DeviceManager&) = delete;

   // Core Guidelines: movable (handles can be moved).
   DeviceManager(DeviceManager&&) = default;
   DeviceManager& operator=(DeviceManager&&) = default;

   // Enumerate all available audio devices on the system.
   //
   // Returns a vector of DeviceInfo for each device found.
   // If no devices are found, returns an empty vector.
   //
   // Core Guidelines: this function is the only place where Core Audio's
   // property-based API is used. All error handling is centralized here.
   //
   // @return Vector of device information, or empty vector on failure.
   [[nodiscard]] std::vector<DeviceInfo> enumerateDevices() const;

   // Find a specific device by name.
   //
   // Searches the enumerated devices for one whose name matches the
   // provided string (case-insensitive). Returns the first match, or
   // nullptr if no match is found.
   //
   // Core Guidelines: this function is case-insensitive because device
   // names can vary (e.g., "BlackHole 2ch" vs "blackhole 2ch").
   //
   // @param deviceName The name to search for (case-insensitive).
   // @return Pointer to the device info, or nullptr if not found.
   [[nodiscard]] const DeviceInfo*
   findDeviceByName(const std::string& deviceName) const;

   // Get the format of a specific device.
   //
   // Queries Core Audio for the stream configuration of the given device.
   // This is the only place where we extract the AudioStreamBasicDescription
   // from the device's input stream.
   //
   // Core Guidelines: this function handles the full lifecycle of the
   // AudioBufferList allocation and deallocation. Callers never see
   // raw pointers.
   //
   // @param device The device to query (from enumerateDevices).
   // @return The audio format, or a default-constructed AudioFormat
   //         if the query fails.
   [[nodiscard]] AudioFormat getDeviceFormat(const DeviceInfo& device) const;

 private:
   // List of all enumerated devices (owned by this instance).
   // Core Guidelines: mutable because this is a cache that doesn't affect
   // the logical state of the object. It's populated by enumerateDevices()
   // and searched by findDeviceByName(). Making it mutable allows the
   // const-qualified enumerateDevices() to populate this cache.
   mutable std::vector<DeviceInfo> devices_;

   // Internal helper: query the name of a device by its device ID.
   // Core Guidelines: this is a private helper that handles Core Audio's
   // property-based name query. It returns an empty string on failure.
   [[nodiscard]] std::string getDeviceName(AudioDeviceID deviceID) const;

   // Internal helper: query the stream configuration of a device.
   // Core Guidelines: this handles the full lifecycle of the
   // AudioBufferList (allocation, extraction, deallocation).
   // It returns a default-constructed AudioFormat on failure.
   // When the device is not active (mData == nullptr), it falls back
   // to kAudioDevicePropertyStreamFormat.
   [[nodiscard]] AudioFormat getStreamConfig(AudioDeviceID deviceID) const;

   // Internal helper: query the stream format of a device.
   // Core Guidelines: this returns the AudioStreamBasicDescription
   // directly, without requiring access to the raw audio data.
   // This is used as a fallback when kAudioDevicePropertyStreamConfiguration
   // returns mData == nullptr (e.g., when the device is not active).
   [[nodiscard]] AudioFormat getStreamFormat(AudioDeviceID deviceID) const;

   // Internal helper: enumerate all audio device IDs on the system.
   // Core Guidelines: this is the only place where we call
   // AudioObjectGetPropertyData for device enumeration.
   [[nodiscard]] std::vector<AudioDeviceID> enumerateDeviceIDs() const;
};

#endif // AIFFCAPTURE_DEVICE_H

/**
 * @file recorder.h
 * @brief Audio recording session management via Core Audio IO proc.
 *
 * This module encapsulates the Core Audio recording lifecycle: opening
 * a device, starting/stopping recording, and reading PCM data via the
 * IO proc callback pattern.
 *
 * @section recorder-architecture Why the IO Proc Pattern?
 *
 * Core Audio's recording API has two patterns:
 *
 * 1. **Deprecated synchronous** (`AudioDeviceRead`): Blocks the calling
 *    thread until data is available. Unreliable, prone to dropouts.
 *
 * 2. **Modern IO proc** (`AudioDeviceCreateIOProcID`): Registers a
 *    callback that Core Audio invokes whenever audio data is available.
 *    This is the only reliable way to capture real-time audio on macOS.
 *
 * The IO proc pattern has several steps:
 * 1. Open the device and register an IO proc (`AudioDeviceCreateIOProcID`).
 * 2. Start the device (`AudioDeviceStart`).
 * 3. Core Audio calls the IO proc whenever audio data is available.
 * 4. The IO proc callback writes data to the AIFF file via an output
 *    callback (set via `setOutputCallback()`).
 * 5. Stop the device (`AudioDeviceStop`).
 * 6. Destroy the IO proc (`AudioDeviceDestroyIOProcID`).
 *
 * By encapsulating these steps here, we:
 * 1. Keep the rest of the codebase framework-free (except for AudioDeviceID).
 * 2. Centralize error handling for recording.
 * 3. Make it easy to test with mock devices.
 *
 * @section recorder-io-proc Why an IO Proc Callback?
 *
 * The IO proc is a function pointer that Core Audio calls from an
 * internal thread whenever audio data is available. We register our
 * IO proc with `AudioDeviceCreateIOProcID()`, which returns an
 * `AudioDeviceIOProcID` handle. We then start the device with
 * `AudioDeviceStart(deviceID, ioProcID)` — note that we pass the
 * ioProcID, NOT nullptr. Passing nullptr means the device starts
 * without our IO proc, so no audio data flows through the callback.
 * This was a critical bug fix in the original implementation.
 *
 * The IO proc callback calls `processInputData()`, which in turn
 * calls the output callback (set via `setOutputCallback()`). The
 * output callback is responsible for converting 32-bit float PCM
 * to 16-bit signed integer and writing it to the AIFF file.
 *
 * @see lode/terminology.md — Core Audio API terms (IO proc)
 * @see lode/practices.md — Core Audio development patterns
 */

#ifndef AIFFCAPTURE_RECORDER_H
#define AIFFCAPTURE_RECORDER_H

#include <aiffcapture/audio_types.h>
#include <aiffcapture/device.h>
#include <functional>
#include <string>

// Core Guidelines: we include Core Audio headers here because AudioDeviceID
// is a simple typedef (uint32_t) that is safe to expose in a public header.
#include <CoreAudio/CoreAudio.h>

// ============================================================================
// Recorder — Manages a Core Audio recording session.
//
// Domain context: The Recorder class manages the full lifecycle of a
// Core Audio recording session. It is a resource acquisition is
// initialization (RAII) object: it acquires the audio device in the
// constructor (via `open()`) and releases it in the destructor.
//
// Key design decisions:
//
// 1. IO proc lifecycle:
//    - `open()` registers the IO proc and allocates the read buffer.
//    - `start()` starts the device with the registered IO proc.
//    - `stop()` stops the device.
//    - Destructor calls `stop()` and `AudioDeviceDestroyIOProcID()`.
//
// 2. Buffer management:
//    - The buffer is allocated in `allocateBuffer()` based on the
//      device's block size (frames) and the format's bytes per frame.
//    - The buffer is reused for all read operations (no per-frame
//      allocation). This avoids allocation overhead during real-time
//      audio processing.
//
// 3. Output callback pattern:
//    - The IO proc calls `processInputData()`, which calls the output
//      callback (set via `setOutputCallback()`). The output callback
//      is responsible for converting 32-bit float PCM to 16-bit
//      signed integer and writing it to the AIFF file.
//    - This indirection avoids a circular dependency between Recorder
//      and AiffWriter.
//
// 4. Debugging hooks:
//    - `ioCallbackCount_` counts how many times the IO proc was called.
//    - `totalBytesReceived_` counts total bytes received by the IO proc.
//    - If `ioCallbackCount` is 0, no audio is flowing through the device.
//    - If `totalBytesReceived` is 0 but `ioCallbackCount > 0`, the device
//      is idle (no audio playing through it).
//
// Core Guidelines: this class encapsulates the full recording lifecycle.
// It is a resource acquisition is initialization (RAII) object: it acquires
// the audio device in the constructor and releases it in the destructor.
// ============================================================================
class Recorder {
 public:
   // Default constructor. No resources acquired.
   Recorder() = default;

   // Destructor. Releases the audio device if open.
   // Core Guidelines: RAII — resources are released automatically.
   ~Recorder();

   // Core Guidelines: non-copyable (handles are non-copyable).
   Recorder(const Recorder&) = delete;
   Recorder& operator=(const Recorder&) = delete;

   // Core Guidelines: movable (handles can be moved).
   Recorder(Recorder&& other) noexcept;
   Recorder& operator=(Recorder&& other) noexcept;

   // Open the specified device for recording.
   //
   // This is the only place where we call AudioDeviceCreate. It opens the
   // device and prepares it for recording. The device must be valid (from
   // DeviceManager::enumerateDevices).
   //
   // Core Guidelines: this function is the only place where we create
   // an AudioDeviceHandle. All error handling is centralized here.
   //
   // @param device The device to open (from DeviceManager::enumerateDevices).
   // @param format The audio format to use (from
   // DeviceManager::getDeviceFormat).
   // @return true if the device was opened successfully, false otherwise.
   bool open(const DeviceInfo& device, const AudioFormat& format);

   // Start recording.
   //
   // This starts the audio device's recording stream. The device must be
   // open (open() must have returned true).
   //
   // Core Guidelines: this function is the only place where we call
   // AudioDeviceStart. All error handling is centralized here.
   //
   // @return true if recording started successfully, false otherwise.
   bool start();

   // Stop recording.
   //
   // This stops the audio device's recording stream. The device must be
   // open (open() must have returned true).
   //
   // Core Guidelines: this function is the only place where we call
   // AudioDeviceStop. All error handling is centralized here.
   //
   // @return true if recording stopped successfully, false otherwise.
   bool stop();

   // Read a single buffer of PCM data from the recording stream.
   //
   // This reads one block of samples from the device. The buffer is
   // pre-allocated in the object (see allocateBuffer()). The caller
   // is responsible for writing the data to disk.
   //
   // Core Guidelines: this function is the only place where we call
   // AudioDeviceRead. All error handling is centralized here.
   //
   // @param[out] buffer Pointer to a pointer that will receive the buffer.
   //                    The caller must not free this buffer (it is owned
   //                    by this object).
   // @param[out] bytesWritten Number of bytes written to the buffer.
   // @return true if data was read successfully, false otherwise.
   bool readBuffer(unsigned char** buffer, uint32_t* bytesWritten);

   // Get the buffer size (in bytes) for a single read operation.
   //
   // This is the block size reported by the device. It determines how
   // much data is read per call to readBuffer().
   //
   // Core Guidelines: this function is the only place where we query
   // the device's block size. It is called once during open().
   //
   // @return The buffer size in bytes, or 0 if the device is not open.
   [[nodiscard]] uint32_t getBufferSize() const;

   // Get the audio format of the recording stream.
   //
   // Core Guidelines: this function is a simple accessor.
   //
   // @return The audio format, or a default-constructed AudioFormat
   //         if the device is not open.
   [[nodiscard]] const AudioFormat& getFormat() const;

   // Get the total number of frames recorded so far.
   //
   // Core Guidelines: this function is a simple accessor.
   //
   // @return The total number of frames recorded, or 0 if not recording.
   [[nodiscard]] uint64_t getFrameCount() const;

   // Check if the device is currently open.
   //
   // Core Guidelines: this function is a simple accessor.
   //
   // @return true if the device is open, false otherwise.
   [[nodiscard]] bool isOpen() const;

   // Process input data from the IO proc.
   //
   // Core Guidelines: this is called by the IO proc whenever new input
   // data is available. It writes the data to the AIFF file.
   //
   // @param inputData The input data (AudioBufferList).
   // @return OSStatus (noErr on success).
   OSStatus processInputData(const AudioBufferList* inputData);

   // Set the output callback for the IO proc.
   //
   // Core Guidelines: this method sets a callback that is called by the
   // IO proc whenever new input data is available. The callback receives
   // the input data as an AudioBufferList. This allows the caller to
   // process the data (e.g., write it to an AIFF file) without creating
   // a circular dependency between Recorder and AiffWriter.
   //
   // @param callback The callback function to call with input data.
   void setOutputCallback(std::function<void(const AudioBufferList*)> callback);

   // Set verbose mode for this recorder instance.
   //
   // When true, the recorder prints detailed information about each IO
   // proc callback to stderr, including the number of frames and bytes
   // received. This is useful for debugging whether audio data is
   // actually flowing through the device.
   //
   // @param verbose True to print verbose IO proc statistics.
   void setVerbose(bool verbose);

   // Get the total number of IO proc callbacks received.
   //
   // Core Guidelines: this is a simple accessor for debugging.
   //
   // @return The number of times the IO proc was called.
   [[nodiscard]] uint64_t getIoCallbackCount() const;

   // Get the total number of bytes received by the IO proc.
   //
   // Core Guidelines: this is a simple accessor for debugging.
   //
   // @return The total bytes received.
   [[nodiscard]] uint64_t getTotalBytesReceived() const;

 private:
   // Device ID of the Core Audio device.
   // Core Guidelines: AudioDeviceID is a simple uint32_t typedef.
   // It is safe to expose in a public header.
   AudioDeviceID deviceID_ = 0;

   // IO proc ID (function pointer) for the device.
   // Core Guidelines: AudioDeviceIOProcID is a typedef for the IO proc
   // function pointer. We store it separately from the device ID.
   AudioDeviceIOProcID ioProcID_ = nullptr;

   // Buffer size (in bytes) for a single read operation.
   // Core Guidelines: explicit size, not derived from other values.
   uint32_t bufferSize_ = 0;

   // Total number of frames recorded so far.
   // Core Guidelines: explicit counter, not derived from timestamps.
   uint64_t frameCount_ = 0;

   // Audio format of the recording stream.
   // Core Guidelines: explicit format, not derived from device.
   AudioFormat format_;

   // Pre-allocated buffer for reading PCM data.
   // Core Guidelines: explicit ownership, no raw pointers.
   unsigned char* buffer_ = nullptr;

   // Output callback for the IO proc.
   // Core Guidelines: this is called by the IO proc whenever new input
   // data is available. The callback receives the input data as an
   // AudioBufferList. This allows the caller to process the data
   // (e.g., write it to an AIFF file) without creating a circular
   // dependency between Recorder and AiffWriter.
   std::function<void(const AudioBufferList*)> outputCallback_;

   // Verbose mode flag for debugging IO proc callbacks.
   // Core Guidelines: when true, prints detailed statistics about each
   // IO proc callback to stderr.
   bool verbose_ = false;

   // Counter for the number of IO proc callbacks received.
   // Core Guidelines: used for debugging whether audio data is flowing.
   uint64_t ioCallbackCount_ = 0;

   // Total bytes received by the IO proc.
   // Core Guidelines: used for debugging whether audio data is flowing.
   uint64_t totalBytesReceived_ = 0;

   // Internal helper: allocate the read buffer.
   // Core Guidelines: this function allocates the buffer based on the
   // device's block size and format. It is called once during open().
   bool allocateBuffer();

   // Internal helper: deallocate the read buffer.
   // Core Guidelines: this function frees the buffer. It is called
   // once during close().
   void deallocateBuffer();
};

#endif // AIFFCAPTURE_RECORDER_H

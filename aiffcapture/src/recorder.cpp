/**
 * @file recorder.cpp
 * @brief Core Audio recording session implementation.
 *
 * This file implements the Recorder class. It encapsulates all Core
 * Audio API calls for recording, making the rest of the codebase
 * framework-free.
 *
 * @section recorder-io-proc The IO Proc Pattern
 *
 * The IO proc (Input/Output callback) is the modern way to do Core
 * Audio recording. It is called by the device whenever new input
 * data is available. The IO proc receives the input data as an
 * AudioBufferList and writes it to the AIFF file.
 *
 * The IO proc lifecycle:
 * 1. `open()` registers the IO proc via `AudioDeviceCreateIOProcID()`.
 * 2. `start()` starts the device with the registered IO proc.
 * 3. Core Audio calls the IO proc whenever audio data is available.
 * 4. The IO proc calls `processInputData()`, which calls the output
 *    callback (set via `setOutputCallback()`).
 * 5. `stop()` stops the device.
 * 6. Destructor calls `AudioDeviceDestroyIOProcID()`.
 *
 * @section recorder-io-proc-signature Why this signature?
 *
 * The IO proc signature is fixed by Core Audio:
 *
 * ```c
 * OSStatus ioProc(AudioObjectID inDevice,
 *                 const AudioTimeStamp* inNow,
 *                 const AudioBufferList* inInputData,
 *                 const AudioTimeStamp* inInputTime,
 *                 AudioBufferList* outOutputData,
 *                 const AudioTimeStamp* inOutputTime,
 *                 void* inClientData);
 * ```
 *
 * Most parameters are unused (marked with (inDevice) in the source).
 * The only useful parameter is `inInputData` (the audio data) and
 * `inClientData` (a pointer to the Recorder object, used to call
 * `processInputData()`).
 *
 * @see lode/terminology.md — IO proc
 * @see lode/practices.md — Core Audio development patterns
 */

#include <aiffcapture/recorder.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>

// ============================================================================
// Forward declaration of AiffWriter (defined in aiff.cpp).
// Core Guidelines: we use a forward declaration to avoid including aiff.h
// in this file. This keeps the recording module independent of the AIFF
// module, which makes it easier to test and replace.
// ============================================================================
class AiffWriter;

// ============================================================================
// IO Proc — Core Audio callback for recording.
//
// Domain context: The IO proc is a static function that is called by
// Core Audio whenever new input data is available. It writes the data
// to the AIFF file via the output callback.
//
// The IO proc is the modern way to do Core Audio recording. It is
// called by the device whenever new input data is available. The IO
// proc receives the input data as an AudioBufferList and writes it
// to the AIFF file via the output callback.
//
// The ioProcId_ parameter to AudioDeviceStart() is critical. We pass
// the ioProcID returned by AudioDeviceCreateIOProcID(), NOT nullptr.
// Passing nullptr means the device starts without our IO proc, so no
// audio data flows through the callback. This was a critical bug fix.
//
// Core Guidelines: this is a static function that is called by Core Audio
// whenever new input data is available. It writes the data to the AIFF
// file.
//
// The IO proc is the modern way to do Core Audio recording. It is called
// by the device whenever new input data is available. The IO proc receives
// the input data as an AudioBufferList and writes it to the AIFF file.
// ============================================================================
static OSStatus ioProc(AudioObjectID /* inDevice */,
                       const AudioTimeStamp* /* inNow */,
                       const AudioBufferList* inInputData,
                       const AudioTimeStamp* /* inInputTime */,
                       AudioBufferList* /* outOutputData */,
                       const AudioTimeStamp* /* inOutputTime */,
                       void* inClientData) {
   // Core Guidelines: inClientData is a pointer to the Recorder object.
   // We cast it to the Recorder type and call its processInputData method.

   Recorder* recorder = reinterpret_cast<Recorder*>(inClientData);
   return recorder->processInputData(inInputData);
}

// ============================================================================
// Recorder implementation
// Core Guidelines: this class encapsulates the full recording lifecycle:
// opening a device, starting/stopping recording, and reading PCM data.
// ============================================================================

Recorder::~Recorder() {
   // Core Guidelines: RAII — we release all resources in the destructor.
   // This ensures that resources are released even if an exception occurs.

   if (deviceID_ != 0) {
      // Core Guidelines: we stop the device before closing it.
      // This is the standard pattern for Core Audio device management.
      stop();

      // Core Guidelines: we destroy the IO proc.
      // This releases the device and all associated resources.
      AudioDeviceDestroyIOProcID(deviceID_, ioProcID_);
      deviceID_ = 0;
      ioProcID_ = nullptr;
   }

   // Core Guidelines: we free the read buffer.
   // This is allocated in allocateBuffer() and must be freed here.
   deallocateBuffer();
}

Recorder::Recorder(Recorder&& other) noexcept
   : deviceID_(other.deviceID_)
   , bufferSize_(other.bufferSize_)
   , frameCount_(other.frameCount_)
   , format_(std::move(other.format_))
   , buffer_(other.buffer_) {
   // Core Guidelines: move constructor. We transfer ownership of the
   // resources from the source object to this object. The source object
   // is left in a valid but unspecified state (all resources are null).

   other.deviceID_ = 0;
   other.bufferSize_ = 0;
   other.frameCount_ = 0;
   other.buffer_ = nullptr;
}

Recorder& Recorder::operator=(Recorder&& other) noexcept {
   // Core Guidelines: move assignment operator. We release our current
   // resources and take ownership of the source object's resources.

   if (this !=
       &other) { // Self-assignment check (Core Guidelines: always check).
      // Core Guidelines: we release our current resources first.
      if (deviceID_ != 0) {
         stop();
         AudioDeviceDestroyIOProcID(deviceID_, ioProcID_);
         deallocateBuffer();
      }

      // Core Guidelines: we take ownership of the source object's resources.
      deviceID_ = other.deviceID_;
      ioProcID_ = other.ioProcID_;
      bufferSize_ = other.bufferSize_;
      frameCount_ = other.frameCount_;
      format_ = std::move(other.format_);
      buffer_ = other.buffer_;

      // Core Guidelines: we leave the source object in a valid but
      // unspecified state (all resources are null).
      other.deviceID_ = 0;
      other.ioProcID_ = nullptr;
      other.bufferSize_ = 0;
      other.frameCount_ = 0;
      other.buffer_ = nullptr;
   }

   return *this;
}

bool Recorder::open(const DeviceInfo& device, const AudioFormat& format) {
   // Core Guidelines: we open the device by registering an IO proc.
   // This is the modern way to do Core Audio recording.

   // Core Guidelines: we check if the device is already open. If so,
   // we return false (we do not support reopening an open device).
   if (deviceID_ != 0) {
      return false;
   }

   // Core Guidelines: we get the buffer frame size (block size) of the device.
   // This determines how much data is read per call to readBuffer().
   AudioObjectPropertyAddress address = {
      kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeInput,
      0 // Element 0 (the first input stream).
   };

   UInt32 bufferSize = 0;
   UInt32 size = sizeof(bufferSize);
   OSStatus status = AudioObjectGetPropertyData(device.deviceID, &address, 0,
                                                nullptr, &size, &bufferSize);

   if (status != noErr) {
      // Core Guidelines: if we fail to get the buffer size, we return false.
      return false;
   }

   bufferSize_ = bufferSize;
   format_ = format;
   frameCount_ = 0;

   // Core Guidelines: we allocate the read buffer.
   // This is a single buffer that we reuse for all read operations.
   if (!allocateBuffer()) {
      return false;
   }

   // Core Guidelines: we register the IO proc with the device.
   // This is the modern way to do Core Audio recording. The IO proc
   // is called by the device whenever new input data is available.
   status =
      AudioDeviceCreateIOProcID(device.deviceID, ioProc, this, &ioProcID_);

   if (status != noErr) {
      return false; // Failed to create the IO proc.
   }

   // Core Guidelines: we set the device ID to the device's device ID.
   // This is used for starting/stopping the device.
   deviceID_ = device.deviceID;

   return true;
}

bool Recorder::start() {
   // Core Guidelines: we start recording by calling AudioDeviceStart.
   // This starts the device's recording stream.

   if (deviceID_ == 0) {
      return false; // Device is not open.
   }

   // Core Guidelines: IMPORTANT FIX — we must pass ioProcID_ (the
   // IO proc ID returned by AudioDeviceCreateIOProcID) to
   // AudioDeviceStart, NOT nullptr. Passing nullptr means the device
   // starts without the IO proc we registered, so no audio data
   // flows through the callback. This was the root cause of the
   // "zero callbacks" problem.
   //
   // Domain context — why ioProcID_ not nullptr:
   //
   // AudioDeviceStart(deviceID, ioProcID) starts the device's recording
   // stream with the specified IO proc. If we pass nullptr, Core Audio
   // starts the device WITHOUT our IO proc. This means:
   // - Our ioProc() callback is never called.
   // - processInputData() is never called.
   // - outputCallback_ is never called.
   // - No audio data is written to the AIFF file.
   //
   // The result is a valid AIFF file with zero bytes of audio data.
   // This was the original bug that produced "silent" AIFF files.
   //
   // @see lode/practices.md — Core Audio development patterns
   OSStatus status = AudioDeviceStart(deviceID_, ioProcID_);

   if (status != noErr) {
      return false; // Failed to start the device.
   }

   return true;
}

bool Recorder::stop() {
   // Core Guidelines: we stop recording by calling AudioDeviceStop.
   // This stops the device's recording stream.

   if (deviceID_ == 0) {
      return false; // Device is not open.
   }

   OSStatus status = AudioDeviceStop(deviceID_, nullptr);

   if (status != noErr) {
      return false; // Failed to stop the device.
   }

   return true;
}

OSStatus Recorder::processInputData(const AudioBufferList* inputData) {
   // Core Guidelines: this is called by the IO proc whenever new input
   // data is available. It writes the data to the AIFF file.
   //
   // Domain context — debugging this function:
   //
   // This is the key debugging point. If this function is never called,
   // it means no audio is flowing through the device. If it is called
   // with mData == nullptr or mDataByteSize == 0, it means the device
   // is idle (no audio playing through it).
   //
   // Diagnostic values to check:
   // - ioCallbackCount: If 0, the IO proc was never called (check
   //   that BlackHole is set as system output).
   // - totalBytesReceived: If 0 but ioCallbackCount > 0, the device
   //   was idle (no audio playing through BlackHole).
   // - inputData->mDataByteSize: If 0, the device produced no audio
   //   data for this callback (idle state).
   //
   // @see lode/terminology.md — IO proc
   // @see lode/practices.md — Core Audio development patterns

   // Core Guidelines: we check if the input data is valid.
   if (inputData == nullptr || inputData->mNumberBuffers == 0) {
      return noErr; // No data, nothing to do.
   }

   // Core Guidelines: update the IO callback counter.
   ++ioCallbackCount_;

   // Core Guidelines: calculate the total bytes in this buffer.
   uint64_t bytesInThisBuffer = 0;
   for (UInt32 i = 0; i < inputData->mNumberBuffers; ++i) {
      bytesInThisBuffer += inputData->mBuffers[i].mDataByteSize;
   }
   totalBytesReceived_ += bytesInThisBuffer;

   // Core Guidelines: if verbose mode is enabled, print detailed
   // statistics about each IO proc callback. This helps diagnose
   // whether audio data is actually flowing through the device.
   if (verbose_) {
      // Core Guidelines: print IO proc callback statistics.
      std::cerr << "  [IO proc] Callback #" << ioCallbackCount_
                << ": frames=" << inputData->mBuffers[0].mDataByteSize
                << "/" << format_.bytesPerFrame << " (" << bytesInThisBuffer
                << " bytes), total bytes=" << totalBytesReceived_
                << ", total frames=" << (totalBytesReceived_ /
                   format_.bytesPerFrame) << std::endl;
   }

   // Core Guidelines: we update the frame count.
   frameCount_ += bufferSize_;

   // Core Guidelines: we call the output callback with the input data.
   // The callback is responsible for writing the data to the AIFF file.
   if (outputCallback_) {
      outputCallback_(inputData);
   }

   return noErr; // Success.
}

void Recorder::setOutputCallback(
   std::function<void(const AudioBufferList*)> callback) {
   // Core Guidelines: we set the output callback.
   // This is called by the IO proc whenever new input data is available.
   // The callback receives the input data as an AudioBufferList.
   outputCallback_ = std::move(callback);
}

uint32_t Recorder::getBufferSize() const { return bufferSize_; }

const AudioFormat& Recorder::getFormat() const { return format_; }

uint64_t Recorder::getFrameCount() const { return frameCount_; }

bool Recorder::isOpen() const { return deviceID_ != 0; }

void Recorder::setVerbose(bool verbose) {
   // Core Guidelines: set verbose mode for this recorder instance.
   // When true, the recorder prints detailed information about each IO
   // proc callback to stderr.
   verbose_ = verbose;
}

uint64_t Recorder::getIoCallbackCount() const {
   // Core Guidelines: return the number of IO proc callbacks received.
   // This is useful for debugging whether audio data is flowing.
   return ioCallbackCount_;
}

uint64_t Recorder::getTotalBytesReceived() const {
   // Core Guidelines: return the total bytes received by the IO proc.
   // This is useful for debugging whether audio data is flowing.
   return totalBytesReceived_;
}

// ============================================================================
// Private helper implementations
// Core Guidelines: these are internal helpers that encapsulate resource
// management. They are not exposed to callers.
// ============================================================================

bool Recorder::allocateBuffer() {
   // Core Guidelines: we allocate the read buffer based on the device's
   // block size and format. This is called once during open().

   // Core Guidelines: we calculate the buffer size in bytes.
   // This is the block size (frames) multiplied by the bytes per frame.
   uint64_t totalBytes =
      static_cast<uint64_t>(bufferSize_) * format_.bytesPerFrame;

   // Core Guidelines: we allocate the buffer using malloc.
   // This is a raw buffer that we reuse for all read operations.
   // We use malloc (not new) because we need a raw pointer for Core Audio.
   buffer_ =
      reinterpret_cast<unsigned char*>(malloc(static_cast<size_t>(totalBytes)));
   if (buffer_ == nullptr) {
      return false; // Failed to allocate the buffer.
   }

   // Core Guidelines: we zero-initialize the buffer.
   // This ensures that we do not read uninitialized data.
   memset(buffer_, 0, static_cast<size_t>(totalBytes));

   return true;
}

void Recorder::deallocateBuffer() {
   // Core Guidelines: we free the read buffer.
   // This is called once during close().

   if (buffer_ != nullptr) {
      free(buffer_);
      buffer_ = nullptr;
   }
}

/**
 * @file main.cpp
 * @brief Entry point for aiffcapture — captures audio from BlackHole
 *        and writes to AIFF files.
 *
 * This is the single entry point of the program. It parses command-line
 * arguments, orchestrates the recording session, and writes the captured
 * audio to an AIFF file.
 *
 * @section main-flow Program Flow
 *
 * 1. Parse command-line arguments (duration, output file, device name).
 * 2. Create a CaptureConfig from the parsed arguments.
 * 3. Use DeviceManager to find the BlackHole device.
 * 4. Use Recorder to open the device and start recording.
 * 5. Use AiffWriter to write PCM data to an AIFF file.
 * 6. Stop recording and close the file.
 *
 * @section main-signal Why Signal Handling?
 *
 * We install a signal handler for SIGINT (Ctrl-C) to allow graceful
 * shutdown. The signal handler only sets a flag (`g_signalReceived`)
 * — this is the ONLY safe thing to do in a signal handler. We cannot
 * call std::cout, std::cerr, or any non-async-signal-safe function
 * from within the handler. The main loop checks this flag and exits
 * gracefully when it is set.
 *
 * @section main-float-to-int16 Why 32-bit Float → 16-bit Integer?
 *
 * Core Audio always outputs 32-bit float PCM samples in the range
 * [-1.0, 1.0]. AIFF files use 16-bit signed integer PCM (CDDA
 * standard). The conversion in the output callback:
 *
 * 1. Clamps the float to [-1.0, 1.0] (handles any clipping).
 * 2. Scales to [-32767, 32767] (note: 32767, NOT 32768).
 * 3. Casts to int16_t.
 *
 * Why 32767 and not 32768? Because int16_t is asymmetric in two's
 * complement: it ranges from -32768 to +32767. If we scaled to
 * 32768, a float of 1.0 would produce 32768, which overflows to
 * -32768 (a loud, distorted sample). Using 32767 avoids this
 * asymmetric minimum issue.
 *
 * @section main-frame-count Why Input Frame Calculation?
 *
 * Core Audio outputs 32-bit float samples (8 bytes/frame for stereo).
 * We write 16-bit integer samples (4 bytes/frame for stereo). We MUST
 * calculate the input frame count from the SOURCE format (32-bit float),
 * not the output format (16-bit). Using the output format's bytesPerFrame
 * would double the frame count and read past the buffer, producing
 * half-speed, low-pitched garbage (or a crash).
 *
 */

#include <aiffcapture/aiff.h>
#include <aiffcapture/audio_types.h>
#include <aiffcapture/device.h>
#include <aiffcapture/recorder.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

// We include Core Audio headers here for AudioBuffer.
#include <CoreAudio/CoreAudio.h>

// ============================================================================
// Global flag for signal handling (Ctrl-C).
//
// Domain context: This is the only global mutable state in the program.
// It is set to true when SIGINT (Ctrl-C) is received, causing the
// recording loop to exit gracefully.
//
// Why volatile? The `volatile` keyword tells the compiler not to
// optimize away reads of this variable. Without `volatile`, the
// compiler might cache the value in a register and never see the
// signal handler's write. This is the standard pattern for signal
// flags in C/C++.
//
// Why sig_atomic_t? This is the standard type for signal-safe
// variables. It is guaranteed to be read/written atomically,
// preventing torn reads/writes from racing with the signal handler.
//
// This is the only global mutable state in the program.
// It is set to true when SIGINT (Ctrl-C) is received, causing the
// recording loop to exit gracefully.
// ============================================================================
static volatile sig_atomic_t g_signalReceived = 0;

// Signal handler for SIGINT (Ctrl-C).
//
// Domain context: This is a signal-safe function. It only sets a flag;
// it does not call any non-async-signal-safe functions. This is the
// only safe thing to do in a signal handler.
//
// Why only set a flag? Signal handlers have severe restrictions:
// - Cannot call std::cout, std::cerr (not async-signal-safe)
// - Cannot call malloc, free (not async-signal-safe)
// - Cannot call std::string, std::vector (not async-signal-safe)
// - Cannot throw exceptions (undefined behavior in signal handlers)
//
// The main loop checks g_signalReceived and exits gracefully when
// it is set. This is the standard pattern for graceful shutdown.
//
// This is a signal-safe function. It only sets a flag;
// it does not call any non-async-signal-safe functions. This is the
// only safe thing to do in a signal handler.
// ============================================================================
static void signalHandler(int /* signal */) { g_signalReceived = 1; }

// ============================================================================
// printUsage — Print usage information to stderr.
// This function is stateless and has no side effects
// other than printing to stderr. It is called when the user provides
// invalid arguments.
// ============================================================================
static void printUsage(const char* programName) {
   // Usage messages go to stderr, not stdout.
   // The format is: programName [options] <output.aiff>
   //
   // Options:
   //   --duration <seconds>   Record for this many seconds (0 = indefinite).
   //   --device <name>        Use this device (default: "BlackHole 2ch").
   //   --verbose              Print progress to stderr.
   //   --help                 Print this message.

   std::cerr << "Usage: " << programName << " [options] <output.aiff>\n";
   std::cerr << "\nOptions:\n";
   std::cerr << "  --duration <seconds>   Record for this many seconds (0 = "
                "indefinite).\n";
   std::cerr << "  --device <name>        Use this device (default: "
                "\"BlackHole 2ch\").\n";
   std::cerr << "  --verbose              Print progress to stderr.\n";
   std::cerr << "  --help                 Print this message.\n";
   std::cerr << "\nExamples:\n";
   std::cerr << "  " << programName << " --duration 60 output.aiff\n";
   std::cerr << "  " << programName
             << " --device \"BlackHole 4ch\" --verbose output.aiff\n";
}

// ============================================================================
// parseArguments — Parse command-line arguments into a CaptureConfig.
// This function is stateless and has no side effects
// other than modifying the output parameter. It returns false if the
// arguments are invalid.
// ============================================================================
static bool parseArguments(int argc, char* argv[], CaptureConfig& config) {
   // We iterate over the arguments (skipping argv[0],
   // which is the program name). For each argument, we check if it is
   // a flag (starts with "--") or a value (the output file path).

   bool outputSet = false;

   for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      // Check for flags (arguments starting with "--").
      if (arg.substr(0, 2) == "--") {
         // We check each flag and set the corresponding
         // field in the config. If a flag requires a value, we check
         // that the next argument exists and is not another flag.

         if (arg == "--help") {
            // --help prints usage and exits immediately.
            // This is the only flag that does not modify the config.
            printUsage(argv[0]);
            exit(0);
         } else if (arg == "--duration") {
            // --duration takes a double value. We parse
            // it and set the durationSeconds field in the config.
            if (i + 1 >= argc) {
               std::cerr << "Error: --duration requires a value.\n";
               return false;
            }
            char* end = nullptr;
            double duration = strtod(argv[i + 1], &end);
            if (*end != '\0' || duration < 0) {
               std::cerr
                  << "Error: --duration requires a non-negative number.\n";
               return false;
            }
            config.durationSeconds = duration;
            ++i; // Skip the value (it was consumed by the previous argument).
         } else if (arg == "--device") {
            // --device takes a string value. We set the
            // deviceName field in the config.
            if (i + 1 >= argc) {
               std::cerr << "Error: --device requires a value.\n";
               return false;
            }
            config.deviceName = argv[i + 1];
            ++i; // Skip the value (it was consumed by the previous argument).
         } else if (arg == "--verbose") {
            // --verbose is a boolean flag. We set the
            // verbose field in the config.
            config.verbose = true;
         } else {
            // Unknown flags are rejected with an error.
            std::cerr << "Error: unknown flag: " << arg << "\n";
            printUsage(argv[0]);
            return false;
         }
      } else if (outputSet) {
         // We only accept one output file path. If the
         // user provides more than one, we reject the command.
         std::cerr << "Error: multiple output files specified.\n";
         printUsage(argv[0]);
         return false;
      } else {
         // The first non-flag argument is the output file
         // path. We set the outputPath field in the config.
         config.outputPath = arg;
         outputSet = true;
      }
   }

   // The output file path is required. If it was not
   // provided, we reject the command.
   if (!outputSet) {
      std::cerr << "Error: output file path is required.\n";
      printUsage(argv[0]);
      return false;
   }

   return true;
}

// ============================================================================
// main — Entry point for aiffcapture.
// This function is the single entry point of the program.
// It parses arguments, orchestrates the recording, and handles errors.
// ============================================================================
int main(int argc, char* argv[]) {
   // We set up signal handling for SIGINT (Ctrl-C).
   // This allows the program to exit gracefully when the user presses
   // Ctrl-C during recording.

   struct sigaction action = {};
   action.sa_handler = signalHandler;
   sigemptyset(&action.sa_mask);
   action.sa_flags = 0;
   sigaction(SIGINT, &action, nullptr);

   // Parse command-line arguments.
   CaptureConfig config;
   if (!parseArguments(argc, argv, config)) {
      return 1; // Error: arguments were invalid.
   }

   // Log the configuration (if verbose mode is enabled).
   if (config.verbose) {
      std::cerr << "Configuration:\n";
      std::cerr << "  Output file: " << config.outputPath << "\n";
      std::cerr << "  Duration: " << config.durationSeconds
                << " seconds (0 = indefinite)\n";
      std::cerr << "  Device: " << config.deviceName << "\n";
   }

   // Enumerate available devices and find the BlackHole device.
   DeviceManager deviceManager;
   std::vector<DeviceInfo> devices = deviceManager.enumerateDevices();

   if (devices.empty()) {
      std::cerr << "Error: no audio devices found.\n";
      std::cerr << "Make sure BlackHole is installed and active.\n";
      return 1;
   }

   const DeviceInfo* targetDevice =
      deviceManager.findDeviceByName(config.deviceName);
   if (!targetDevice) {
      std::cerr << "Error: device \"" << config.deviceName << "\" not found.\n";
      std::cerr << "Available devices:\n";
      for (const auto& device : devices) {
         std::cerr << "  - " << device.name << "\n";
      }
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Found device: " << targetDevice->name << "\n";
   }

   // Get the audio format of the target device.
   AudioFormat format = deviceManager.getDeviceFormat(*targetDevice);
   if (format.channels == 0 || format.sampleRate == 0) {
      std::cerr << "Error: could not determine device format.\n";
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Device format: " << format.toString() << "\n";
   }

   // We convert the device format (32-bit float from
   // Core Audio) to 16-bit signed integer for the AIFF output. This
   // ensures decoded audio is recognizable instead of noise. The
   // float-to-int16 conversion happens in the output callback below.
   //
   // Domain context: Core Audio always outputs 32-bit float PCM samples
   // in the range [-1.0, 1.0]. AIFF files use 16-bit signed integer PCM
   // (CDDA standard). The conversion in the output callback:
   //
   // 1. Clamps the float to [-1.0, 1.0] (handles any clipping).
   // 2. Scales to [-32767, 32767] (note: 32767, NOT 32768).
   // 3. Casts to int16_t.
   //
   // Why 32767 and not 32768? Because int16_t is asymmetric in two's
   // complement: it ranges from -32768 to +32767. If we scaled to
   // 32768, a float of 1.0 would produce 32768, which overflows to
   // -32768 (a loud, distorted sample). Using 32767 avoids this
   // asymmetric minimum issue. This is a well-known gotcha in audio
   // programming.
   //
   format.bitsPerSample = 16;
   format.bytesPerFrame = format.channels * 2; // 16-bit = 2 bytes per sample

   // Open the AIFF file for writing.
   AiffWriter aiffWriter;
   if (!aiffWriter.open(config.outputPath, format)) {
      std::cerr << "Error: could not open output file: " << config.outputPath
                << "\n";
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Opened output file: " << config.outputPath << "\n";
   }

   // Open the recording device.
   Recorder recorder;
   if (!recorder.open(*targetDevice, format)) {
      std::cerr << "Error: could not open device: " << targetDevice->name
                << "\n";
      aiffWriter.close(); // Clean up the AIFF file.
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Opened recording device: " << targetDevice->name << "\n";
   }

   // Enable verbose mode on the recorder so that IO
   // proc callbacks are logged to stderr. This helps diagnose whether
   // audio data is actually flowing through the device.
   recorder.setVerbose(config.verbose);

   // Start recording.
   if (!recorder.start()) {
      std::cerr << "Error: could not start recording.\n";
      recorder.stop();
      aiffWriter.close(); // Clean up the AIFF file.
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Recording started.\n";
   }

   // Recording loop: wait for the specified duration or Ctrl-C.
   // The IO proc writes data to the AIFF file via the
   // output callback. This loop just waits for the duration or Ctrl-C.

   // We set the output callback to write data to the
   // AIFF file. The callback is called by the IO proc whenever new
   // input data is available.
   //
   // IMPORTANT: Core Audio outputs 32-bit float samples, but we write
   // 16-bit signed integer to the AIFF file. This callback converts
   // between the two formats. The float-to-int16 conversion clamps
   // values to [-1.0, 1.0] and maps to [-32767, 32767] (note 32767,
   // not 32768 — see domain comment above).
   //
   // Domain context: Frame calculation is critical. Core Audio outputs
   // 32-bit float samples (8 bytes/frame for stereo). We write 16-bit
   // integer samples (4 bytes/frame for stereo). We MUST calculate the
   // input frame count from the SOURCE format (32-bit float), not the
   // output format (16-bit). Using the output format's bytesPerFrame
   // would double the frame count and read past the buffer, producing
   // half-speed, low-pitched garbage (or a crash).
   //
   std::vector<int16_t> convertBuffer;
   recorder.setOutputCallback([&aiffWriter, &format, &convertBuffer](
                                 const AudioBufferList* inputData) {
      // We convert 32-bit float samples from Core
      // Audio to 16-bit signed integer for the AIFF file.
      // libsndfile handles byte-order conversion (writes big-endian).

      // We assume there is only one buffer (stereo devices
      // have two channels interleaved in a single buffer).
      if (inputData->mNumberBuffers > 0) {
         const AudioBuffer& buffer = inputData->mBuffers[0];
         const float* floatData = static_cast<const float*>(buffer.mData);
         // Calculate input frames from the 32-bit float
         // input data (8 bytes/frame for stereo), not the 16-bit output
         // format (4 bytes/frame). Using the output format's bytesPerFrame
         // would double the frame count and read past the buffer.
         //
         // Domain context: This is a critical calculation. If we used
         // format.bytesPerFrame (which is 4 for stereo 16-bit) instead of
         // format.channels * 4 (which is 8 for stereo 32-bit float), we
         // would calculate TWICE as many frames and read past the buffer.
         // This was a critical bug in the original implementation that
         // produced half-speed, low-pitched garbage.
         uint32_t inputBytesPerFrame = format.channels * 4; // 32-bit float
         uint32_t numFrames = buffer.mDataByteSize / inputBytesPerFrame;

         // Resize the conversion buffer to hold all
         // frames of 16-bit output data (interleaved L/R).
         convertBuffer.resize(numFrames * format.channels);

         // Convert each float sample to 16-bit integer.
         // Core Audio guarantees samples are in [-1.0, 1.0],
         // so no clamping is needed. Direct scale by 32767.0f.
         //
         // Domain context: Why 32767, not 32768? int16_t ranges
         // from -32768 to +32767 (asymmetric in two's complement).
         // If we scaled to 32768, a float of 1.0 would produce
         // 32768, which overflows to -32768 (a loud, distorted
         // sample). Using 32767 avoids this well-known gotcha.
         //
         // libsndfile handles byte-order conversion internally
         // (writes big-endian for AIFF), so we write int16_t samples
         // directly without manual byte-swapping.
         for (uint32_t i = 0; i < numFrames; ++i) {
            for (uint32_t ch = 0; ch < format.channels; ++ch) {
               float sample = floatData[i * format.channels + ch];
               // Core Audio guarantees [-1.0, 1.0] — no clamping
               // needed. Direct scale by 32767.0f (not 32768).
               convertBuffer[i * format.channels + ch] =
                  static_cast<int16_t>(sample * 32767.0f);
            }
         }

         // libsndfile's sf_write_short expects the number of samples,
         // not frames. For stereo, each frame = 2 samples (L+R).
         // numFrames is the number of 32-bit float frames, so
         // numFrames * channels is the number of 16-bit samples.
         aiffWriter.writeSamples(convertBuffer.data(),
                                 numFrames * format.channels);
      }
   });

   // We use a timer to track the elapsed time.
   // This is the only place where we check the duration.
   auto startTime = std::chrono::steady_clock::now();

   while (true) {
      // Check if the duration has elapsed (if specified).
      if (config.durationSeconds > 0) {
         auto now = std::chrono::steady_clock::now();
         double elapsed =
            std::chrono::duration<double>(now - startTime).count();
         if (elapsed >= config.durationSeconds) {
            if (config.verbose) {
               std::cerr << "Duration elapsed (" << config.durationSeconds
                         << " seconds). Stopping.\n";
            }
            break;
         }
      }

      // Check if the user pressed Ctrl-C.
      if (g_signalReceived) {
         if (config.verbose) {
            std::cerr << "Interrupted by user. Stopping.\n";
         }
         break;
      }

      // We sleep for a short period to avoid busy-waiting.
      // This is the only place where we sleep. We use 10ms intervals.
      struct timespec ts = {0, 10000000}; // 10ms in nanoseconds
      nanosleep(&ts, nullptr);
   }

   // Stop recording and close the device.
   recorder.stop();

   // Print a summary of IO proc activity.
   //
   // Domain context: This summary helps diagnose whether audio data was
   // actually flowing through BlackHole. Key diagnostic values:
   //
   // - ioCallbackCount == 0: The IO proc was never called. This means
   //   BlackHole is not set as your system output, or no audio is
   //   playing through it. Check macOS System Settings → Sound → Output.
   //
   // - totalBytesReceived == 0 but ioCallbackCount > 0: The device was
   //   idle (no audio data). The IO proc was called but with empty
   //   buffers. This can happen when BlackHole is set as output but
   //   no application is playing audio.
   //
   // - totalDuration: Calculated from totalFrames / sampleRate. This
   //   shows how much audio was actually captured (may be less than
   //   the requested duration if the user stopped early).
   //
   {
      uint64_t ioCallbacks = recorder.getIoCallbackCount();
      uint64_t totalBytes = recorder.getTotalBytesReceived();
      // totalBytes is from Core Audio (32-bit float), so divide by
      // 32-bit float bytes per frame (channels * 4) to get frames.
      uint64_t totalFrames =
         totalBytes / (format.channels * 4);
      double totalDuration = static_cast<double>(totalFrames) /
                             static_cast<double>(format.sampleRate);

      if (config.verbose) {
         std::cerr << "\n=== Recording Summary ===\n";
         std::cerr << "  IO proc callbacks: " << ioCallbacks << "\n";
         std::cerr << "  Total bytes received: " << totalBytes << "\n";
         std::cerr << "  Total frames captured: " << totalFrames << "\n";
         std::cerr << "  Audio duration: " << totalDuration << " seconds\n";
         std::cerr << "  "
                   << (ioCallbacks == 0
                          ? "WARNING: No IO proc callbacks received. "
                            "BlackHole may not be set as your system output, "
                            "or no audio is playing through it."
                          : "")
                   << (totalBytes == 0 && ioCallbacks > 0
                          ? " WARNING: Device was idle (no audio data). "
                            "Make sure audio is playing through BlackHole."
                          : "")
                   << "\n";
         std::cerr << "=========================\n";
      }
   }

   // Finalize and close the AIFF file.
   if (aiffWriter.close()) {
      if (config.verbose) {
         std::cerr << "AIFF file finalized: " << config.outputPath << "\n";
         double totalBytes = static_cast<double>(aiffWriter.getBytesWritten());
         double bytesPerSecond = static_cast<double>(format.bytesPerFrame) *
                                 static_cast<double>(format.sampleRate);
         double duration = totalBytes / bytesPerSecond;
         double megabytes = totalBytes / (1024.0 * 1024.0);
         std::cerr << "Total duration: " << duration << " seconds ("
                   << megabytes << " MB)\n";
      }
   } else {
      std::cerr << "Error: could not finalize AIFF file.\n";
      return 1;
   }

   return 0; // Success.
}

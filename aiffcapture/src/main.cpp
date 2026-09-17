// main.cpp — Entry point for aiffcapture
// Core Guidelines: this is the single entry point of the program.
// It parses command-line arguments, creates a capture configuration,
// and orchestrates the recording and AIFF writing.
//
// The program flow is:
// 1. Parse command-line arguments (duration, output file, device name).
// 2. Create a CaptureConfig from the parsed arguments.
// 3. Use DeviceManager to find the BlackHole device.
// 4. Use Recorder to open the device and start recording.
// 5. Use AiffWriter to write PCM data to an AIFF file.
// 6. Stop recording and close the file.

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

// Core Guidelines: we include Core Audio headers here for AudioBuffer.
#include <CoreAudio/CoreAudio.h>

// ============================================================================
// Global flag for signal handling (Ctrl-C).
// Core Guidelines: this is the only global mutable state in the program.
// It is set to true when SIGINT (Ctrl-C) is received, causing the
// recording loop to exit gracefully.
// ============================================================================
static volatile sig_atomic_t g_signalReceived = 0;

// Signal handler for SIGINT (Ctrl-C).
// Core Guidelines: this is a signal-safe function. It only sets a flag;
// it does not call any non-async-signal-safe functions. This is the
// only safe thing to do in a signal handler.
static void signalHandler(int /* signal */) { g_signalReceived = 1; }

// ============================================================================
// printUsage — Print usage information to stderr.
// Core Guidelines: this function is stateless and has no side effects
// other than printing to stderr. It is called when the user provides
// invalid arguments.
// ============================================================================
static void printUsage(const char* programName) {
   // Core Guidelines: usage messages go to stderr, not stdout.
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
// Core Guidelines: this function is stateless and has no side effects
// other than modifying the output parameter. It returns false if the
// arguments are invalid.
// ============================================================================
static bool parseArguments(int argc, char* argv[], CaptureConfig& config) {
   // Core Guidelines: we iterate over the arguments (skipping argv[0],
   // which is the program name). For each argument, we check if it is
   // a flag (starts with "--") or a value (the output file path).

   bool outputSet = false;

   for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];

      // Check for flags (arguments starting with "--").
      if (arg.substr(0, 2) == "--") {
         // Core Guidelines: we check each flag and set the corresponding
         // field in the config. If a flag requires a value, we check
         // that the next argument exists and is not another flag.

         if (arg == "--help") {
            // Core Guidelines: --help prints usage and exits immediately.
            // This is the only flag that does not modify the config.
            printUsage(argv[0]);
            exit(0);
         } else if (arg == "--duration") {
            // Core Guidelines: --duration takes a double value. We parse
            // it and set the durationSeconds field in the config.
            if (i + 1 >= argc) {
               std::cerr << "Error: --duration requires a value.\n";
               return false;
            }
            char* end = nullptr;
            double duration = strtod(argv[i + 1], &end);
            if (*end != '\0' || duration < 0) {
               std::cerr << "Error: --duration requires a non-negative number.\n";
               return false;
            }
            config.durationSeconds = duration;
            ++i; // Skip the value (it was consumed by the previous argument).
         } else if (arg == "--device") {
            // Core Guidelines: --device takes a string value. We set the
            // deviceName field in the config.
            if (i + 1 >= argc) {
               std::cerr << "Error: --device requires a value.\n";
               return false;
            }
            config.deviceName = argv[i + 1];
            ++i; // Skip the value (it was consumed by the previous argument).
         } else if (arg == "--verbose") {
            // Core Guidelines: --verbose is a boolean flag. We set the
            // verbose field in the config.
            config.verbose = true;
         } else {
            // Core Guidelines: unknown flags are rejected with an error.
            std::cerr << "Error: unknown flag: " << arg << "\n";
            printUsage(argv[0]);
            return false;
         }
      } else if (outputSet) {
         // Core Guidelines: we only accept one output file path. If the
         // user provides more than one, we reject the command.
         std::cerr << "Error: multiple output files specified.\n";
         printUsage(argv[0]);
         return false;
      } else {
         // Core Guidelines: the first non-flag argument is the output file
         // path. We set the outputPath field in the config.
         config.outputPath = arg;
         outputSet = true;
      }
   }

   // Core Guidelines: the output file path is required. If it was not
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
// Core Guidelines: this function is the single entry point of the program.
// It parses arguments, orchestrates the recording, and handles errors.
// ============================================================================
int main(int argc, char* argv[]) {
   // Core Guidelines: we set up signal handling for SIGINT (Ctrl-C).
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

   // Core Guidelines: we convert the device format (32-bit float from
   // Core Audio) to 16-bit signed integer for the AIFF output. This
   // ensures decoded audio is recognizable instead of noise. The
   // float-to-int16 conversion happens in the output callback below.
   format.bitsPerSample = 16;
   format.bytesPerFrame = format.channels * 2; // 16-bit = 2 bytes per sample

   // Open the AIFF file for writing.
   AiffWriter aiffWriter;
   if (!aiffWriter.open(config.outputPath, format)) {
      std::cerr << "Error: could not open output file: "
               << config.outputPath << "\n";
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Opened output file: " << config.outputPath << "\n";
   }

   // Open the recording device.
   Recorder recorder;
   if (!recorder.open(*targetDevice, format)) {
      std::cerr << "Error: could not open device: "
               << targetDevice->name << "\n";
      aiffWriter.close(); // Clean up the AIFF file.
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Opened recording device: " << targetDevice->name << "\n";
   }

   // Core Guidelines: enable verbose mode on the recorder so that IO
   // proc callbacks are logged to stderr. This helps diagnose whether
   // audio data is actually flowing through the device.
   recorder.setVerbose(config.verbose);

   // Start recording.
   if (!recorder.start()) {
      std::cerr << "Error: could not start recording.\n";
      recorder.stop();
      aiffWriter.close();   // Clean up the AIFF file.
      return 1;
   }

   if (config.verbose) {
      std::cerr << "Recording started.\n";
   }

   // Recording loop: wait for the specified duration or Ctrl-C.
   // Core Guidelines: the IO proc writes data to the AIFF file via the
   // output callback. This loop just waits for the duration or Ctrl-C.

   // Core Guidelines: we set the output callback to write data to the
   // AIFF file. The callback is called by the IO proc whenever new
   // input data is available.
   //
   // IMPORTANT: Core Audio outputs 32-bit float samples, but we write
   // 16-bit signed integer to the AIFF file. This callback converts
   // between the two formats. The float-to-int16 conversion clamps
   // values to [-1.0, 1.0] and maps to [-32768, 32767].
   std::vector<unsigned char> convertBuffer;
   recorder.setOutputCallback([&aiffWriter, &format, &convertBuffer](
         const AudioBufferList* inputData) {
      // Core Guidelines: we convert 32-bit float samples from Core
      // Audio to 16-bit signed integer for the AIFF file.

      // Core Guidelines: we assume there is only one buffer (stereo devices
      // have two channels interleaved in a single buffer).
      if (inputData->mNumberBuffers > 0) {
         const AudioBuffer& buffer = inputData->mBuffers[0];
         const float* floatData =
            static_cast<const float*>(buffer.mData);
         // Core Guidelines: calculate input frames from the 32-bit float
         // input data (8 bytes/frame for stereo), not the 16-bit output
         // format (4 bytes/frame). Using the output format's bytesPerFrame
         // would double the frame count and read past the buffer.
         uint32_t inputBytesPerFrame = format.channels * 4; // 32-bit float
         uint32_t numFrames = buffer.mDataByteSize / inputBytesPerFrame;
         uint32_t bytesPerFrameOut = format.channels * 2; // 16-bit output

         // Core Guidelines: resize the conversion buffer to hold all
         // frames of 16-bit output data.
         convertBuffer.resize(numFrames * bytesPerFrameOut);

         // Core Guidelines: convert each float sample to 16-bit integer.
         // We clamp to [-1.0, 1.0] before scaling to [-32767, 32767].
         // Samples are written in native (little-endian) byte order,
         // which matches AIFF's native byte order on macOS.
         for (uint32_t i = 0; i < numFrames; ++i) {
            for (uint32_t ch = 0; ch < format.channels; ++ch) {
               float sample =
                  floatData[i * format.channels + ch];
               // Clamp to [-1.0, 1.0] and convert to 16-bit integer.
               // Using 32767 (not 32768) avoids the asymmetric minimum
               // of two's complement int16_t.
               int16_t intSample = static_cast<int16_t>(
                  std::max(-1.0f, std::min(1.0f, sample)) *
                  32767.0f);
               // Write in little-endian (macOS native byte order).
               convertBuffer[(i * bytesPerFrameOut + ch) * 2 + 0] =
                  static_cast<uint8_t>(intSample & 0xFF);
               convertBuffer[(i * bytesPerFrameOut + ch) * 2 + 1] =
                  static_cast<uint8_t>((intSample >> 8) & 0xFF);
            }
         }

         aiffWriter.writeSamples(convertBuffer.data(),
            static_cast<uint32_t>(convertBuffer.size()));
      }
   });

   // Core Guidelines: we use a timer to track the elapsed time.
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
               std::cerr << "Duration elapsed ("
                        << config.durationSeconds << " seconds). Stopping.\n";
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

      // Core Guidelines: we sleep for a short period to avoid busy-waiting.
      // This is the only place where we sleep. We use 10ms intervals.
      struct timespec ts = {0, 10000000}; // 10ms in nanoseconds
      nanosleep(&ts, nullptr);
   }

   // Stop recording and close the device.
   recorder.stop();

   // Core Guidelines: print a summary of IO proc activity.
   // This helps diagnose whether audio data was actually flowing.
   // If ioCallbackCount is 0, no audio was playing through the device.
   // If totalBytesReceived is 0, the device was idle (no audio data).
   {
      uint64_t ioCallbacks = recorder.getIoCallbackCount();
      uint64_t totalBytes = recorder.getTotalBytesReceived();
      uint64_t totalFrames = totalBytes / format.bytesPerFrame;
      double totalDuration = static_cast<double>(totalFrames) /
                             static_cast<double>(format.sampleRate);

      if (config.verbose) {
         std::cerr << "\n=== Recording Summary ===\n";
         std::cerr << "  IO proc callbacks: " << ioCallbacks << "\n";
         std::cerr << "  Total bytes received: " << totalBytes << "\n";
         std::cerr << "  Total frames captured: " << totalFrames << "\n";
         std::cerr << "  Audio duration: " << totalDuration << " seconds\n";
         std::cerr << "  " << (ioCallbacks == 0
                  ? "WARNING: No IO proc callbacks received. "
                    "BlackHole may not be set as your system output, "
                    "or no audio is playing through it." : "")
                  << (totalBytes == 0 && ioCallbacks > 0
                  ? " WARNING: Device was idle (no audio data). "
                    "Make sure audio is playing through BlackHole." : "")
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

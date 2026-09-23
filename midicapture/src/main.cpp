/**
 * @file main.cpp
 * @brief Entry point for midicapture — audio-to-MIDI transcription.
 *
 * This is the single entry point of the program. It parses command-line
 * arguments using Boost program_options, opens the input audio file,
 * runs pitch detection and onset detection, builds a HIR Score, and
 * writes the result to a MIDI file.
 *
 * For the monophonic prototype:
 * 1. Read audio file metadata (sample rate, channels, duration).
 * 2. Run monophonic pitch detection (YINfft) frame by frame.
 * 3. Use onset detection (spectral flux) to find note boundaries.
 * 4. Build a simple Score with detected notes.
 * 5. Write the Score to a Type 1 MIDI file (480 ticks/qn).
 *
 * @section cli-interface Command-Line Interface
 *
 * Usage: midicapture [options] <input.aiff> <output.mid>
 *
 * Options:
 *   --window-size <int>  FFT window size (default: 2048).
 *   --hop-size <int>     Hop size (default: 512).
 *   --silence <float>    Silence threshold in dB (default: -40).
 *   --tempo <float>      Tempo in BPM (default: 120).
 *   --method <string>    Pitch detection method (default: "yinfft").
 *   --help               Print this message.
 *
 * @section monophonic-design Monophonic Prototype Design
 *
 * The monophonic prototype assumes only one note at a time:
 * - Pitch detection runs YINfft on each audio frame.
 * - Onset detection (spectral flux) marks note starts.
 * - When energy stays below the silence threshold for a few hops,
 *   the note ends (hysteresis).
 * - Notes are sorted by start time and written to the MIDI file.
 *
 * Polyphony (chords) is a future enhancement: it will use spectral
 * peak tracking + multiple pitch detection to resolve overlapping notes.
 *
 */

#include <boost/program_options.hpp>

#include <filesystem>
#include <iostream>
#include <libaudio/audioFile.h>
#include <libaudio/hir.h>
#include <libaudio/midiFileWriter.h>
#include <midicapture/transcriber.h>
#include <string>
#include <vector>

using namespace libaudio;

// ============================================================================
// printUsage — Print usage information to stderr.
//
// Domain context: This function is the fallback when Boost program_options
// parsing fails. It mirrors the --help output exactly, ensuring the user
// always gets consistent documentation.
// ============================================================================
static void printUsage(const char* programName) {
   // Usage messages go to stderr, not stdout.
   // The format is: programName [options] <input.aiff> <output.mid>
   //
   // Options:
   //   --window-size <int>  FFT window size (default: 2048).
   //   --hop-size <int>     Hop size (default: 512).
   //   --silence <float>    Silence threshold in dB (default: -40).
   //   --tempo <float>      Tempo in BPM (default: 120).
   //   --method <string>    Pitch detection method (default: "yinfft").
   //   --help               Print this message.

   std::cerr << "Usage: " << programName
             << " [options] <input.aiff> <output.mid>\n"
             << "\nOptions:\n";
   std::cerr << "  --help                    Print this message.\n";
   std::cerr
      << "  --window-size <int>       FFT window size (default: 2048).\n";
   std::cerr << "  --hop-size <int>          Hop size (default: 512).\n";
   std::cerr << "  --silence <float>         Silence threshold in dB (default: "
             << "-40).\n";
   std::cerr << "  --tempo <float>           Tempo in BPM (default: 120).\n";
   std::cerr << "  --method <string>         Pitch detection method (default: "
             << "\"yinfft\").\n";
   std::cerr << "  --generate-test-midi-files  Generate a set of simple"
             << " monophonic scale MIDI files.\n";
   std::cerr << "  --output-dir <string>     Directory for generated MIDI files"
             << " (default: .).\n";
   std::cerr << "\nExamples:\n";
   std::cerr << "  " << programName << " input.aiff output.mid\n";
   std::cerr << "  " << programName << " --window-size 1024 --silence -50\n";
   std::cerr << "     input.aiff output.mid\n";
   std::cerr << "  " << programName
             << " --method yinfast --tempo 144 input.aiff output.mid\n";
   std::cerr << "  " << programName << " --generate-test-midi-files\n";
   std::cerr << "  " << programName
             << " --generate-test-midi-files --output-dir ./test-midi\n";
}

// ============================================================================
// makeSanityScore — build the canonical single-note Score used by --test.
//
// Domain context: This is a fixed, input-independent note (middle C, velocity
// 100, one second). It gives a stable round-trip target for validating the
// MIDI writer and the midicsv/timidity toolchain without depending on the
// (work-in-progress) transcription pipeline.
// ============================================================================
static Score makeSanityScore(double tempoBpm) {
   Score score;
   score.tempo = tempoBpm;
   score.title = "midicapture sanity test";

   Note note;
   note.startTime = 0.0;
   note.endTime = 1.0; // one second
   note.pitch = 60;    // middle C (C4)
   note.velocity = 100;
   note.channel = 0; // channel 1 (Acoustic Grand Piano).
   note.sustain = false;
   score.notes.push_back(note);

   return score;
}

// ============================================================================
// buildScaleScore — build a monophonic piano Score from a sequence of pitches.
//
// Domain context: This is a small renderer helper shared by the
// --generate-test-midi-files mode. It lays a set of *monophonic* notes
// (one at a time) on the timeline with a fixed number of beats per note.
// Timing is tempo-relative: a beat is (60.0 / tempoBpm) real seconds, so the
// same pattern renders faster or slower purely by the Score's tempo. Every
// note uses a uniform velocity (no dynamics) and channel 0 (Acoustic Grand),
// with sustain off — the simplest possible performance, ideal as known ground
// truth for the audio→MIDI transcription path. A single beat of rest follows
// the final note.
//
// The writer (MidiFileWriter) converts these seconds to ticks using the Score's
// tempo, so callers only reason in beats — never in ticks.
//
// @param tempoBpm   Tempo in BPM (drives real-time duration per note).
// @param pitches    MIDI note numbers, in performance order (one at a time).
// @param noteBeats  Duration of each note, in beats (2 = whole, 1 = half,
//                   0.5 = quarter). Must be > 0.
// @param title      Score title (embedded in the MIDI file metadata).
// ============================================================================
static Score buildScaleScore(double tempoBpm, const std::vector<int>& pitches,
                             double noteBeats, const std::string& title) {
   Score score;
   score.tempo = tempoBpm;
   score.title = title;

   // One real second per beat at the given tempo (a beat is a quarter note).
   const double beatSeconds = 60.0 / tempoBpm;
   const double noteSeconds = noteBeats * beatSeconds;

   double cursor = 0.0;
   for (int pitch : pitches) {
      Note note;
      note.startTime = cursor;
      note.endTime = cursor + noteSeconds;
      note.pitch = static_cast<uint8_t>(pitch);
      note.velocity =
         100;           // uniform dynamics — the only signal is pitch + timing.
      note.channel = 0; // channel 1 (Acoustic Grand Piano).
      note.sustain = false;
      score.notes.push_back(note);
      cursor += noteSeconds; // monophonic: the next note starts when this ends.
   }

   return score;
}

// ============================================================================
// TestScale — one generated MIDI file: a filename, a pattern, a tempo, and
//            the duration (in beats) of each of its notes.
// ============================================================================
struct TestScale {
   std::string fileName;     // output filename (relative to --output-dir).
   std::vector<int> pitches; // MIDI note numbers, one at a time.
   double tempoBpm;          // drives real-time duration.
   double noteBeats; // beats per note (2 = whole, 1 = half, 0.5 = quarter).
};

// ============================================================================
// testScaleSet — the fixed set of performances written by
// --generate-test-midi-files.
//
// Domain context: A small, deliberately simple corpus of monophonic piano
// performances. It mixes (a) three note durations — whole, half, quarter —
// and (b) three tempos — 60, 90, 120 BPM — so the resulting audio exercises a
// spread of timing shapes the transcription pipeline must resolve. Patterns are
// the classic first-lesson repertoire: a C-major arpeggio (both directions),
// a one-octave chromatic scale, and an A-minor arpeggio. Durations land the
// pieces in the "around five seconds" ballpark (~5–8 s each) without forcing a
// hard five-second target.
// ============================================================================
static std::vector<TestScale> testScaleSet() {
   // C-major arpeggio: C4=60, E4=64, G4=67, C5=72.
   const std::vector<int> majorUp = {60, 64, 67, 72};
   const std::vector<int> majorDown = {72, 67, 64, 60};
   // A-minor arpeggio: A4=69, C5=72, E5=76, A5=81.
   const std::vector<int> minorUp = {69, 72, 76, 81};
   // One-octave chromatic run: C4=60 .. B4=71.
   std::vector<int> chromaticUp;
   for (int p = 60; p <= 71; ++p) {
      chromaticUp.push_back(p);
   }

   return {
      {"scale-major-ascending-whole-notes-60bpm.mid", majorUp, 60.0, 2.0},
      {"scale-major-ascending-half-notes-90bpm.mid", majorUp, 90.0, 1.0},
      {"scale-major-descending-whole-notes-60bpm.mid", majorDown, 60.0, 2.0},
      {"scale-major-descending-half-notes-90bpm.mid", majorDown, 90.0, 1.0},
      {"scale-chromatic-ascending-quarter-notes-120bpm.mid", chromaticUp, 120.0,
       0.5},
      {"scale-minor-ascending-whole-notes-60bpm.mid", minorUp, 60.0, 2.0},
   };
}

// ============================================================================
// runGenerateTestMidiFiles — generator-mode entry point.
//
// Domain context: Called from main when --generate-test-midi-files is present
// (it takes precedence over --test). It creates the output directory (if
// missing) and writes each performance in the fixed test set as a .mid file.
// Returns 0 on success, 1 if any file failed to write.
// ============================================================================
static int runGenerateTestMidiFiles(const std::string& outputDir) {
   namespace fs = std::filesystem;

   std::cout << "midicapture — generating test MIDI files\n";
   std::cout
      << "============================================================\n\n";
   std::cout << "Mode: GENERATE TEST MIDI FILES (all other options ignored)\n";
   std::cout << "Output directory: " << outputDir << "\n\n";

   // Create the output directory if it does not already exist (mkdir -p).
   std::error_code ec;
   fs::create_directories(outputDir, ec);
   if (ec) {
      std::cerr << "Error: could not create output directory \"" << outputDir
                << "\": " << ec.message() << "\n";
      return 1;
   }

   const std::vector<TestScale> scales = testScaleSet();
   int written = 0;
   int failed = 0;

   for (const TestScale& scale : scales) {
      const std::string fullPath =
         (fs::path(outputDir) / scale.fileName).string();

      Score score = buildScaleScore(scale.tempoBpm, scale.pitches,
                                    scale.noteBeats, scale.fileName);

      MidiFileWriter midiWriter(fullPath);
      if (midiWriter.write(score)) {
         std::cout << "  Wrote " << fullPath << "  ("
                   << midiWriter.bytesWritten() << " bytes, "
                   << scale.pitches.size() << " notes, " << scale.tempoBpm
                   << " BPM)\n";
         ++written;
      } else {
         std::cerr << "  Failed to write " << fullPath << "\n";
         ++failed;
      }
   }

   std::cout << "\nGenerated " << written << " of " << scales.size()
             << " test MIDI files in " << outputDir << "\n";
   if (failed == 0) {
      std::cout
         << "Render reference audio with: timidity -Ow <name>.wav <name>.mid\n";
      std::cout << "Then transcribe with:  midicapture <name>.mid\n";
      return 0;
   }
   std::cerr << "\nError: " << failed << " file(s) failed to write.\n";
   return 1;
}

// ============================================================================
// main — Entry point for midicapture.
//
// Domain context: The program flow is:
// 1. Parse command-line arguments using Boost program_options.
// 2. Open the input audio file and print its metadata.
// 3. Run pitch detection and onset detection hop by hop.
// 4. Build a HIR Score with detected notes.
// 5. Write the Score to a Type 1 MIDI file.
//
// Key design decisions:
// - Boost program_options is used for robust, extensible CLI parsing.
// - The monophonic prototype processes audio hop by hop.
// - Notes start on detected onsets; they end when energy stays below
//   the silence threshold for a few hops (hysteresis).
// - The output is a Type 1 MIDI file with 480 ticks per quarter note.
//
// @return 0 on success, 1 on error.
// ============================================================================
int main(int argc, char* argv[]) {
   // =====================================================================
   // Parse command-line arguments using Boost program_options.
   //
   // Domain context: Boost program_options provides robust, extensible
   // command-line parsing. It handles:
   // - Short and long option names (--window-size, -w)
   // - Type coercion (int, float, string)
   // - Default values
   // - Error messages for invalid arguments
   // - Help text generation
   //
   // Why Boost? It is the standard C++ option parsing library, well-tested,
   // cross-platform, and integrates cleanly with our CMake build.
   // =====================================================================

   std::string inputPath, outputPath;
   uint32_t windowSize = 2048;
   uint32_t hopSize = 512;
   float silenceDb = -40.0f;
   double tempoBpm = 120.0;
   std::string pitchMethod = "yinfft";
   bool testMode = false;
   bool generateTestMidiFiles = false;
   std::string outputDir = ".";

   // Define the options: name, type, description.
   namespace po = boost::program_options;
   // Build a header that puts the usage line between the project name
   // and the option list.  Boost appends ":" after the header string,
   // so we end with a newline to absorb it.
   // Boost's operator<< appends ":\n" after the header text, then prints
   // the option list.  We end the header with a newline so the ":" that
   // Boost appends replaces the blank line separator.
   std::string header = "midicapture — audio-to-MIDI transcription\n\nUsage: " +
                        std::string(argv[0]) +
                        " [options] <input.aiff> [output.mid]";
   po::options_description desc(header);
   desc.add_options()("help,h", "Print usage information.")(
      "input,i", po::value<std::string>(&inputPath),
      "Input audio file path (AIFF, WAV, FLAC, etc.).")(
      "output,o", po::value<std::string>(&outputPath),
      "Output MIDI filename (.mid).  When omitted, the input"
      " filename is reused with a .mid extension.")(
      "window-size", po::value<uint32_t>(&windowSize)->default_value(2048),
      "FFT window size (power of 2, default: 2048).")(
      "hop-size", po::value<uint32_t>(&hopSize)->default_value(512),
      "Hop size between frames (default: 512).")(
      "silence", po::value<float>(&silenceDb)->default_value(-40.0f),
      "Silence threshold in dB (default: -40).")(
      "tempo", po::value<double>(&tempoBpm)->default_value(120.0),
      "Tempo in BPM (default: 120).")(
      "method", po::value<std::string>(&pitchMethod)->default_value("yinfft"),
      "Pitch detection method (default: \"yinfft\").")(
      "test,t",
      "Sanity test: write a single middle-C note (C4, velocity 100, 1s)"
      " regardless of the input audio contents (no analysis is run).")(
      "generate-test-midi-files",
      "Generate a set of simple monophonic scale MIDI files in"
      " --output-dir. All other options are ignored; no input is needed.")(
      "output-dir", po::value<std::string>(&outputDir)->default_value("."),
      "Directory the generated MIDI files are written to"
      " (default: current directory).");

   // Define positional options: <input.aiff> <output.mid>.
   po::positional_options_description positional;
   positional.add("input", 1).add("output", 1);

   // Parse the command line with both named and positional options.
   po::variables_map vm;
   try {
      po::store(po::command_line_parser(argc, argv)
                   .options(desc)
                   .positional(positional)
                   .run(),
                vm);
      po::notify(vm);
   } catch (const po::error& e) {
      std::cerr << "Error: " << e.what() << "\n\n";
      printUsage(argv[0]);
      return 1;
   }

   testMode = (vm.count("test") > 0);
   generateTestMidiFiles = (vm.count("generate-test-midi-files") > 0);

   // Print a POSIX-style help message.
   if (vm.count("help")) {
      std::cout
         << "midicapture — audio-to-MIDI transcription\n\n"
         << "Usage: " << argv[0] << " [options] <input.aiff> [output.mid]\n"
         << "\nMain options:\n"
         << "  -h [ --help ]             Print usage information.\n"
         << "  -i [ --input ] arg        Input audio file path (AIFF, WAV, "
            "FLAC, "
            "etc.).\n"
         << "  -o [ --output ] arg       Output MIDI filename (.mid)."
            "  When omitted, the input filename is reused.\n"
         << "  --window-size arg (=2048) FFT window size (power of 2, default: "
            "2048).\n"
         << "  --hop-size arg (=512)     Hop size between frames (default: "
            "512).\n"
         << "  --silence arg (=-40)      Silence threshold in dB (default: "
            "-40).\n"
         << "  --tempo arg (=120)        Tempo in BPM (default: 120).\n"
         << "  --method arg (=yinfft)    Pitch detection method (default: "
            "\"yinfft\").\n"
         << "  -t [ --test ]             Sanity test: write a single middle-C"
            " note (C4, velocity 100,\n"
         << "                            1s) regardless of the input audio."
            " No analysis is run.\n"
         << "  --generate-test-midi-files"
         << "                            Generate a set of simple monophonic"
            " scale MIDI files.\n"
         << "  --output-dir arg (=. )  Directory the generated MIDI files go"
            " to\n"
         << "                            (default: current directory).\n"
         << "\n";
      return 0;
   }

   // ------------------------------------------------------------------
   // Generator mode: --generate-test-midi-files.
   //
   // Domain context: This mode is a pure renderer. It ignores the input audio,
   // the output filename, and every analysis option, and instead writes a
   // fixed set of simple monophonic scale performances into --output-dir. It
   // takes precedence over --test (they are both generator modes; the file
   // set is strictly more useful than the single sanity note).
   // ------------------------------------------------------------------
   if (generateTestMidiFiles) {
      return runGenerateTestMidiFiles(outputDir);
   }

   // Validate arguments: input is required unless in sanity-test mode.
   if (inputPath.empty() && !testMode) {
      std::cerr << "Error: an input audio file is required.\n\n";
      printUsage(argv[0]);
      return 1;
   }
   if (outputPath.empty()) {
      if (inputPath.empty()) {
         // Sanity-test mode with no input file: default output name.
         outputPath = "midicapture-test.mid";
      } else {
         // Derive output path from input: strip directory, replace
         // extension with .mid.  This ensures the output goes into the
         // current working directory (e.g., song.aiff → song.mid).
         auto slash = inputPath.rfind('/');
         std::string baseName = (slash != std::string::npos)
                                   ? inputPath.substr(slash + 1)
                                   : inputPath;
         auto dot = baseName.rfind('.');
         if (dot != std::string::npos) {
            outputPath = baseName.substr(0, dot) + ".mid";
         } else {
            outputPath = baseName + ".mid";
         }
      }
   }

   // Validate window size (must be a power of 2).
   if (windowSize == 0 || (windowSize & (windowSize - 1)) != 0) {
      std::cerr << "Error: window-size must be a power of 2.\n";
      return 1;
   }

   // Validate hop size (must be less than or equal to window size).
   if (hopSize == 0 || hopSize > windowSize) {
      std::cerr << "Error: hop-size must be between 1 and window-size.\n";
      return 1;
   }

   // =====================================================================
   // Open the input audio file and print its metadata.
   //
   // Domain context: AudioFileReader (from libaudio) opens the audio
   // file via libsndfile. This supports AIFF, WAV, FLAC, OGG, and
   // many other formats. We print the file metadata (sample rate,
   // channels, duration) to stdout for user feedback.
   // =====================================================================

   std::cout
      << "midicapture — audio-to-MIDI transcription (monophonic prototype)\n";
   std::cout
      << "============================================================\n\n";

   // ---- Sanity-test mode: write a known single note, skip all analysis. ----
   // The output is identical for every input, so it doubles as a stable
   // round-trip target for validating the MIDI writer + midicsv/timidity.
   if (testMode) {
      std::cout << "Mode: SANITY TEST (input audio contents ignored)\n\n";
      Score score = makeSanityScore(tempoBpm);
      std::cout << "  Note: middle C (C4, MIDI note 60), velocity 100,"
                   " 1.0 second.\n";
      std::cout << "\nWriting MIDI file: " << outputPath << "\n";

      MidiFileWriter midiWriter(outputPath);
      if (midiWriter.write(score)) {
         std::cout << "Wrote " << midiWriter.bytesWritten() << " bytes.\n";
         std::cout << "Validate: midicsv " << outputPath
                   << "   (or: timidity -Ow " << outputPath << ")\n";
         std::cout << "Done.\n";
      } else {
         std::cerr << "Error: Failed to write MIDI file.\n";
         return 1;
      }
      return 0;
   }

   try {
      AudioFileReader audioReader(inputPath);

      std::cout << "Input file: " << inputPath << "\n";
      std::cout << "  Sample rate: " << audioReader.sampleRate() << " Hz\n";
      std::cout << "  Channels: " << audioReader.channels() << "\n";
      std::cout << "  Total frames: " << audioReader.totalFrames() << "\n";
      double duration = audioReader.duration();
      std::cout << "  Duration: " << duration << " seconds\n";
      std::cout << "  Format: " << audioReader.formatName() << "\n\n";

      std::cout << "Configuration:\n";
      std::cout << "  Window size: " << windowSize << "\n";
      std::cout << "  Hop size: " << hopSize << "\n";
      std::cout << "  Silence threshold: " << silenceDb << " dB\n";
      std::cout << "  Tempo: " << tempoBpm << " BPM\n";
      std::cout << "  Pitch method: " << pitchMethod << "\n\n";

      // =====================================================================
      // Transcribe the audio file to MIDI.
      //
      // Domain context: The Transcriber orchestrates the full transcription
      // pipeline:
      // 1. Read audio hops from the file.
      // 2. Run pitch detection (YINfft) on each hop.
      // 3. Run onset detection (spectral flux) on each hop.
      // 4. Build a HIR Score with detected notes.
      // 5. Write the Score to a Type 1 MIDI file.
      // =====================================================================

      Transcriber transcriber(windowSize, hopSize, silenceDb, pitchMethod);

      // Transcribe the audio file.
      Score score = transcriber.transcribe(inputPath);

      std::cout << "Detected " << score.notes.size() << " notes.\n\n";

      // Print detected notes.
      for (const auto& note : score.notes) {
         // Convert MIDI note number to note name.
         const char* noteNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                    "F#", "G",  "G#", "A",  "A#", "B"};
         int octave = (note.pitch / 12) - 1;
         const char* noteName =
            noteNames[note.pitch % 12]; // Note name within octave.

         std::cout << "  " << noteName << octave << "  "
                   << static_cast<int>(note.pitch) << "  " << note.startTime
                   << "s → " << note.endTime << "s  "
                   << static_cast<int>(note.velocity) << "\n";
      }

      std::cout << "\nWriting MIDI file: " << outputPath << "\n";

      // Write the Score to a MIDI file.
      MidiFileWriter midiWriter(outputPath);
      bool success = midiWriter.write(score);

      if (success) {
         std::cout << "Wrote " << midiWriter.bytesWritten() << " bytes.\n";
         std::cout << "Done.\n";
      } else {
         std::cerr << "Error: Failed to write MIDI file.\n";
         return 1;
      }

   } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
   }

   return 0;
}

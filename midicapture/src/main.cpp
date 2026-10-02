/**
 * @file main.cpp
 * @brief Entry point for midicapture — audio-to-MIDI transcription.
 *
 * This is the single entry point of the program. It parses command-line
 * arguments using Boost program_options, opens the input audio file, runs the
 * selected transcription engine (an `libaudio::Analyzer`), and writes the
 * resulting HIR Score to a Type 1 MIDI file.
 *
 * Two engines are selectable via `--model`:
 *   - "basic" / "basic-pitch" (synonyms) — the Tier-2 polyphonic neural engine
 *     (`BasicPitch`, ONNX + Core ML). The *default* in a Tier-2 build.
 *   - "aubio" — the Tier-1 monophonic engine (`Transcriber`, YINfft + onset).
 * A Tier-1 build has only "aubio"; `--model` is absent there.
 *
 * Direct transcription path:
 * 1. Read audio file metadata (sample rate, channels, duration).
 * 2. Select the engine by `--model` and run it on the file.
 * 3. The engine returns a HIR Score of detected notes.
 * 4. Write the Score to a Type 1 MIDI file (480 ticks/qn).
 *
 * @section cli-interface Command-Line Interface
 *
 * Usage: midicapture [options] <input.aiff> <output.mid>
 *
 * Options:
 *   --model <string>     Transcription model (Tier-2): "basic" (default, the
 *                        polyphonic neural engine) or "aubio" (monophonic).
 *   --window-size <int>  FFT window size (default: 2048). [aubio only]
 *   --hop-size <int>     Hop size (default: 512). [aubio only]
 *   --silence <float>    Silence threshold in dB (default: -40). [aubio only]
 *   --tempo <float>      Tempo in BPM (default: 120).
 *   --method <string>    Pitch detection method (default: "yinfft"). [aubio
 * only]
 *   --ffmpeg <string>    Path to ffmpeg (aubio container-decode fallback +
 * corpus MP3 encoding; not used by basic-pitch).
 *   --help               Print this message.
 *
 * @section engine-design Engine Design
 *
 * The "aubio" engine is monophonic: it tracks one note at a time (YINfft pitch
 * per frame, spectral-flux onsets, an energy-hysteresis note state machine,
 * and defragmentation). The "basic" engine is polyphonic and models the
 * harmonic series, so it resolves the octave the monophonic YIN path cannot.
 * Both emit the same HIR Score, which the shared `MidiFileWriter` turns into a
 * Type 1 MIDI file.
 *
 */

#include <boost/program_options.hpp>

#include <filesystem>
#include <iostream>
#include <libaudio/analyzer.h>
#include <libaudio/audioFile.h>
#include <libaudio/hir.h>
#include <libaudio/midiFileWriter.h>
#include <libaudio/transcriber.h>
#include <memory>
#ifdef LIBAUDIO_HAS_TIER2
#include <libaudio/basicPitch.h>
#include <libaudio/rawMap.h>
#endif // LIBAUDIO_HAS_TIER2
#include <midicapture/corpusHarness.h>
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
#ifdef LIBAUDIO_HAS_TIER2
   // A polyphonic (basic) and a monophonic (aubio) engine coexist in a Tier-2
   // build, so the aubio-specific DSP knobs are tagged "[aubio only]". A
   // Tier-1 build has only aubio, so no tag (and no --model) is shown.
   static const char* aubioOnly = "   [aubio only]";
   std::cerr << "  --model <string>          Transcription model: \"basic\" "
             << "(default, the polyphonic basic-pitch engine) or \"aubio\" "
             << "(the monophonic Tier-1 engine).\n";
#else
   static const char* aubioOnly = "";
#endif
   std::cerr << "  --window-size <int>       FFT window size (default: 2048)"
             << aubioOnly << ".\n";
   std::cerr << "  --hop-size <int>          Hop size (default: 512)"
             << aubioOnly << ".\n";
   std::cerr << "  --silence <float>         Silence threshold in dB (default:"
             << " -40)" << aubioOnly << ".\n";
   std::cerr << "  --tempo <float>           Tempo in BPM (default: 120).\n";
   std::cerr << "  --method <string>         Pitch detection method (default:"
             << " \"yinfft\")" << aubioOnly << ".\n";
   std::cerr << "  --generate-test-midi-files  Generate a set of simple"
             << " monophonic scale MIDI files.\n";
   std::cerr << "  --output-dir <string>     Directory for generated MIDI files"
             << " (default: .).\n";
#ifdef LIBAUDIO_HAS_TIER2
   std::cerr << "  --run-corpus DIR          Run the 14-file corpus evaluation"
             << " in DIR with --model.\n";
   std::cerr
      << "  --analyzer <string>       Deprecated: use --model instead.\n";
   std::cerr
      << "  --clean                   Regenerate the corpus assets before"
      << " evaluating.\n";
   std::cerr << "  --ffmpeg <string>         Path to the ffmpeg executable"
             << " (aubio container-decode fallback + corpus MP3 encoding)."
             << " Not used by basic-pitch (in-process decode). Default:"
             << " /opt/homebrew/bin/ffmpeg.\n";
   std::cerr << "  --no-pitch-bends          For --model basic: skip pitch-bend"
             << " extraction (default: on).\n";
   std::cerr << "  --multiple-pitch-bends    For --model basic: route each"
             << " distinct bent pitch to its own channel (default: off).\n";
   std::cerr << "  --onset-threshold <f>     For --model basic: min onset"
             << " activation, 0..1 (default: 0.5).\n";
   std::cerr << "  --frame-threshold <f>     For --model basic: min frame"
             << " activation, 0..1 (default: 0.3).\n";
   std::cerr << "  --min-note-len <ms>       For --model basic: min note"
             << " length (default: 127.7 ms).\n";
   std::cerr << "  --min-freq <hz>           For --model basic: lowest Hz to"
             << " keep (default: 27.5, A0).\n";
   std::cerr << "  --max-freq <hz>           For --model basic: highest Hz to"
             << " keep (default: 4186, C8).\n";
   std::cerr << "  --velocity-scale <int>    For --model basic: velocity"
             << " = clamp(round(scale*amp), 1, 127) (default: 127).\n";
   std::cerr << "  --bend-deadband <bins>    For --model basic: near-flat"
             << " bend floor (default: 1.0).\n";
   std::cerr << "  --no-infer-onsets         For --model basic: skip"
             << " onset inference (default: on).\n";
   std::cerr << "  --no-melodia              For --model basic: skip the"
             << " melodia trick (default: on).\n";
   std::cerr << "  --dump-raw-map <path>     For --model basic: run the model"
             << " once and write its raw stitched maps to <path>; writes no"
             << " MIDI.\n";
#endif // LIBAUDIO_HAS_TIER2
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
                             double noteBeats, const std::string& title,
                             const std::vector<int>& velocities = {},
                             double gapBeats = 0.0) {
   Score score;
   score.tempo = tempoBpm;
   score.title = title;

   // One real second per beat at the given tempo (a beat is a quarter note).
   const double beatSeconds = 60.0 / tempoBpm;
   const double noteSeconds = noteBeats * beatSeconds;
   const double gapSeconds = gapBeats * beatSeconds; // silence after each note.

   // Per-note velocity ladder: use the provided value when there is one for
   // this note index, otherwise fall back to a uniform 100. This is how a
   // dynamics test (soft → loud) is expressed without changing pitch or timing.
   const bool haveVelocities = velocities.size() == pitches.size();

   double cursor = 0.0;
   for (size_t i = 0; i < pitches.size(); ++i) {
      Note note;
      note.startTime = cursor;
      note.endTime = cursor + noteSeconds;
      note.pitch = static_cast<uint8_t>(pitches[i]);
      note.velocity =
         static_cast<uint8_t>(haveVelocities ? velocities[i] : 100);
      note.channel = 0; // channel 1 (Acoustic Grand Piano).
      note.sustain = false;
      score.notes.push_back(note);
      // Monophonic: the next note starts one gap after this one ends.
      cursor += noteSeconds + gapSeconds;
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
   // Optional per-note velocity ladder (one value per pitch). When it is the
   // right size, the i-th note gets velocities[i]; otherwise every note uses
   // the uniform default (100). This lets a test exercise *dynamics* (velocity)
   // rather than just pitch + timing.
   std::vector<int> velocities;
   // Silence between successive notes, in beats (0 = legato, notes touch).
   // A non-zero gap gives a test the explicit rest between two notes that the
   // same-pitch merge must *not* swallow.
   double gapBeats = 0.0;
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

   // One-octave runs at three heights: separates an *octave* error from a
   // *chroma* (pitch-class) error. A transcription that is "one octave low"
   // here is a different, diagnosable failure than one with a wrong note.
   const std::vector<int> majorLow = {48, 52, 55, 60};  // C3..C4
   const std::vector<int> majorMid = {60, 64, 67, 72};  // C4..C5 (== majorUp)
   const std::vector<int> majorHigh = {72, 76, 79, 84}; // C5..C6

   // A sustained note, one beat of rest, then another — exercises the
   // same-pitch merge's *boundary*: the gap equals a whole rest at this
   // tempo, so the two must NOT be merged (they are distinct physical notes
   // separated by silence, not wobble fragments of one note).
   const std::vector<int> restSeparated = {60, 60};
   // Same note, no rest: the run is a wobble-fragmented sustained note that
   // the same-pitch merge must recombine into a single long note.
   const std::vector<int> sustainedRun = {60, 60, 60, 60};

   return {
      // Scales: pitch + timing + defragmentation.
      {"scale-major-ascending-whole-notes-60bpm.mid",
       majorUp,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-ascending-half-notes-90bpm.mid",
       majorUp,
       90.0,
       1.0,
       {},
       0.0},
      {"scale-major-descending-whole-notes-60bpm.mid",
       majorDown,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-descending-half-notes-90bpm.mid",
       majorDown,
       90.0,
       1.0,
       {},
       0.0},
      {"scale-chromatic-ascending-quarter-notes-120bpm.mid",
       chromaticUp,
       120.0,
       0.5,
       {},
       0.0},
      {"scale-minor-ascending-whole-notes-60bpm.mid",
       minorUp,
       60.0,
       2.0,
       {},
       0.0},
      // Octave: the same major arpeggio at three heights.
      {"scale-major-low-octave-whole-notes-60bpm.mid",
       majorLow,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-mid-octave-whole-notes-60bpm.mid",
       majorMid,
       60.0,
       2.0,
       {},
       0.0},
      {"scale-major-high-octave-whole-notes-60bpm.mid",
       majorHigh,
       60.0,
       2.0,
       {},
       0.0},
      // Timing: the same scale at two tempos (slow + fast).
      {"scale-major-ascending-whole-notes-30bpm.mid",
       majorUp,
       30.0,
       2.0,
       {},
       0.0},
      {"scale-major-ascending-whole-notes-180bpm.mid",
       majorUp,
       180.0,
       2.0,
       {},
       0.0},
      // Dynamics: a soft → loud velocity ladder on one pitch. The only
      // varying signal is loudness, so this is the suite's velocity test.
      {"velocity-soft-loud-quarter-notes-60bpm.mid",
       std::vector<int>{60, 60, 60, 60, 60, 60}, 60.0, 0.5,
       std::vector<int>{30, 50, 70, 90, 110, 127}, 0.0},
      // Defrag / merge: a wobble run that must collapse, and a rest-separated
      // run that must NOT merge across the gap.
      {"sustained-run-whole-notes-60bpm.mid", sustainedRun, 60.0, 2.0, {}, 0.0},
      {"rest-separated-whole-notes-60bpm.mid",
       restSeparated,
       60.0,
       2.0,
       {},
       1.0}, // one-beat rest between the two notes.
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

      Score score =
         buildScaleScore(scale.tempoBpm, scale.pitches, scale.noteBeats,
                         scale.fileName, scale.velocities, scale.gapBeats);

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

#ifdef LIBAUDIO_HAS_TIER2
   // Engine selection (--model) + the analyzer-agnostic corpus harness. These
   // are parsed and used only in a Tier-2 build (BasicPitch + the harness
   // exist only then).
   //
   // `modelName` is the canonical choice: "basic" by default (= the polyphonic
   // basic-pitch engine); "aubio" selects the Tier-1 monophonic engine. The
   // deprecated `--analyzer` alias binds to its OWN variable (`analyzerArg`,
   // no default) so it can never clobber `modelName`'s default during Boost's
   // notify(); it is folded into `modelName` after parsing instead.
   std::string runCorpusDir;
   std::string dumpRawMapPath;
   std::string modelName = "basic";
   std::string analyzerArg;
   bool clean = false;
   std::string ffmpegPath = "/opt/homebrew/bin/ffmpeg";
   bool noPitchBends = false;
   bool multiplePitchBends = false;
   // basic-pitch note-creation knobs (the post-processing policy). Each
   // carries the no-op default so a run with none of them stays
   // byte-identical; `makeAnalyzer` maps them onto `BasicPitchOptions` and
   // `setOptions` clamps each to its sane range.
   float onsetThreshold = 0.5f;
   float frameThreshold = 0.3f;
   double minNoteLenMs = 127.7;
   int velocityScale = 127;
   double minFrequency = 27.5;
   double maxFrequency = 4186.0;
   double bendDeadband = 1.0;
   bool noInferOnsets = false;
   bool noMelodia = false;
#endif // LIBAUDIO_HAS_TIER2

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
   //
   // The input and output paths are each registered under *two distinct*
   // option names that map into the same backing variable:
   //   - "input" / "output" — positional-only, no flag spellings.  These are
   //     the names bound in `positional` below, so the usage line can document
   //     them and they appear in the option list in `desc`.
   //   - "_input_alias,i" / "_output_alias,o" — flag-only (the user-facing
   //     --input/-i and --output/-o), sharing the same backing variable.
   //
   // Boost program_options cannot express "an option that is both a named
   // flag and a positional" for a *single* name: a value supplied both via
   // the flag and via the positional slot would be a second use of the same
   // logical option and would fail with
   //   Error: option '--input' cannot be specified more than once
   // whenever the documented form `midicapture --input in.aiff out.mid` is
   // used (the trailing positional `out.mid` would be bound to the same
   // "input" slot that the flag already filled).  Splitting each path into a
   // positional-only name plus a flag-only alias keeps both invocation styles
   // working without colliding.
   //
   // Residual limitation: a single invocation cannot specify the *same* file
   // path via both a flag and a positional (e.g. `--input in.aiff in.aiff`)
   // because the two registered names map into the same backing variable —
   // that's a usage error the user should fix, not a parsing collision.  The
   // alias options are registered *after* the positional names in `desc` so
   // the user-facing --input / --output rows appear in the help output under
   // their long/short names.
   desc.add_options()("help,h", "Print usage information.")(
      "input", po::value<std::string>(&inputPath),
      "Input audio file path (AIFF, WAV, FLAC, etc.).")(
      "output", po::value<std::string>(&outputPath),
      "Output MIDI filename (.mid).  When omitted, the input"
      " filename is reused with a .mid extension.")(
      "_input_alias,i", po::value<std::string>(&inputPath),
      "Input audio file path (AIFF, WAV, FLAC, etc.).")(
      "_output_alias,o", po::value<std::string>(&outputPath),
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

#ifdef LIBAUDIO_HAS_TIER2
   // Engine selection (--model) + the analyzer-agnostic corpus evaluator.
   // Registered only in a Tier-2 build (BasicPitch + the harness exist only
   // then). --model accepts "basic" / "basic-pitch" (synonyms, both select the
   // polyphonic BasicPitch engine; "basic" is the default) or "aubio" (the
   // Tier-1 monophonic engine). --analyzer is a deprecated alias for --model,
   // bound to its own variable so it never clobbers the --model default.
   desc.add_options()(
      "model", po::value<std::string>(&modelName)->default_value("basic"),
      "Transcription model: \"basic\" (default, the polyphonic"
      " basic-pitch engine) or \"aubio\" (the monophonic"
      " Tier-1 engine).")("analyzer", po::value<std::string>(&analyzerArg),
                          "Deprecated alias for --model. Use --model instead.")(
      "run-corpus", po::value<std::string>(&runCorpusDir),
      "Run the 14-file corpus evaluation in DIR with --model (no positional"
      " input needed).")(
      "clean", po::bool_switch(&clean),
      "Before evaluating, regenerate the corpus assets (.mid + .mp3).")(
      "ffmpeg",
      po::value<std::string>(&ffmpegPath)
         ->default_value("/opt/homebrew/bin/ffmpeg"),
      "Path to the ffmpeg executable (aubio container-decode fallback and"
      " corpus MP3 encoding). Not used by basic-pitch (in-process decode).")(
      "no-pitch-bends", po::bool_switch(&noPitchBends),
      "For --model basic: skip pitch-bend extraction (the default is on,"
      " matching the Python reference).")(
      "multiple-pitch-bends", po::bool_switch(&multiplePitchBends),
      "For --model basic: route each distinct bent pitch to its own MIDI"
      " channel (the reference default is off — one channel).")(
      "onset-threshold", po::value<float>(&onsetThreshold)->default_value(0.5f),
      "For --model basic: min onset activation, 0..1 (default: 0.5).")(
      "frame-threshold", po::value<float>(&frameThreshold)->default_value(0.3f),
      "For --model basic: min frame activation, 0..1 (default: 0.3).")(
      "min-note-len", po::value<double>(&minNoteLenMs)->default_value(127.7),
      "For --model basic: min note length in ms (default: 127.7; below this,"
      " a note is merged into its predecessor).")(
      "min-freq", po::value<double>(&minFrequency)->default_value(27.5),
      "For --model basic: lowest frequency (Hz) to keep (default: 27.5, A0).")(
      "max-freq", po::value<double>(&maxFrequency)->default_value(4186.0),
      "For --model basic: highest frequency (Hz) to keep (default: 4186, C8).")(
      "velocity-scale", po::value<int>(&velocityScale)->default_value(127),
      "For --model basic: velocity = clamp(round(scale*amplitude), 1, 127)"
      " (default: 127).")(
      "bend-deadband", po::value<double>(&bendDeadband)->default_value(1.0),
      "For --model basic: near-flat pitch-bend floor in bins (default: 1.0;"
      " a note that never moves by this much is not bent).")(
      "no-infer-onsets", po::bool_switch(&noInferOnsets),
      "For --model basic: skip the onset-inference decode step (default: on).")(
      "no-melodia", po::bool_switch(&noMelodia),
      "For --model basic: skip the melodia-trick decode step (default: on).")(
      "dump-raw-map", po::value<std::string>(&dumpRawMapPath),
      "For --model basic: run the model once and write its raw stitched"
      " activation maps (note/onset/contour) to PATH as a binary raw-map"
      " file; writes no MIDI. Requires a positional input audio file.");
#endif // LIBAUDIO_HAS_TIER2

   // Define positional options: <input.aiff> <output.mid>.  These bind the
   // alias-free names registered above ("input" / "output"), so the
   // positional slots never collide with the flag aliases
   // ("_input_alias" / "_output_alias").
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

   // --- Post-parse validation ---
   //
   // Boost program_options treats `--input`/`-i` (the alias) and the first
   // positional slot ("input") as *distinct* options even though they share a
   // backing variable.  A value supplied via both (e.g. `--input in.aiff`
   // plus a trailing `in.aiff`) is two uses of the same logical file path;
   // the flag would win in the shared variable and the positional's value
   // would be silently dropped.  Catch the user mistake explicitly with an
   // actionable message instead of letting the last write win.
   //
   // Boost's own duplicate detection fires *before* we get here when the
   // same option name is supplied twice in a single invocation (e.g.
   // `--input in.aiff --input in.aiff`), so this only catches the
   // cross-spelling case where the flag and the positional slot are
   // *different* registered names that happen to share a backing variable
   // (e.g. `--input in.aiff` followed by a trailing positional `in.aiff`).
   if (vm.count("_input_alias") > 0 && vm.count("input") > 0) {
      std::cerr << "Error: --input and a positional <input.aiff> cannot both"
                << " be specified.  Use one form or the other.\n";
      std::cerr << "  Example (positional): " << argv[0]
                << " in.aiff [out.mid]\n";
      std::cerr << "  Example (flag-only):  " << argv[0]
                << " --input in.aiff   # no trailing arguments\n";
      return 1;
   }
   // Same for output: `--output`/`-o` plus a trailing positional.
   if (vm.count("_output_alias") > 0 && vm.count("output") > 0) {
      std::cerr << "Error: --output and a positional [output.mid] cannot both"
                << " be specified.  Use one form or the other.\n";
      std::cerr << "  Example (positional): " << argv[0]
                << " in.aiff out.mid\n";
      std::cerr << "  Example (flag-only):  " << argv[0]
                << " in.aiff --output out.mid\n";
      return 1;
   }

   testMode = (vm.count("test") > 0);
   generateTestMidiFiles = (vm.count("generate-test-midi-files") > 0);

#ifdef LIBAUDIO_HAS_TIER2
   // Fold the deprecated --analyzer alias into --model. `analyzerArg` carries
   // no default of its own, so `vm.count("analyzer") > 0` reliably means "the
   // user actually passed --analyzer" (a registered default would be > 0 even
   // when the flag was absent). The alias wins over the --model default only
   // when the user supplies it; an empty `--analyzer` value would otherwise
   // clobber a good --model value, so we only override on a non-empty arg.
   if (vm.count("analyzer") > 0 && !analyzerArg.empty()) {
      std::cerr << "Warning: --analyzer is deprecated; use --model.\n";
      modelName = analyzerArg;
   }
#endif // LIBAUDIO_HAS_TIER2

   // Print a POSIX-style help message.
   if (vm.count("help")) {
#ifdef LIBAUDIO_HAS_TIER2
      // A polyphonic (basic) + monophonic (aubio) engine coexist, so tag the
      // aubio-only DSP knobs; a Tier-1 build has only aubio (no tag, no
      // --model).
      static const char* aubioOnly = "  [aubio only]";
#else
      static const char* aubioOnly = "";
#endif
      std::cout
         << "midicapture — audio-to-MIDI transcription\n\n"
         << "Usage: " << argv[0] << " [options] <input.aiff> [output.mid]\n"
         << "\nMain options:\n"
         << "  -h [ --help ]             Print usage information.\n"
         << "  <input.aiff>              Input audio file path (AIFF, WAV, "
            "FLAC, "
            "etc.).\n"
         << "  [output.mid]             Output MIDI filename (.mid).  When "
            "omitted, the input filename is reused.\n"
#ifdef LIBAUDIO_HAS_TIER2
         << "  --model arg (=basic)      Transcription model: \"basic\" "
            "(default, the polyphonic\n"
         << "                            basic-pitch engine) or \"aubio\" "
            "(the monophonic Tier-1\n"
         << "                            engine).\n"
#endif
         << "  --window-size arg (=2048) FFT window size (power of 2, default: "
            "2048)"
         << aubioOnly << ".\n"
         << "  --hop-size arg (=512)     Hop size between frames (default: "
            "512)"
         << aubioOnly << ".\n"
         << "  --silence arg (=-40)      Silence threshold in dB (default: "
            "-40)"
         << aubioOnly << ".\n"
         << "  --tempo arg (=120)        Tempo in BPM (default: 120).\n"
         << "  --method arg (=yinfft)    Pitch detection method (default: "
            "\"yinfft\")"
         << aubioOnly << ".\n"
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
#ifdef LIBAUDIO_HAS_TIER2
         << "  --run-corpus DIR           Run the 14-file corpus evaluation in"
            " DIR with --model.\n"
         << "  --analyzer arg             Deprecated: use --model instead.\n"
         << "  --clean                    Regenerate the corpus assets before"
            " evaluating.\n"
         << "  --ffmpeg arg (=/opt/homebrew/bin/ffmpeg)\n"
         << "                            Path to the ffmpeg executable (aubio"
            " container-decode fallback + corpus MP3 encoding). Not used by"
            " basic-pitch (in-process decode).\n"
         << "  --no-pitch-bends           For --model basic: skip pitch-bend"
            " extraction (default: on).\n"
         << "  --multiple-pitch-bends     For --model basic: route each"
            " distinct bent pitch to its own channel.\n"
         << "  --onset-threshold <f>      For --model basic: min onset"
            " activation, 0..1 (default: 0.5).\n"
         << "  --frame-threshold <f>      For --model basic: min frame"
            " activation, 0..1 (default: 0.3).\n"
         << "  --min-note-len <ms>        For --model basic: min note length"
            " (default: 127.7 ms).\n"
         << "  --min-freq <hz>            For --model basic: lowest Hz to"
            " keep (default: 27.5, A0).\n"
         << "  --max-freq <hz>            For --model basic: highest Hz to"
            " keep (default: 4186, C8).\n"
         << "  --velocity-scale <int>     For --model basic: velocity ="
            " clamp(round(scale*amp), 1, 127) (default: 127).\n"
         << "  --bend-deadband <bins>     For --model basic: near-flat"
            " bend floor (default: 1.0).\n"
         << "  --no-infer-onsets          For --model basic: skip onset"
            " inference (default: on).\n"
         << "  --no-melodia               For --model basic: skip the"
            " melodia trick (default: on).\n"
         << "  --dump-raw-map <path>      For --model basic: run the model"
            " once and write its raw stitched maps to <path>; writes no"
            " MIDI.\n"
#endif // LIBAUDIO_HAS_TIER2
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

#ifdef LIBAUDIO_HAS_TIER2
   // --run-corpus: the analyzer-agnostic 14-file corpus evaluator. Like the
   // generator mode above it needs no positional input and ignores the
   // analysis options, so it runs before the input-required check below.
   if (vm.count("run-corpus") > 0) {
      return runCorpus(runCorpusDir, modelName, clean, ffmpegPath,
                       !noPitchBends, multiplePitchBends);
   }
#endif // LIBAUDIO_HAS_TIER2

   // Validate arguments: input is required unless in sanity-test mode.
   if (inputPath.empty() && !testMode) {
      std::cerr << "Error: an input audio file is required.\n\n";
      printUsage(argv[0]);
      return 1;
   }

#ifdef LIBAUDIO_HAS_TIER2
   // --dump-raw-map PATH: run the basic-pitch model once over the input and
   // write its three stitched activation maps to PATH (a small binary raw-map
   // file). Standalone: it writes no MIDI. The positional input was validated
   // above; an output .mid, if given, is ignored in this mode.
   if (vm.count("dump-raw-map") > 0) {
      if (dumpRawMapPath.empty()) {
         std::cerr << "Error: --dump-raw-map requires a PATH (the raw-map"
                   << " output file).\n";
         return 1;
      }
      BasicPitch bp;
      const RawPredictions raw = bp.getRawPredictions(inputPath);
      writeRawPredictions(raw, dumpRawMapPath);
      auto fmtShape = [](const Tensor& t) {
         std::string s = "(";
         for (size_t i = 0; i < t.dims.size(); ++i) {
            if (i > 0) {
               s += ", ";
            }
            s += std::to_string(t.dims[i]);
         }
         return s + ")";
      };
      std::cout << "midicapture — raw-map dump (basic-pitch)\n";
      std::cout
         << "============================================================\n\n";
      std::cout << "Input:      " << inputPath << "\n";
      std::cout << "Raw map:    " << dumpRawMapPath << "\n";
      std::cout << "  noteMap       " << fmtShape(raw.noteMap) << "\n";
      std::cout << "  onsetMap      " << fmtShape(raw.onsetMap) << "\n";
      std::cout << "  contourMap    " << fmtShape(raw.contourMap) << "\n";
      std::cout << "  annotNFrames  " << raw.annotNFrames
                << " (untrimmed per-window)\n";
      std::cout << "  haveContour   " << (raw.haveContour ? "yes" : "no")
                << "\n";
      return 0;
   }
#endif // LIBAUDIO_HAS_TIER2

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

   std::cout << "midicapture — audio-to-MIDI transcription\n";
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
      // Best-effort header: libsndfile cannot open every container (e.g. an
      // mp4). If the probe fails we cannot print the sample rate and friends,
      // but that is not fatal — each engine decodes the file itself (basic:
      // in-process via the FFmpeg libraries; aubio: the ffmpeg binary
      // fallback), so the transcription below still runs.
      try {
         AudioFileReader audioReader(inputPath);

         std::cout << "Input file: " << inputPath << "\n";
         std::cout << "  Sample rate: " << audioReader.sampleRate() << " Hz\n";
         std::cout << "  Channels: " << audioReader.channels() << "\n";
         std::cout << "  Total frames: " << audioReader.totalFrames() << "\n";
         double duration = audioReader.duration();
         std::cout << "  Duration: " << duration << " seconds\n";
         std::cout << "  Format: " << audioReader.formatName() << "\n\n";
      } catch (const std::exception& e) {
         std::cerr << "Input file: " << inputPath << "\n"
                   << "  Note: libsndfile cannot open this container ("
                   << e.what() << "); the engine will decode it via FFmpeg.\n"
                   << "\n";
      }

      // =====================================================================
      // Select the engine and print a model-aware configuration block.
      //
      // Domain context: a Tier-2 build offers two engines behind the Analyzer
      // port. The aubio-only DSP knobs (window / hop / silence / method) tune
      // only the monophonic aubio engine; the basic-pitch engine's window and
      // frame rate are fixed by the model, so those knobs are inert there. A
      // Tier-1 build has only the aubio engine (and no --model flag). The
      // shared makeAnalyzer factory (corpusHarness.h) is the single source of
      // truth for the model-name -> engine mapping, used here and by the
      // corpus harness.
      // =====================================================================
      std::unique_ptr<libaudio::Analyzer> analyzer;
#ifdef LIBAUDIO_HAS_TIER2
      AnalyzerParams params;
      params.windowSize = windowSize;
      params.hopSize = hopSize;
      params.silenceDb = silenceDb;
      params.pitchMethod = pitchMethod;
      params.tempoBpm = tempoBpm;
      params.ffmpegPath = ffmpegPath;
      params.includePitchBends = !noPitchBends;
      params.multiplePitchBends = multiplePitchBends;
      // The basic-pitch note-creation knobs. Each CLI var holds its no-op
      // default when the flag is absent, so a no-flag run maps onto the
      // reference and stays byte-identical. `midiTempo` is intentionally not
      // flagged here: --tempo (tempoBpm) sets the output tempo via the
      // `score.tempo` override below, and the corpus uses the struct default.
      params.onsetThreshold = onsetThreshold;
      params.frameThreshold = frameThreshold;
      params.minNoteLenMs = minNoteLenMs;
      params.velocityScale = velocityScale;
      params.minFrequency = minFrequency;
      params.maxFrequency = maxFrequency;
      params.bendDeadbandBins = bendDeadband;
      params.inferOnsets = !noInferOnsets;
      params.melodiaTrick = !noMelodia;
      analyzer = makeAnalyzer(modelName, params);
#endif

      std::cout << "Configuration:\n";
#ifdef LIBAUDIO_HAS_TIER2
      if (modelName == "aubio") {
         std::cout << "  Engine: aubio (Tier-1 monophonic)\n";
         std::cout << "  Window size: " << windowSize << "\n";
         std::cout << "  Hop size: " << hopSize << "\n";
         std::cout << "  Silence threshold: " << silenceDb << " dB\n";
         std::cout << "  Pitch method: " << pitchMethod << "\n";
         // The basic-pitch note-creation knobs are inert for aubio; if the
         // user passed any of them with the aubio model, say so rather than
         // failing silently (gate on !defaulted() so a no-flag run is quiet).
         if ((vm.count("onset-threshold") > 0 &&
              !vm["onset-threshold"].defaulted()) ||
             (vm.count("frame-threshold") > 0 &&
              !vm["frame-threshold"].defaulted()) ||
             (vm.count("min-note-len") > 0 &&
              !vm["min-note-len"].defaulted()) ||
             (vm.count("min-freq") > 0 && !vm["min-freq"].defaulted()) ||
             (vm.count("max-freq") > 0 && !vm["max-freq"].defaulted()) ||
             (vm.count("velocity-scale") > 0 &&
              !vm["velocity-scale"].defaulted()) ||
             (vm.count("bend-deadband") > 0 &&
              !vm["bend-deadband"].defaulted()) ||
             vm.count("no-infer-onsets") > 0 || vm.count("no-melodia") > 0) {
            std::cerr
               << "  Note: the basic-pitch note-creation knobs (--onset-"
               << "threshold, --frame-threshold, --min-note-len, --min-freq,"
               << " --max-freq, --velocity-scale, --bend-deadband,"
               << " --no-infer-onsets, --no-melodia) are ignored by the \""
               << modelName << "\" model.\n";
         }
      } else {
         auto* bp = dynamic_cast<libaudio::BasicPitch*>(analyzer.get());
         std::cout << "  Engine: " << analyzer->name()
                   << " (Tier-2 polyphonic)\n";
         std::cout << "  Core ML: "
                   << (bp && bp->coreMlActive() ? "active" : "cpu-fallback")
                   << "\n";
         std::cout << "  Pitch bends: " << (!noPitchBends ? "on" : "off")
                   << (multiplePitchBends ? " (multi-channel)" : "") << "\n";
         // The aubio-only DSP knobs are inert for basic-pitch (its window and
         // frame rate are fixed by the model); if the user passed any of them
         // with the basic model, say so rather than failing silently. Boost
         // reports count() > 0 for a *defaulted* option, so gate each knob on
         // !defaulted() to avoid a notice on every basic run.
         if ((vm.count("window-size") > 0 && !vm["window-size"].defaulted()) ||
             (vm.count("hop-size") > 0 && !vm["hop-size"].defaulted()) ||
             (vm.count("silence") > 0 && !vm["silence"].defaulted()) ||
             (vm.count("method") > 0 && !vm["method"].defaulted())) {
            std::cerr << "  Note: --window-size / --hop-size / --silence /"
                      << " --method are aubio-only; ignored by the \""
                      << modelName << "\" model.\n";
         }
         // Only warn when the user actually passed the flag: boost reports
         // count() > 0 for a defaulted option, which would turn this into a
         // notice on every basic run.
         if (vm.count("ffmpeg") > 0 && !vm["ffmpeg"].defaulted()) {
            std::cerr << "  Note: --ffmpeg is no longer used by basic-pitch"
                      << " (decode is in-process).\n";
         }
      }
#else
      analyzer =
         std::make_unique<libaudio::Transcriber>(windowSize, hopSize, silenceDb,
                                                 pitchMethod, tempoBpm);
      std::cout << "  Engine: aubio (Tier-1 monophonic)\n";
      std::cout << "  Window size: " << windowSize << "\n";
      std::cout << "  Hop size: " << hopSize << "\n";
      std::cout << "  Silence threshold: " << silenceDb << " dB\n";
      std::cout << "  Pitch method: " << pitchMethod << "\n";
#endif
      std::cout << "  Tempo: " << tempoBpm << " BPM\n\n";

      // =====================================================================
      // Transcribe the audio file to MIDI with the selected engine.
      //
      // Domain context: `Analyzer::transcribe` is the analyzer-agnostic seam;
      // both engines read the file, detect notes, and return a HIR Score. The
      // caller then writes that Score to a Type 1 MIDI file.
      // =====================================================================

      // Transcribe the audio file.
      Score score = analyzer->transcribe(inputPath);

      // The HIR stores note times in seconds; the MIDI writer maps seconds to
      // ticks (and sets the tempo meta-event) via the Score's tempo.
      // basic-pitch fixes its own output tempo at 120 internally, so set it
      // here to honour --tempo uniformly across both engines (a no-op for
      // aubio, which already forwards it). This is a playback-rate control,
      // not a note-content change.
      score.tempo = tempoBpm;

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

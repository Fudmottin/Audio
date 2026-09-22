/**
 * @file main.cpp
 * @brief Entry point for waterfall — audio frequency analysis.
 *
 * waterfall reads an audio file and renders a frequency analysis of its
 * acoustic energy over time as a SONAR-style waterfall. This file is the
 * whole analysis tool: it parses command-line arguments, drives a forward
 * FFT per row via libaudio, quantizes the magnitude spectrum to 16-bit
 * integers on a log (grayscale-quantization) scale, and prints the result
 * as space-separated hex values.
 *
 * @section modes Analysis modes
 *
 * waterfall runs in one of two modes, selected by the `--pcm` flag (MIDI is
 * the default):
 *
 *  - **MIDI** (default): the column grid is **128 MIDI notes × bands-per-note
 *    columns**. Each column's center frequency is the true exponential (equal-
 *    tempered) frequency of its note, and each note's bands are distributed
 *    logarithmically across that note's frequency span. This is the musically
 *    faithful mapping: a 100 Hz source lights column 33, a 200 Hz source
 *    lights column 65, a 440 Hz source lights column 89, etc.
 *
 *  - **PCM**: the legacy behavior — a **linear** frequency sweep. The full
 *    Nyquist is divided into 128 equal *Hz* chunks ("notes"), each subdivided
 *    into bands of equal *Hz* width. This produces a visually interesting
 *    SONAR image but is *not* a faithful pitch mapping: a note is a fixed Hz
 *    slice, so the same 100 Hz source lights different columns depending on
 *    sample rate, and the mapping is sample-rate dependent.
 *
 * The active mode is reported in the output header (`mode=MIDI` or `mode=PCM`).
 * A consumer that does not find a `mode` key should assume PCM (the legacy
 * linear behavior), so older waterfall output remains interpretable.
 *
 * @section output Output format
 *
 * Line 1 is a single line of `name=value` pairs describing the grid,
 * including the frequency of every column, followed by a trailing newline.
 * Each subsequent line is one row (one time slice) of `numColumns` 16-bit
 * values as 4-hex-digit, space-separated integers.
 *
 * Example header (abridged):
 *   sampleRate=48000 ... mode=MIDI ... col0=8.175758 col1=8.661642 ...
 *
 * @section cli-interface Command-Line Interface
 *
 * Usage: waterfall [options] <input.aiff>
 *
 * Options:
 *   --window-size <int>   FFT window size, power of two (default: 2048).
 *   --time-slice <float>  Row duration in milliseconds (default: 10).
 *   --bands-per-note <int> Bands per MIDI note (default: 8).
 *   --ref-db <float>      Full-scale reference level (dB, default: 0).
 *   --pcm                 Use the legacy linear frequency mapping (default:
 *                         the musically faithful MIDI mapping).
 *   --help                Print this message.
 *
 * @section analysis Analysis
 *
 * For each row: read `hopSize` samples, forward-FFT them, map the
 * magnitude spectrum onto the column grid, convert each magnitude
 * to dB, then quantize dB to a 16-bit integer with a log mapping. The
 * column grid is (128 notes) x (bands-per-note) = numColumns. In MIDI mode
 * each column's center frequency is the note's true exponential frequency;
 * in PCM mode the columns are laid out linearly across the Nyquist so that
 * column c falls within note c / bandsPerNote. The hop is a fixed
 * power of two (aubio requirement); advancing by one hop per row means
 * consecutive rows overlap when the window is wider than the hop, and
 * each row still contains at least one sample.
 *
 */

#include <waterfall/audioFile.h>

#include <boost/program_options.hpp>
#include <libaudio/fft.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace po = boost::program_options;

// ============================================================================
// WaterfallMode — Selects how the column grid maps onto the frequency axis.
//
// Domain context: the column count is (128 notes) x (bands-per-note) in both
// modes, but the *placement* of each column's center frequency differs. MIDI
// is the faithful pitch mapping (exponential, sample-rate independent); PCM is
// the legacy linear sweep (equal Hz per column chunk, sample-rate dependent).
// ============================================================================
enum class WaterfallMode {
   Midi, ///< True exponential (equal-tempered) note-to-frequency mapping.
   Pcm   ///< Legacy linear frequency sweep (equal Hz per column chunk).
};

// ============================================================================
// midiNoteFrequency — Exponential (equal-tempered) MIDI note frequency.
//
// Domain context: MIDI note n has frequency 440 * 2^((n-69)/12) Hz, anchored
// at A4 = note 69 = 440 Hz. This is the physically correct pitch-to-frequency
// relationship; it is what makes a 100 Hz source land in column 33, a 200 Hz
// source in column 65, and a 440 Hz source in column 89.
//
// @param midiNote MIDI note number, 0..127.
// @return Frequency in Hz.
// ============================================================================
static float midiNoteFrequency(int midiNote) {
   return 440.0f * std::pow(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
}

// ============================================================================
// modeToHeaderValue — String form of a mode for the output header.
// ============================================================================
static const char* modeToHeaderValue(WaterfallMode mode) {
   return mode == WaterfallMode::Midi ? "MIDI" : "PCM";
}

// ============================================================================
// printUsage — Print usage information to stderr.
//
// Domain context: This function is the fallback when Boost program_options
// parsing fails. It mirrors the --help output exactly.
// ============================================================================
static void printUsage(const char* programName) {
   std::cerr << "Usage: " << programName
             << " [options] <input.aiff>\n";
   std::cerr << "\n<input.aiff> may be any format libsndfile supports\n"
             << "(AIFF, WAV, FLAC, OGG, etc.). Output is a single line of\n"
             << "name=value pairs (header) followed by one line per time\n"
             << "slice, each line containing numColumns space-separated\n"
             << "4-digit hex integers.\n";
   std::cerr << "\nOptions:\n";
   std::cerr << "  --help                     Print this message.\n";
   std::cerr << "  --window-size <int>        FFT window size, power of two"
             << " (default: 2048).\n";
   std::cerr << "  --time-slice <float>       Row duration in milliseconds"
             << " (default: 10).\n";
   std::cerr << "  --bands-per-note <int>     Bands per MIDI note"
             << " (default: 8).\n";
   std::cerr << "  --ref-db <float>           Full-scale reference level (dB,"
             << " default: 0).\n";
   std::cerr << "  --no-auto-scale            Use --ref-db verbatim (default"
             << " auto-scales to the file peak).\n";
   std::cerr << "  --pcm                      Use the legacy linear frequency"
             << " mapping (default: MIDI mapping).\n";
   std::cerr << "\nModes:\n";
   std::cerr << "  MIDI   (default) Columns use the true exponential (equal-"
             << "tempered)\n"
             << "           note-to-frequency mapping; a 100 Hz source lights"
             << " column\n"
             << "           33, 200 Hz lights 65, 440 Hz lights 89, etc.\n";
   std::cerr << "  PCM       (--pcm)   Legacy linear sweep: equal Hz per column"
             << " chunk across the\n"
             << "                   Nyquist. Visually interesting but not a"
             << " faithful pitch\n"
             << "                   mapping (sample-rate dependent).\n";
   std::cerr << "\nExamples:\n";
   std::cerr << "  " << programName << " recording.aiff\n";
   std::cerr << "  " << programName << " --time-slice 2.78 --window-size 4096"
             << " recording.aiff\n";
   std::cerr << "  " << programName << " --pcm recording.aiff > legacy.txt\n";
}

// ============================================================================
// quantizeTo16bit — Convert a magnitude to a 16-bit intensity on a log scale.
//
// Domain context: This is the grayscale quantization step. Acoustic energy
// spans a wide dynamic range, so we map linearly in dB (a logarithmic
// quantity) to the full 16-bit range. A reference level (refDb, 0 dB by
// default) marks full scale; a noise floor (kFloorDb, -60 dB) maps to 0.
//
// Note: kFloorDb is a *pragmatic display cut-off*, not a physical limit. The
// theoretical dynamic range of a 16-bit word is ~96 dB, but a FFT *bin
// magnitude* rarely spans anything like that in practice: a full-scale tone
// yields a peak bin of ~N/2 (~60 dB below a full-scale reference at a 2048-pt
// window), a window function adds ~6 dB more, and real signals spread their
// energy across many bins. So reachable bin-magnitude levels cluster in the
// ~-60..0 dB band, which is what this floor is tuned to. Values at or below
// the floor are treated as silence.
//
// @param magnitude  FFT magnitude (linear scale).
// @param refDb      Full-scale reference in dB.
// @return 16-bit intensity, 0..65535.
// ============================================================================
static uint16_t quantizeTo16bit(float magnitude, float refDb) {
   const float kFloorDb = -60.0f;

   if (magnitude <= 0.0f) {
      return 0;
   }

   const float magDb = 20.0f * std::log10(magnitude);
   // Normalize dB to [0, 1]: kFloorDb -> 0, refDb -> 1.
   const float normalized =
      (magDb - kFloorDb) / (refDb - kFloorDb);
   const float clamped = std::max(0.0f, std::min(1.0f, normalized));
   return static_cast<uint16_t>(std::lround(clamped * 65535.0f));
}

// ============================================================================
// mapSpectrumToColumns — Map an FFT magnitude spectrum onto the columns.
//
// Domain context: The FFT produces `numBins` bins covering 0..Nyquist. Each
// column has a center frequency and a (per-column) band width in Hz; we find
// the FFT bin(s) that fall within each column's band and take the maximum
// magnitude. A per-column width is used so that bands that are narrow in Hz
// (the high end in MIDI mode) are not widened to cover bins that belong to
// the next column — a uniform width would smear content across adjacent
// columns. This yields `numColumns` values where column i represents the
// energy near `columnFrequencies[i]`.
//
// @param magnitudes         FFT magnitude spectrum (length numBins).
// @param numBins            Number of FFT bins.
// @param columnFrequencies  Center frequency of each column (Hz).
// @param columnBandWidths   Per-column frequency band width (Hz).
// @param binWidthHz         Width of each FFT bin (Hz).
// @return Vector of one magnitude per column.
// ============================================================================
static std::vector<float> mapSpectrumToColumns(
   const std::vector<float>& magnitudes,
   uint32_t numBins,
   const std::vector<float>& columnFrequencies,
   const std::vector<float>& columnBandWidths,
   float binWidthHz) {
   const uint32_t numColumns = static_cast<uint32_t>(columnFrequencies.size());
   std::vector<float> columns(numColumns, 0.0f);

   if (numBins == 0 || binWidthHz <= 0.0f) {
      return columns;
   }

   for (uint32_t c = 0; c < numColumns; ++c) {
      const float center = columnFrequencies[c];
      const float half = columnBandWidths[c] / 2.0f;
      const float low = center - half;
      const float high = center + half;

      const uint32_t lowBin =
         static_cast<uint32_t>(std::max(0.0f, low) / binWidthHz);
      const uint32_t highBin = std::min(
         numBins - 1,
         static_cast<uint32_t>(std::max(0.0f, high) / binWidthHz));

      float maxMag = 0.0f;
      for (uint32_t b = lowBin; b <= highBin; ++b) {
         maxMag = std::max(maxMag, magnitudes[b]);
      }
      columns[c] = maxMag;
   }

   return columns;
}

// ============================================================================
// printHex — Write a 16-bit value as 4 hex digits to a stream.
// ============================================================================
static void printHex(std::ostream& os, uint16_t value) {
   os << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
      << static_cast<unsigned int>(value);
   // Reset so subsequent decimal output (header) is unaffected.
   os << std::dec;
}

// ============================================================================
// main — Parse arguments, analyze the audio, and print the waterfall.
// ============================================================================
int main(int argc, char* argv[]) {
   // --- Command-line options ---
   po::options_description desc("waterfall options");
   desc.add_options()
      ("help,h", "Print this message.")
      // Positional-only option: bound to a single unnamed file argument.
      // Declared with a value type but no short flag so it accepts exactly
      // one positional argument (the input audio file).
      ("input", po::value<std::string>(), "Input audio file path.")
      ("window-size",
       po::value<int>()->default_value(2048),
       "FFT window size, power of two.")
      ("time-slice",
       po::value<double>()->default_value(10.0),
       "Row duration in milliseconds.")
      ("bands-per-note",
       po::value<int>()->default_value(8),
       "Bands per MIDI note.")
      ("ref-db",
       po::value<float>()->default_value(0.0f),
       "Full-scale reference level (dB). Ignored when --auto-scale is on.")
      ("no-auto-scale",
       po::bool_switch()->default_value(false),
       "Use --ref-db verbatim instead of auto-scaling to the file peak.")
      ("pcm",
       po::bool_switch()->default_value(false),
       "Use the legacy linear frequency mapping (default: MIDI mapping).");

   po::positional_options_description positional;
   positional.add("input", 1);

   po::variables_map vm;
   try {
      po::store(po::command_line_parser(argc, argv).options(desc)
                   .positional(positional)
                   .run(),
                vm);
      if (vm.count("help")) {
         printUsage(argv[0]);
         return 0;
      }
      po::notify(vm);
   } catch (const po::error& e) {
      std::cerr << "Error: " << e.what() << "\n";
      printUsage(argv[0]);
      return 1;
   }

   if (!vm.count("input")) {
      std::cerr << "Error: no input file given.\n";
      printUsage(argv[0]);
      return 1;
   }

   // --- Resolve analysis parameters ---
   const int windowSizeArg = vm["window-size"].as<int>();
   const double timeSliceMs = vm["time-slice"].as<double>();
   const int bandsPerNoteArg = vm["bands-per-note"].as<int>();
   const float refDbArg = vm["ref-db"].as<float>();
   const bool autoScale = !vm["no-auto-scale"].as<bool>();

   // Analysis mode: MIDI (default) is the faithful pitch mapping; PCM is the
   // legacy linear sweep selected with --pcm.
   const WaterfallMode mode =
      vm["pcm"].as<bool>() ? WaterfallMode::Pcm : WaterfallMode::Midi;

   // windowSize must be a power of two (aubio requirement).
   if (windowSizeArg < 2 || (windowSizeArg & (windowSizeArg - 1)) != 0) {
      std::cerr << "Error: --window-size must be a power of two.\n";
      return 1;
   }
   if (bandsPerNoteArg < 1) {
      std::cerr << "Error: --bands-per-note must be >= 1.\n";
      return 1;
   }
   const uint32_t windowSize = static_cast<uint32_t>(windowSizeArg);
   const uint32_t bandsPerNote = static_cast<uint32_t>(bandsPerNoteArg);
   const uint32_t hopSize = windowSize; // one hop per row.

   // --- Open the input file ---
   AudioFile file(vm["input"].as<std::string>());
   const uint32_t sampleRate = file.sampleRate();
   const uint32_t totalFrames = file.totalFrames();

   if (sampleRate == 0 || totalFrames == 0) {
      std::cerr << "Error: could not read audio data from input file.\n";
      return 1;
   }

   // --- Set up the FFT ---
   FFT fft(windowSize);
   const uint32_t numBins = fft.numBins();
   // Width of each FFT bin in Hz (bins span 0..Nyquist).
   const float binWidthHz = static_cast<float>(sampleRate) / 2.0f /
                            static_cast<float>(numBins);

   // --- Derive the column grid (center frequencies + per-column band widths) ---
   // Both modes use (128 notes) x (bands-per-note) columns; only the placement
   // of each column's center frequency differs.
   const uint32_t kMidiNotes = 128;
   const uint32_t totalColumns = kMidiNotes * bandsPerNote; // e.g. 1024

   const float nyquist = static_cast<float>(sampleRate) / 2.0f;
   std::vector<float> columnFrequencies(totalColumns);
   std::vector<float> columnBandWidths(totalColumns);

   if (mode == WaterfallMode::Pcm) {
      // Legacy linear sweep: divide the full Nyquist into 128 equal Hz
      // chunks ("notes"), each subdivided into `bandsPerNote` equal-Hz bands.
      const float noteWidth = nyquist / static_cast<float>(kMidiNotes);
      const float bandWidthHz = noteWidth / static_cast<float>(bandsPerNote);

      for (uint32_t c = 0; c < totalColumns; ++c) {
         const float noteIndex = static_cast<float>(c / bandsPerNote);
         const float bandIndex = static_cast<float>(c % bandsPerNote);
         columnFrequencies[c] =
            (noteIndex + (bandIndex + 0.5f) / static_cast<float>(bandsPerNote)) *
            noteWidth;
         columnBandWidths[c] = bandWidthHz; // uniform width in the linear case.
      }
   } else {
      // MIDI: the true equal-tempered mapping. Each note n spans the frequency
      // interval [midiNoteFrequency(n), midiNoteFrequency(n+1)) (a 12th of an
      // octave), and its `bandsPerNote` bands are distributed logarithmically
      // across that interval. The column's band width is its fractional slice
      // of the note's interval, so adjacent bands stay adjacent without
      // overlapping. Note 127 has no upper neighbor in MIDI; its high edge is
      // the next (theoretical) note's frequency so the band stays a clean
      // 12th-of-an-octave slice.
      for (uint32_t c = 0; c < totalColumns; ++c) {
         const uint32_t noteIndex = c / bandsPerNote;
         const uint32_t bandIndex = c % bandsPerNote;
         const float lo = midiNoteFrequency(static_cast<int>(noteIndex));
         const float hi = midiNoteFrequency(static_cast<int>(noteIndex + 1));
         // Logarithmically even band centers across [lo, hi].
         const float loLog = std::log(lo);
         const float hiLog = std::log(hi);
         const float f =
            std::exp(loLog +
                     (static_cast<float>(bandIndex) + 0.5f) /
                     static_cast<float>(bandsPerNote) * (hiLog - loLog));
         columnFrequencies[c] = f;
         columnBandWidths[c] = (hi - lo) / static_cast<float>(bandsPerNote);
      }
   }

   // --- Allocate working buffers ---
   std::vector<float> window;
   window.resize(windowSize, 0.0f);

   // --- Auto-scale: find the file's peak magnitude (one full pass) ---
   // Domain context: with a fixed refDb (default 0 dB) a loud signal saturates
   // every column to FFFF. We instead auto-scale refDb to the loudest FFT bin
   // seen anywhere in the file, so the peak maps to FFFF and the full dynamic
   // range is preserved. The resolved value is reported in the header so
   // downstream scripts know the scale.
   float refDb = refDbArg;
   if (autoScale) {
      float peakMag = 0.0f;
      uint32_t scanPos = 0;
      while (scanPos < totalFrames) {
         file.readInto(hopSize, window.data());
         const auto [mag, ph] = fft.forward(window.data());
         (void)ph;
         for (float m : mag) {
            peakMag = std::max(peakMag, m);
         }
         scanPos += hopSize;
      }
      if (peakMag > 0.0f) {
         // Express the reference as the dB value of the peak bin magnitude.
         refDb = 20.0f * std::log10(peakMag);
      }
      file.reset(); // rewind for the analysis pass.
   }

   // --- Print the header line (name=value pairs) ---
   // A consumer that does not find a `mode` key should assume PCM (the legacy
   // linear behavior) so older waterfall output stays interpretable.
   {
      std::ostringstream header;
      header << std::fixed;

      header << "sampleRate=" << sampleRate << " ";
      header << "timeSliceMs=" << timeSliceMs << " ";
      header << "hopSize=" << hopSize << " ";
      header << "windowSize=" << windowSize << " ";
      header << "numBins=" << numBins << " ";
      header << "midiNotes=128 bandsPerNote=" << bandsPerNote << " ";
      header << "nyquistHz=" << nyquist << " ";
      header << "numColumns=" << totalColumns << " ";
      header << "mode=" << modeToHeaderValue(mode) << " ";
      header << "refDb=" << refDb << " ";
      header << "autoScale=" << (autoScale ? 1 : 0) << " ";

      // One center frequency per column.
      for (uint32_t c = 0; c < totalColumns; ++c) {
         header << " col" << c << "=" << columnFrequencies[c];
      }
      header << "\n";

      std::cout << header.str();
   }

   // Sample index of the next frame to read.
   uint32_t framePos = 0;

   // --- Process rows ---
   while (framePos < totalFrames) {
      // Read up to hopSize samples into the window, zero-padding at EOF.
      file.readInto(hopSize, window.data());

      // Forward FFT.
      const auto [magnitude, phase] = fft.forward(window.data());
      (void)phase; // Not needed for intensity.

      // Map the spectrum onto the column grid.
      const std::vector<float> columns = mapSpectrumToColumns(
         magnitude, numBins, columnFrequencies, columnBandWidths, binWidthHz);

      // Quantize and print one row.
      for (uint32_t c = 0; c < totalColumns; ++c) {
         printHex(std::cout, quantizeTo16bit(columns[c], refDb));
         if (c + 1 < totalColumns) {
            std::cout << ' ';
         }
      }
      std::cout << "\n";

      framePos += hopSize;
   }

   std::cout.flush();
   return 0;
}

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
 * @section output Output format
 *
 * Line 1 is a single line of `name=value` pairs describing the grid,
 * including the frequency of every column, followed by a trailing newline.
 * Each subsequent line is one row (one time slice) of `numColumns` 16-bit
 * values as 4-hex-digit, space-separated integers.
 *
 * Example header (abridged):
 *   sampleRate=48000 timeSliceMs=10.0 hopSize=2048 ...  col0=0 col1=23.4 ...
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
 *   --help                Print this message.
 *
 * @section analysis Analysis
 *
 * For each row: read `hopSize` samples, forward-FFT them, map the
 * magnitude spectrum onto the column grid, convert each magnitude
 * to dB, then quantize dB to a 16-bit integer with a log mapping. The
 * column grid is (128 MIDI notes) x (bands-per-note) = numColumns, with
 * each column's center frequency laid out linearly across the Nyquist so
 * that column c falls within note c / bandsPerNote. The hop is a fixed
 * power of two (aubio requirement); advancing by one hop per row means
 * consecutive rows overlap when the window is wider than the hop, and
 * each row still contains at least one sample.
 *
 */

#include <waterfall/audioFile.h>

#include <boost/program_options.hpp>
#include <libaudio/fft.h>

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
   std::cerr << "\nExamples:\n";
   std::cerr << "  " << programName << " recording.aiff\n";
   std::cerr << "  " << programName << " --time-slice 2.78 --window-size 4096"
             << " recording.aiff\n";
}

// ============================================================================
// quantizeTo16bit — Convert a magnitude to a 16-bit intensity on a log scale.
//
// Domain context: This is the grayscale quantization step. Acoustic energy
// spans a huge dynamic range, so we map linearly in dB (a logarithmic
// quantity) to the full 16-bit range. A reference level (refDb, 0 dB by
// default) marks full scale; the floor (-60 dB) maps to 0.
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
// Domain context: The FFT produces `numBins` bins covering 0..Nyquist. The
// user requests a specific frequency range (freqMin..freqMax) and a specific
// column count. We map each column to its center frequency, find the
// matching bin, and take the maximum magnitude over the bins that fall in
// the column's frequency band. This yields `numColumns` values where column
// i represents the energy near `columnFrequencies[i]`.
//
// @param magnitudes         FFT magnitude spectrum (length numBins).
// @param numBins            Number of FFT bins.
// @param columnFrequencies  Center frequency of each column (Hz).
// @param bandWidthHz        Width of each column's frequency band (Hz).
// @param binWidthHz         Width of each FFT bin (Hz).
// @return Vector of one magnitude per column.
// ============================================================================
static std::vector<float> mapSpectrumToColumns(
   const std::vector<float>& magnitudes,
   uint32_t numBins,
   const std::vector<float>& columnFrequencies,
   float bandWidthHz,
   float binWidthHz) {
   const uint32_t numColumns = static_cast<uint32_t>(columnFrequencies.size());
   std::vector<float> columns(numColumns, 0.0f);

   if (numBins == 0 || binWidthHz <= 0.0f) {
      return columns;
   }

   for (uint32_t c = 0; c < numColumns; ++c) {
      const float center = columnFrequencies[c];
      const float low = center - bandWidthHz / 2.0f;
      const float high = center + bandWidthHz / 2.0f;

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
       "Use --ref-db verbatim instead of auto-scaling to the file peak.");

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

   // --- Derive column frequencies from the note/band scheme ---
   // The README defines the columns as (128 MIDI notes) x (8 bands), so we
   // generate the column grid from that scheme rather than a plain linear
   // sweep: each note spans `bandsPerNote` contiguous bands across the full
   // Nyquist. Column c therefore falls within note (c / bandsPerNote) and
   // within that note's band (c % bandsPerNote).
   const uint32_t kMidiNotes = 128;
   const uint32_t totalColumns = kMidiNotes * bandsPerNote; // e.g. 1024

   const float nyquist = static_cast<float>(sampleRate) / 2.0f;
   const float noteWidth = nyquist / static_cast<float>(kMidiNotes);
   const float bandWidthHz = noteWidth / static_cast<float>(bandsPerNote);

   std::vector<float> columnFrequencies(totalColumns);
   for (uint32_t c = 0; c < totalColumns; ++c) {
      const float noteIndex = static_cast<float>(c / bandsPerNote);
      const float bandIndex = static_cast<float>(c % bandsPerNote);
      columnFrequencies[c] =
         (noteIndex + (bandIndex + 0.5f) / static_cast<float>(bandsPerNote)) *
         noteWidth;
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
      header << "numColumns=" << totalColumns;
      header << " bandWidthHz=" << bandWidthHz << " ";
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
         magnitude, numBins, columnFrequencies, bandWidthHz, binWidthHz);

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

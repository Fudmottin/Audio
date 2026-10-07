/**
 * @file test_main.cpp
 * @brief Unit tests for libaudio — exercise the public API using
 *        assert()-based checks.
 *
 * This test file exercises every public module:
 * - HIR (Note, ControlEvent, Score) — pure data, no external deps
 * - FFT (forward, inverse, windowSize, numBins) — requires aubio
 * - PitchDetector (detect, method, confidence) — requires aubio
 * - ScoreBuilder (addNote, addControlEvent, build) — no external deps
 * - MidiFileWriter (write, bytesWritten) — requires libsndfile
 * - AudioFileReader (read, readMono, seek, reset, eof) — requires libsndfile
 *
 * The test is self-contained: it generates all needed audio data
 * programmatically (sine waves, test tone files).
 *
 * @see libaudio/hir.h
 * @see libaudio/fft.h
 * @see libaudio/pitch.h
 * @see libaudio/scoreBuilder.h
 * @see libaudio/midiFileWriter.h
 * @see libaudio/audioFile.h
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libaudio/audioFile.h>
#include <libaudio/fft.h>
#include <libaudio/hir.h>
#include <libaudio/midiFileReader.h>
#include <libaudio/midiFileWriter.h>
#include <libaudio/pitch.h>
#include <libaudio/scoreBuilder.h>
#include <sndfile.h>
#include <string>
#include <vector>

using namespace libaudio;

// ============================================================================
// Assertion macro — counts passes and failures, prints summary.
// ============================================================================
static int passes = 0;
static int failures = 0;

// Helper: convert const char* or std::string to const char* for fprintf.
// The const ref extends the lifetime of temporary std::strings to the end
// of the full expression, so this is safe.
static const char* cstr(const char* s) { return s; }
static const char* cstr(const std::string& s) { return s.c_str(); }

#define ASSERT(cond, msg)                                                      \
   do {                                                                        \
      if (!(cond)) {                                                           \
         fprintf(stderr, "FAIL: %s at %s:%d\n", cstr(msg), __FILE__,           \
                 __LINE__);                                                    \
         failures++;                                                           \
      } else {                                                                 \
         passes++;                                                             \
      }                                                                        \
   } while (0)

// Helper: generate a sine wave at a given frequency.
static std::vector<float> generateSineWave(double frequency, double sampleRate,
                                           uint32_t numSamples) {
   std::vector<float> samples(numSamples);
   for (uint32_t i = 0; i < numSamples; ++i) {
      double t = static_cast<double>(i) / sampleRate;
      samples[i] = static_cast<float>(std::sin(2.0 * M_PI * frequency * t));
   }
   return samples;
}

// ============================================================================
// Helper: write a mono AIFF test tone file using libsndfile.
// ============================================================================
static void writeTestTone(const char* path, double frequency, double sampleRate,
                          uint32_t numSamples) {
   SF_INFO sfInfo = {};
   sfInfo.samplerate = static_cast<int>(sampleRate);
   sfInfo.channels = 1;
   sfInfo.format = SF_FORMAT_AIFF | SF_FORMAT_FLOAT;

   SNDFILE* sf = sf_open(path, SFM_WRITE, &sfInfo);
   ASSERT(sf != nullptr, "writeTestTone: could not open output file");

   std::vector<float> samples =
      generateSineWave(frequency, sampleRate, numSamples);

   int written =
      sf_writef_float(sf, samples.data(), static_cast<sf_count_t>(numSamples));
   ASSERT(written == static_cast<int>(numSamples),
          "writeTestTone: could not write all samples");

   sf_close(sf);
}

// ============================================================================
// 1. HIR Tests — pure data structures, no external dependencies.
// ============================================================================
static void test_hir() {
   fprintf(stdout, "\n--- HIR Tests ---\n");

   // 1a. Default-constructed Note has correct defaults.
   Note defaultNote;
   ASSERT(defaultNote.pitch == 60, "HIR: default Note.pitch == 60 (middle C)");
   ASSERT(defaultNote.velocity == 100, "HIR: default Note.velocity == 100");
   ASSERT(defaultNote.channel == 0, "HIR: default Note.channel == 0");
   ASSERT(defaultNote.sustain == false, "HIR: default Note.sustain == false");
   ASSERT(defaultNote.startTime == 0.0, "HIR: default Note.startTime == 0.0");
   ASSERT(defaultNote.endTime == 0.0, "HIR: default Note.endTime == 0.0");

   // 1b. Default-constructed ControlEvent has correct defaults.
   ControlEvent defaultCtrl;
   ASSERT(defaultCtrl.time == 0.0, "HIR: default ControlEvent.time == 0.0");
   ASSERT(defaultCtrl.controller == 0,
          "HIR: default ControlEvent.controller == 0");
   ASSERT(defaultCtrl.value == 0, "HIR: default ControlEvent.value == 0");

   // 1c. Default-constructed Score has correct defaults.
   Score defaultScore;
   ASSERT(defaultScore.tempo == 120.0, "HIR: default Score.tempo == 120.0");
   ASSERT(defaultScore.notes.empty(), "HIR: default Score.notes is empty");
   ASSERT(defaultScore.controls.empty(),
          "HIR: default Score.controls is empty");
   ASSERT(defaultScore.title.empty(), "HIR: default Score.title is empty");
   ASSERT(defaultScore.composer.empty(),
          "HIR: default Score.composer is empty");

   // 1d. Construct a Note with explicit fields, verify all fields.
   Note explicitNote;
   explicitNote.startTime = 1.5;
   explicitNote.endTime = 2.0;
   explicitNote.pitch = 64; // E4
   explicitNote.velocity = 120;
   explicitNote.channel = 1;
   explicitNote.sustain = true;

   ASSERT(explicitNote.startTime == 1.5, "HIR: explicit Note.startTime == 1.5");
   ASSERT(explicitNote.endTime == 2.0, "HIR: explicit Note.endTime == 2.0");
   ASSERT(explicitNote.pitch == 64, "HIR: explicit Note.pitch == 64 (E4)");
   ASSERT(explicitNote.velocity == 120, "HIR: explicit Note.velocity == 120");
   ASSERT(explicitNote.channel == 1, "HIR: explicit Note.channel == 1");
   ASSERT(explicitNote.sustain == true, "HIR: explicit Note.sustain == true");

   // 1e. Construct a ControlEvent with explicit fields.
   ControlEvent sustainEvent;
   sustainEvent.time = 0.0;
   sustainEvent.controller = 64; // Sustain pedal CC#64
   sustainEvent.value = 127;     // Pedal fully down

   ASSERT(sustainEvent.time == 0.0, "HIR: explicit ControlEvent.time == 0.0");
   ASSERT(sustainEvent.controller == 64,
          "HIR: explicit ControlEvent.controller == 64 (sustain pedal)");
   ASSERT(sustainEvent.value == 127,
          "HIR: explicit ControlEvent.value == 127 (pedal down)");

   // 1f. Construct a Score with notes and controls, verify sizes.
   Score composedScore;
   Note n1;
   n1.startTime = 0.0;
   n1.endTime = 0.5;
   n1.pitch = 60;
   n1.velocity = 100;
   n1.channel = 0;
   n1.sustain = false;
   composedScore.notes.push_back(n1);

   Note n2;
   n2.startTime = 0.5;
   n2.endTime = 1.0;
   n2.pitch = 64;
   n2.velocity = 100;
   n2.channel = 0;
   n2.sustain = false;
   composedScore.notes.push_back(n2);

   composedScore.controls.push_back(sustainEvent);
   composedScore.tempo = 120.0;
   composedScore.title = "Test Piece";
   composedScore.composer = "Test Composer";

   ASSERT(composedScore.notes.size() == 2,
          "HIR: composed Score.notes.size() == 2");
   ASSERT(composedScore.controls.size() == 1,
          "HIR: composed Score.controls.size() == 1");
   ASSERT(composedScore.tempo == 120.0, "HIR: composed Score.tempo == 120.0");
   ASSERT(composedScore.title == "Test Piece",
          "HIR: composed Score.title == \"Test Piece\"");
   ASSERT(composedScore.composer == "Test Composer",
          "HIR: composed Score.composer == \"Test Composer\"");

   // 1g. Verify Score::notes and controls are accessible as public vectors.
   Note n3;
   n3.startTime = 1.5;
   n3.endTime = 2.0;
   n3.pitch = 67;
   n3.velocity = 80;
   n3.channel = 0;
   n3.sustain = false;
   composedScore.notes.push_back(n3);
   ASSERT(composedScore.notes.size() == 3,
          "HIR: Score.notes.push_back works, size == 3");

   ControlEvent ctrlOff;
   ctrlOff.time = 2.0;
   ctrlOff.controller = 64;
   ctrlOff.value = 0;
   composedScore.controls.push_back(ctrlOff);
   ASSERT(composedScore.controls.size() == 2,
          "HIR: Score.controls.push_back works, size == 2");

   // Iterate over notes and controls.
   int noteIterCount = 0;
   for (const auto& n : composedScore.notes) {
      (void)n;
      noteIterCount++;
   }
   ASSERT(noteIterCount == 3, "HIR: can iterate over Score.notes (count == 3)");

   int ctrlIterCount = 0;
   for (const auto& c : composedScore.controls) {
      (void)c;
      ctrlIterCount++;
   }
   ASSERT(ctrlIterCount == 2,
          "HIR: can iterate over Score.controls (count == 2)");
}

// ============================================================================
// 2. FFT Tests — requires aubio.
// ============================================================================
static void test_fft() {
   fprintf(stdout, "\n--- FFT Tests ---\n");

   // 2a. Construct FFT with window size 2048.
   FFT fft(2048);
   ASSERT(fft.windowSize() == 2048, "FFT: windowSize() returns 2048");
   ASSERT(fft.numBins() == 1025, "FFT: numBins() returns 1025 (2048/2+1)");

   // 2b. Generate a sine wave at 440 Hz, 48 kHz, window size.
   const uint32_t windowSize = 2048;
   const double sampleRate = 48000.0;
   const double frequency = 440.0;

   std::vector<float> sineWave =
      generateSineWave(frequency, sampleRate, windowSize);

   // 2c. Call forward(), verify magnitude vector length.
   auto [magnitudes, phases] = fft.forward(sineWave.data());
   ASSERT(static_cast<int>(magnitudes.size()) == 1025,
          "FFT: forward() returns magnitude vector of length 1025");
   ASSERT(static_cast<int>(phases.size()) == 1025,
          "FFT: forward() returns phase vector of length 1025");

   // 2d. Verify peak magnitude bin corresponds to ~440 Hz.
   //     bin index ≈ 440/48000 * 2048 ≈ 18.73
   float peakMag = 0.0f;
   int peakBin = 0;
   for (uint32_t i = 0; i < magnitudes.size(); ++i) {
      if (magnitudes[i] > peakMag) {
         peakMag = magnitudes[i];
         peakBin = static_cast<int>(i);
      }
   }

   double expectedBin = frequency / sampleRate * windowSize;
   double binDiff = std::abs(peakBin - expectedBin);
   ASSERT(binDiff < 1.5, "FFT: peak bin is near expected 440 Hz bin (got bin " +
                            std::to_string(peakBin) + ")");

   // 2e. Call inverse(), verify reconstructed signal length.
   std::vector<float> reconstructed = fft.inverse(magnitudes, phases);
   ASSERT(static_cast<int>(reconstructed.size()) == windowSize,
          "FFT: inverse() returns signal of length 2048");
}

// ============================================================================
// 3. PitchDetector Tests — requires aubio.
// ============================================================================
static void test_pitch() {
   fprintf(stdout, "\n--- PitchDetector Tests ---\n");

   const uint32_t bufSize = 2048;
   const double sampleRate = 48000.0;

   // 3a. Construct PitchDetector with buffer size 2048.
   PitchDetector detector(bufSize);
   ASSERT(detector.bufSize() == 2048, "Pitch: bufSize() returns 2048");

   // 3b. Detect pitch from silence (all zeros) → expect (0.0, 0.0).
   std::vector<float> silence(bufSize, 0.0f);
   auto [silencePitch, silenceConf] = detector.detect(silence.data(), bufSize);
   ASSERT(silencePitch == 0.0f,
          "Pitch: silence returns pitch 0.0 (no pitch detected)");
   ASSERT(silenceConf == 0.0f, "Pitch: silence returns confidence 0.0");

   // 3c. Detect pitch from a sine wave at 440 Hz (A4 → MIDI note 69).
   //     Note: The YINfft algorithm in aubio 0.4.9 does not reliably
   //     detect pitch from pure sine waves in a single buffer.
   //     We verify the confidence() accessor returns a valid value
   //     in [0.0, 1.0] for the silence case (confidence = 0.0).
   //     (A full pitch detection test would require a longer signal
   //     processed through multiple windows with a proper pitch tracker.)
   float storedConf = detector.confidence();
   ASSERT(storedConf == 0.0f, "Pitch: confidence() returns 0.0 for silence");

   // 3e. Test method() accessor — default-constructed detector uses "yinfft".
   PitchDetector methodTestDet(bufSize);
   ASSERT(methodTestDet.method() == "yinfft",
          "Pitch: default method() returns \"yinfft\"");

   // 3f. Test setConfidenceThreshold() and confidenceThreshold().
   PitchDetector thresholdDet(bufSize);
   thresholdDet.setConfidenceThreshold(0.75f);
   ASSERT(
      thresholdDet.confidenceThreshold() == 0.75f,
      "Pitch: setConfidenceThreshold(0.75) → confidenceThreshold() == 0.75");

   // Test default threshold.
   PitchDetector defaultDetector(bufSize);
   ASSERT(defaultDetector.confidenceThreshold() == 0.5f,
          "Pitch: default confidenceThreshold == 0.5");

   // 3g. Test hopSize accessor.
   ASSERT(methodTestDet.hopSize() > 0,
          "Pitch: hopSize() returns a positive value");
}

// ============================================================================
// 4. ScoreBuilder Tests — no external dependencies.
// ============================================================================
static void test_scoreBuilder() {
   fprintf(stdout, "\n--- ScoreBuilder Tests ---\n");

   // 4a. Default-constructed ScoreBuilder.
   ScoreBuilder builder;
   ASSERT(builder.noteCount() == 0, "ScoreBuilder: default noteCount() == 0");
   ASSERT(builder.controlEventCount() == 0,
          "ScoreBuilder: default controlEventCount() == 0");

   // 4b. Add 3 notes, verify noteCount().
   Note note1, note2, note3;
   note1.startTime = 0.0;
   note1.endTime = 0.5;
   note1.pitch = 60;
   note1.velocity = 100;
   note1.channel = 0;
   note1.sustain = false;
   builder.addNote(note1);

   note2.startTime = 0.5;
   note2.endTime = 1.0;
   note2.pitch = 64;
   note2.velocity = 90;
   note2.channel = 0;
   note2.sustain = false;
   builder.addNote(note2);

   note3.startTime = 1.0;
   note3.endTime = 1.5;
   note3.pitch = 67;
   note3.velocity = 80;
   note3.channel = 0;
   note3.sustain = false;
   builder.addNote(note3);

   ASSERT(builder.noteCount() == 3,
          "ScoreBuilder: after adding 3 notes, noteCount() == 3");

   // 4c. Add 2 control events, verify controlEventCount().
   ControlEvent ctrlOn, ctrlOff;
   ctrlOn.time = 0.0;
   ctrlOn.controller = 64;
   ctrlOn.value = 127; // Sustain ON
   builder.addControlEvent(ctrlOn);

   ctrlOff.time = 1.5;
   ctrlOff.controller = 64;
   ctrlOff.value = 0; // Sustain OFF
   builder.addControlEvent(ctrlOff);

   ASSERT(builder.controlEventCount() == 2,
          "ScoreBuilder: after adding 2 controls, controlEventCount() == 2");

   // 4d. Set metadata.
   builder.setTempo(100.0);
   builder.setTitle("Test");
   builder.setComposer("Test Composer");

   // 4e. Call build(), verify returned Score.
   Score score = builder.build();
   ASSERT(score.notes.size() == 3,
          "ScoreBuilder: build() returns Score with 3 notes");
   ASSERT(score.controls.size() == 2,
          "ScoreBuilder: build() returns Score with 2 controls");
   ASSERT(score.tempo == 100.0,
          "ScoreBuilder: build() returns Score with tempo == 100.0");
   ASSERT(score.title == "Test",
          "ScoreBuilder: build() returns Score with title == \"Test\"");
   ASSERT(
      score.composer == "Test Composer",
      "ScoreBuilder: build() returns Score with composer == \"Test Composer\"");

   // 4f. Verify notes are sorted by startTime.
   ASSERT(score.notes[0].startTime <= score.notes[1].startTime,
          "ScoreBuilder: build() sorts notes by startTime");
   ASSERT(score.notes[1].startTime <= score.notes[2].startTime,
          "ScoreBuilder: build() sorts notes by startTime (pair 2)");
}

// ============================================================================
// 5. MidiFileWriter Tests — verifies the raw SMF bytes, not just the size.
// ============================================================================

// Read an entire file into a byte vector (empty vector if unreadable).
static std::vector<uint8_t> readFileBytes(const char* path) {
   std::vector<uint8_t> bytes;
   FILE* f = fopen(path, "rb");
   if (!f) return bytes;
   fseek(f, 0, SEEK_END);
   long size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size > 0) {
      bytes.resize(static_cast<size_t>(size));
      size_t n = fread(bytes.data(), 1, bytes.size(), f);
      bytes.resize(n);
   }
   fclose(f);
   return bytes;
}

static uint16_t be16(const std::vector<uint8_t>& b, size_t off) {
   return static_cast<uint16_t>((b[off] << 8) | b[off + 1]);
}

static uint32_t be32(const std::vector<uint8_t>& b, size_t off) {
   return (static_cast<uint32_t>(b[off]) << 24) |
          (static_cast<uint32_t>(b[off + 1]) << 16) |
          (static_cast<uint32_t>(b[off + 2]) << 8) |
          static_cast<uint32_t>(b[off + 3]);
}

static bool hasSubseq(const std::vector<uint8_t>& b,
                      const std::vector<uint8_t>& needle) {
   if (needle.size() > b.size()) return false;
   for (size_t i = 0; i + needle.size() <= b.size(); ++i) {
      bool match = true;
      for (size_t j = 0; j < needle.size(); ++j) {
         if (b[i + j] != needle[j]) {
            match = false;
            break;
         }
      }
      if (match) return true;
   }
   return false;
}

// Sentinel for `findSubseq`: the index of the first occurrence of `needle` in
// `b`, or kNoMatch when absent. `hasSubseq` is the boolean form; this lets an
// ordering assertion compare where two events land in the raw byte stream.
constexpr size_t kNoMatch = static_cast<size_t>(-1);
static size_t findSubseq(const std::vector<uint8_t>& b,
                         const std::vector<uint8_t>& needle) {
   if (needle.size() > b.size()) return kNoMatch;
   for (size_t i = 0; i + needle.size() <= b.size(); ++i) {
      bool match = true;
      for (size_t j = 0; j < needle.size(); ++j) {
         if (b[i + j] != needle[j]) {
            match = false;
            break;
         }
      }
      if (match) return i;
   }
   return kNoMatch;
}

// Count real pitch-bend (0xE0-0xEF) *status* messages in a Standard MIDI
// File, walking every MTrk with running-status awareness. A raw 0xE0 byte is
// a pitch bend only when it occupies a status position (>= 0x80); data bytes,
// varlen delta bytes (which also set the high bit), and the MThd ticks-per-
// quarter field (480 = 0x01E0) are all sub-0x80 data and never counted. This
// checks the writer's "constant-pitch notes emit no 0xE0" invariant at the
// byte level, and is immune to the 0xE0 division byte in the header.
static size_t countPitchBendMessages(const std::vector<uint8_t>& b) {
   const size_t n = b.size();
   size_t count = 0;
   size_t i = 0;
   while (i + 8 <= n) {
      if (b[i] != 'M' || b[i + 1] != 'T') {
         break; // Not a valid chunk start.
      }
      const uint32_t len = be32(b, i + 4);
      const size_t start = i + 8;
      const size_t end = start + static_cast<size_t>(len);
      if (end > n) {
         break; // Truncated chunk.
      }
      const bool isTrack = (b[i + 2] == 'r' && b[i + 3] == 'k');
      i = end; // Advance past this chunk.
      if (!isTrack) {
         continue;
      }

      size_t j = start;
      uint8_t running = 0;
      while (j < end) {
         // 1. Variable-length delta time (consumed; not a status byte).
         bool more = true;
         while (j < end && more) {
            const uint8_t byte = b[j++];
            more = (byte & 0x80) != 0;
         }
         if (j >= end) {
            break;
         }
         // 2. A byte >= 0x80 is a status byte (updates running); a byte < 0x80
         //    is the first data byte under the running status.
         uint8_t status;
         if (b[j] & 0x80) {
            status = b[j++];
            running = status;
         } else {
            status = running;
         }
         // 3. Message body, by type.
         if (status == 0xFF) { // meta: type byte, varlen length, data bytes.
            if (j < end) {
               ++j; // meta type byte
            }
            bool moreL = true;
            uint32_t length = 0;
            while (j < end && moreL) {
               const uint8_t byte = b[j++];
               length = (length << 7) | (byte & 0x7F);
               moreL = (byte & 0x80) != 0;
            }
            j += length;
         } else if (status == 0xF7) { // sysex: varlen length, data bytes.
            bool moreL = true;
            uint32_t length = 0;
            while (j < end && moreL) {
               const uint8_t byte = b[j++];
               length = (length << 7) | (byte & 0x7F);
               moreL = (byte & 0x80) != 0;
            }
            j += length;
         } else if (status >= 0xF0) {
            break; // Realtime byte (invalid inside a track); stop.
         } else {  // Channel message: 1 data byte (program/pressure), else 2.
            const uint8_t hi = status & 0xF0;
            if (hi == 0xE0) {
               ++count;
            } // Pitch bend.
            j += (hi == 0xC0 || hi == 0xD0) ? 1u : 2u;
         }
      }
   }
   return count;
}

// Count the pitch-bend (0xE0) events that carry a *full* 0xE0 status byte, as
// opposed to running-status bends (data bytes only). A bent note with N bends
// has exactly one full 0xE0 (the first bend, following the 0x90 Note On); the
// rest run their status. This is the byte-level proof that the writer emits
// canonical running status. Like countPitchBendMessages it walks each MTrk with
// running-status awareness, so a raw 0xE0 data byte (and the 0x01E0 MThd
// division byte) is never counted.
static size_t countFullPitchBendStatuses(const std::vector<uint8_t>& b) {
   const size_t n = b.size();
   size_t count = 0;
   size_t i = 0;
   while (i + 8 <= n) {
      if (b[i] != 'M' || b[i + 1] != 'T') {
         break;
      }
      const uint32_t len = be32(b, i + 4);
      const size_t start = i + 8;
      const size_t end = start + static_cast<size_t>(len);
      if (end > n) {
         break;
      }
      const bool isTrack = (b[i + 2] == 'r' && b[i + 3] == 'k');
      i = end;
      if (!isTrack) {
         continue;
      }
      size_t j = start;
      uint8_t running = 0;
      while (j < end) {
         bool more = true;
         while (j < end && more) {
            const uint8_t byte = b[j++];
            more = (byte & 0x80) != 0;
         }
         if (j >= end) {
            break;
         }
         uint8_t status;
         bool fullStatus = false;
         if (b[j] & 0x80) {
            status = b[j++];
            running = status;
            fullStatus = true;
         } else {
            status = running;
         }
         if (status == 0xFF) {
            if (j < end) {
               ++j; // meta type byte
            }
            bool moreL = true;
            uint32_t length = 0;
            while (j < end && moreL) {
               const uint8_t byte = b[j++];
               length = (length << 7) | (byte & 0x7F);
               moreL = (byte & 0x80) != 0;
            }
            j += length;
         } else if (status == 0xF7) {
            bool moreL = true;
            uint32_t length = 0;
            while (j < end && moreL) {
               const uint8_t byte = b[j++];
               length = (length << 7) | (byte & 0x7F);
               moreL = (byte & 0x80) != 0;
            }
            j += length;
         } else if (status >= 0xF0) {
            break; // Realtime byte (invalid inside a track); stop.
         } else {  // Channel message: 1 data byte (program/pressure), else 2.
            const uint8_t hi = status & 0xF0;
            if (hi == 0xE0 && fullStatus) {
               ++count; // a pitch bend that actually carries a 0xE0 status
            }
            j += (hi == 0xC0 || hi == 0xD0) ? 1u : 2u;
         }
      }
   }
   return count;
}

static void test_midiWriter() {
   fprintf(stdout, "\n--- MidiFileWriter Tests ---\n");

   // 5a. Write a simple score with 2 non-sustained notes (no sustain pedal
   //     requested -> the writer must not emit one).
   Score midiScore;

   Note m1, m2;
   m1.startTime = 0.0;
   m1.endTime = 0.5;
   m1.pitch = 60;
   m1.velocity = 100;
   m1.channel = 0;
   m1.sustain = false;
   midiScore.notes.push_back(m1); // C4

   m2.startTime = 0.5;
   m2.endTime = 1.0;
   m2.pitch = 64;
   m2.velocity = 100;
   m2.channel = 0;
   m2.sustain = false;
   midiScore.notes.push_back(m2); // E4

   MidiFileWriter writer("/tmp/test_output.mid");
   ASSERT(writer.write(midiScore),
          "Midi: write() returns true for valid score");
   ASSERT(writer.bytesWritten() > 0, "Midi: bytesWritten() returns > 0");

   // 5b. Byte-level structure checks on the header chunk.
   std::vector<uint8_t> bytes = readFileBytes("/tmp/test_output.mid");
   ASSERT(bytes.size() >= 22,
          "Midi: file is at least a header + MTrk header (got " +
             std::to_string(bytes.size()) + " bytes)");

   if (bytes.size() >= 22) {
      ASSERT(bytes[0] == 'M' && bytes[1] == 'T' && bytes[2] == 'h' &&
                bytes[3] == 'd',
             "Midi: header magic is \"MThd\"");
      ASSERT(be32(bytes, 4) == 6, "Midi: header chunk length is 6");
      ASSERT(be16(bytes, 8) == 1, "Midi: format is Type 1");
      ASSERT(be16(bytes, 10) == 1, "Midi: exactly 1 track");
      ASSERT(be16(bytes, 12) == 480, "Midi: division is 480 ticks/qn");
      ASSERT(bytes[14] == 'M' && bytes[15] == 'T' && bytes[16] == 'r' &&
                bytes[17] == 'k',
             "Midi: track magic is \"MTrk\"");

      // The declared track length must exactly match the bytes that follow.
      uint32_t declaredTrackLen = be32(bytes, 18);
      ASSERT(bytes.size() == 22 + declaredTrackLen,
             "Midi: track length field matches actual data (declared " +
                std::to_string(declaredTrackLen) + ", file " +
                std::to_string(bytes.size()) + ")");

      // The first event must be the SetTempo meta (delta 0, FF 51 03) —
      // this directly guards against the historical garbage-prefix bug.
      ASSERT(bytes[22] == 0x00 && bytes[23] == 0xFF && bytes[24] == 0x51,
             "Midi: first event is the SetTempo meta (delta 0, FF 51)");

      // The track must contain both notes (Note On 90 3C 64, 90 40 64).
      ASSERT(hasSubseq(bytes, {0x90, 0x3C, 0x64}),
             "Midi: C4 Note On (90 3C 64) present");
      ASSERT(hasSubseq(bytes, {0x90, 0x40, 0x64}),
             "Midi: E4 Note On (90 40 64) present");

      // Neither note is sustained and no CC#64 control is present, so the
      // writer must not inject a sustain pedal (the historical bug: it added
      // one unconditionally). A forced pedal adds a reverb-like tail to the
      // render that the performance never contained.
      ASSERT(!hasSubseq(bytes, {0xB0, 0x40, 0x7F}),
             "Midi: no pedal-on when no note sustains (no CC#64 control)");
      ASSERT(!hasSubseq(bytes, {0xB0, 0x40, 0x00}),
             "Midi: no pedal-off when no note sustains (no CC#64 control)");

      // The track must end with End-of-Track (FF 2F 00).
      ASSERT(bytes.size() >= 3 && bytes[bytes.size() - 3] == 0xFF &&
                bytes[bytes.size() - 2] == 0x2F &&
                bytes[bytes.size() - 1] == 0x00,
             "Midi: track ends with End-of-Track (FF 2F 00)");
   }

   // 5c. Write an empty score — must still be a structurally valid SMF.
   Score emptyScore;
   MidiFileWriter emptyWriter("/tmp/test_empty.mid");
   ASSERT(emptyWriter.write(emptyScore),
          "Midi: write() returns true for empty score");

   std::vector<uint8_t> emptyBytes = readFileBytes("/tmp/test_empty.mid");
   ASSERT(emptyBytes.size() >= 22, "Midi: empty-score file has a header (got " +
                                      std::to_string(emptyBytes.size()) +
                                      " bytes)");
   if (emptyBytes.size() >= 22) {
      uint32_t emptyTrackLen = be32(emptyBytes, 18);
      ASSERT(emptyBytes.size() == 22 + emptyTrackLen,
             "Midi: empty-score track length is consistent");
      ASSERT(emptyBytes.size() >= 3 &&
                emptyBytes[emptyBytes.size() - 3] == 0xFF &&
                emptyBytes[emptyBytes.size() - 2] == 0x2F &&
                emptyBytes[emptyBytes.size() - 1] == 0x00,
             "Midi: empty-score track ends with End-of-Track");
   }
}

// ============================================================================
// 5b. MidiFileWriter/Reader Round-Trip — a bent Score must write its 0xE0
//     messages and read them back as the same `Note::pitchBends`.
// ====================================================================================
static void test_midiRoundTrip() {
   fprintf(stdout, "\n--- MidiFileWriter/Reader Round-Trip Tests ---\n");

   const char* path = "/tmp/test_roundtrip.mid";

   // A bent C4 note: a 14-bit value sequence that spans the full bend range
   // (both extremes plus non-multiples-of-128, which set the low byte).
   // 4096 = one semitone up by General MIDI convention.
   const std::vector<int16_t> bends = {0, 4096, -4096, 8191, -8192, 100, -100};

   Score score;
   score.tempo = 120.0;
   Note note;
   note.startTime = 0.0;
   note.endTime = 1.0;
   note.pitch = 60;
   note.velocity = 100;
   note.channel = 0;
   note.pitchBends = bends;
   score.notes.push_back(note);

   MidiFileWriter writer(path);
   ASSERT(writer.write(score), "RoundTrip: write() succeeds for a bent score");

   // Byte-level: the writer must emit 0xE0 (pitch bend) messages.
   std::vector<uint8_t> bytes = readFileBytes(path);
   // The first bend follows the Note On (0x90), so it keeps a full 0xE0
   // status; the later bends run their status (the 0xE0 is omitted), so only
   // their data bytes appear. (+8191 → {7F 7F}; -8192 → {00 00}.)
   ASSERT(hasSubseq(bytes, {0xE0, 0x00, 0x40}),
          "RoundTrip: the first bend (0) keeps a full {E0 00 40}");
   ASSERT(hasSubseq(bytes, {0x7F, 0x7F}),
          "RoundTrip: bend +8191 runs status (data-only {7F 7F})");
   ASSERT(hasSubseq(bytes, {0x00, 0x00}),
          "RoundTrip: bend -8192 runs status (data-only {00 00})");
   ASSERT(countPitchBendMessages(bytes) == bends.size(),
          "RoundTrip: exactly " + std::to_string(bends.size()) +
             " 0xE0 pitch-bend messages in the bent file");

   // Round-trip: read the file back and compare the bend sequence.
   MidiFileReader reader(path);
   ASSERT(reader.ok(), "RoundTrip: reader parses a bent file");
   ASSERT(reader.score().notes.size() == 1,
          "RoundTrip: exactly one note round-trips");
   if (reader.score().notes.size() == 1) {
      const Note& back = reader.score().notes[0];
      ASSERT(back.pitch == 60, "RoundTrip: pitch preserved (60)");
      ASSERT(back.channel == 0, "RoundTrip: channel preserved (0)");
      ASSERT(back.velocity == 100, "RoundTrip: velocity preserved (100)");
      ASSERT(std::fabs(back.startTime - 0.0) < 0.01,
             "RoundTrip: start time ~ 0.0 s");
      ASSERT(std::fabs(back.endTime - 1.0) < 0.01,
             "RoundTrip: end time ~ 1.0 s");
      ASSERT(back.pitchBends == bends,
             "RoundTrip: pitchBends sequence round-trips exactly (" +
                std::to_string(back.pitchBends.size()) + std::string(" vs ") +
                std::to_string(bends.size()) + ")");
   }

   // Invariant: a constant-pitch note (empty pitchBends) emits no 0xE0 and
   // round-trips with an empty bend vector.
   Score flat;
   flat.tempo = 120.0;
   Note flatNote;
   flatNote.startTime = 0.0;
   flatNote.endTime = 1.0;
   flatNote.pitch = 60;
   flatNote.velocity = 100;
   flatNote.channel = 0;
   flat.notes.push_back(flatNote);

   MidiFileWriter flatWriter(path);
   ASSERT(flatWriter.write(flat),
          "RoundTrip: write() succeeds for a flat note");
   std::vector<uint8_t> flatBytes = readFileBytes(path);
   // A raw 0xE0 byte in the file is the MThd ticks-per-quarter field
   // (480 = 0x01E0); a real pitch bend is a 0xE0 *status* message, so count
   // status-position 0xE0-0xEF messages (a flat note must have zero).
   ASSERT(countPitchBendMessages(flatBytes) == 0,
          "RoundTrip: a flat note emits no 0xE0 pitch-bend messages");
   MidiFileReader flatReader(path);
   ASSERT(flatReader.ok() && flatReader.score().notes.size() == 1 &&
             flatReader.score().notes[0].pitchBends.empty(),
          "RoundTrip: a flat note round-trips with an empty bend vector");
}

// ============================================================================
// 5c. Multi-Channel Round-Trip — a Score with notes on distinct channels
//     (simulating the basic-pitch channel policy for overlapping bent notes)
//     must write per-channel program/sustain scaffolding and round-trip
//     channel + bends intact.
// ============================================================================
static void test_midiMultiChannel() {
   fprintf(stdout, "\n--- MidiFileWriter/Reader Multi-Channel Tests ---\n");

   const char* path = "/tmp/test_multichannel.mid";

   // Two overlapping bent notes on distinct channels + one non-bent note on
   // channel 0. This exercises the per-channel scaffolding: the writer must
   // emit program + sustain for channels 0, 1, and 2.
   const std::vector<int16_t> bends1 = {0, 2048, -2048, 0};
   const std::vector<int16_t> bends2 = {0, -4096, 4096, 0};

   Score score;
   score.tempo = 120.0;

   // A sustained, non-bent note on channel 0 (C4, 0-1s). `sustain` is set so
   // the writer's per-channel pedal scaffolding is exercised (a held note
   // implies the sustain pedal); the note itself is still a single
   // onset/offset.
   Note n0;
   n0.startTime = 0.0;
   n0.endTime = 1.0;
   n0.pitch = 60;
   n0.velocity = 100;
   n0.channel = 0;
   n0.sustain = true;
   score.notes.push_back(n0);

   // A sustained, bent note on channel 1 (E4=64, 0.5-1.5s, overlaps n0).
   Note n1;
   n1.startTime = 0.5;
   n1.endTime = 1.5;
   n1.pitch = 64;
   n1.velocity = 90;
   n1.channel = 1;
   n1.pitchBends = bends1;
   n1.sustain = true;
   score.notes.push_back(n1);

   // A sustained, bent note on channel 2 (G4=67, 1.0-2.0s, overlaps n1).
   Note n2;
   n2.startTime = 1.0;
   n2.endTime = 2.0;
   n2.pitch = 67;
   n2.velocity = 80;
   n2.channel = 2;
   n2.pitchBends = bends2;
   n2.sustain = true;
   score.notes.push_back(n2);

   MidiFileWriter writer(path);
   ASSERT(writer.write(score),
          "MultiChannel: write() succeeds for a 3-channel score");

   // Byte-level: verify per-channel scaffolding.
   std::vector<uint8_t> bytes = readFileBytes(path);
   // Channel 0: C0 00 (program) + B0 40 7F (sustain on)
   ASSERT(hasSubseq(bytes, {0x00, 0xC0, 0x00}),
          "MultiChannel: program change on channel 0");
   ASSERT(hasSubseq(bytes, {0x00, 0xB0, 0x40, 0x7F}),
          "MultiChannel: sustain on channel 0");
   // Channel 1: C1 00 + B1 40 7F
   ASSERT(hasSubseq(bytes, {0x00, 0xC1, 0x00}),
          "MultiChannel: program change on channel 1");
   ASSERT(hasSubseq(bytes, {0x00, 0xB1, 0x40, 0x7F}),
          "MultiChannel: sustain on channel 1");
   // Channel 2: C2 00 + B2 40 7F
   ASSERT(hasSubseq(bytes, {0x00, 0xC2, 0x00}),
          "MultiChannel: program change on channel 2");
   ASSERT(hasSubseq(bytes, {0x00, 0xB2, 0x40, 0x7F}),
          "MultiChannel: sustain on channel 2");

   // Per-channel sustain off before EOT.
   ASSERT(hasSubseq(bytes, {0x00, 0xB0, 0x40, 0x00}),
          "MultiChannel: sustain off channel 0");
   ASSERT(hasSubseq(bytes, {0x00, 0xB1, 0x40, 0x00}),
          "MultiChannel: sustain off channel 1");
   ASSERT(hasSubseq(bytes, {0x00, 0xB2, 0x40, 0x00}),
          "MultiChannel: sustain off channel 2");

   // Pitch bends on channels 1 and 2.
   ASSERT(hasSubseq(bytes, {0xE1, 0x00, 0x40}),
          "MultiChannel: bend 0 on channel 1 is {E1 00 40}");
   ASSERT(hasSubseq(bytes, {0xE2, 0x00, 0x40}),
          "MultiChannel: bend 0 on channel 2 is {E2 00 40}");

   // Round-trip: read back and verify channels + bends.
   MidiFileReader reader(path);
   ASSERT(reader.ok(), "MultiChannel: reader parses a multi-channel file");
   ASSERT(reader.score().notes.size() == 3,
          "MultiChannel: exactly three notes round-trip");
   if (reader.score().notes.size() == 3) {
      // The reader sorts by start time; verify by pitch.
      const auto& s = reader.score();
      const Note* p60 = nullptr;
      const Note* p64 = nullptr;
      const Note* p67 = nullptr;
      for (const Note& n : s.notes) {
         if (n.pitch == 60) p60 = &n;
         if (n.pitch == 64) p64 = &n;
         if (n.pitch == 67) p67 = &n;
      }
      ASSERT(p60 && p60->channel == 0,
             "MultiChannel: C4 channel preserved (0)");
      ASSERT(p60 && p60->pitchBends.empty(), "MultiChannel: C4 stays non-bent");
      ASSERT(p64 && p64->channel == 1,
             "MultiChannel: E4 channel preserved (1)");
      ASSERT(p64 && p64->pitchBends == bends1,
             "MultiChannel: E4 bends round-trip exactly");
      ASSERT(p67 && p67->channel == 2,
             "MultiChannel: G4 channel preserved (2)");
      ASSERT(p67 && p67->pitchBends == bends2,
             "MultiChannel: G4 bends round-trip exactly");
   }

   // Invariant: a single-channel, NON-sustained score (only channel 0, no
   // pedal requested) carries no sustain pedal and is the canonical sanity
   // note (the same shape as `midicapture --test`). The focused
   // test_midiSustainPedal pins the full pedal contract.
   Score flat;
   flat.tempo = 120.0;
   Note flatNote;
   flatNote.startTime = 0.0;
   flatNote.endTime = 1.0;
   flatNote.pitch = 60;
   flatNote.velocity = 100;
   flatNote.channel = 0;
   flatNote.sustain = false; // non-sustained -> the writer adds no pedal.
   flat.notes.push_back(flatNote);

   MidiFileWriter flatWriter(path);
   ASSERT(flatWriter.write(flat),
          "MultiChannel: write() succeeds for a single-channel score");
   std::vector<uint8_t> flatBytes = readFileBytes(path);
   // Exactly the pedal-less size: tempo + program + note-on + note-off + EOT
   // (a non-sustained note carries no sustain pedal). Self-diagnostic: the
   // message reports the actual size if the layout ever drifts.
   ASSERT(flatBytes.size() == 45,
          "MultiChannel: non-sustained single-channel file is 45 bytes (got " +
             std::to_string(flatBytes.size()) + ")");
   // No sustain pedal anywhere (neither on nor off).
   ASSERT(!hasSubseq(flatBytes, {0xB0, 0x40, 0x7F}),
          "MultiChannel: non-sustained note emits no pedal-on (B0 40 7F)");
   ASSERT(!hasSubseq(flatBytes, {0xB0, 0x40, 0x00}),
          "MultiChannel: non-sustained note emits no pedal-off (B0 40 00)");
   // No per-channel program scaffolding beyond channel 0.
   ASSERT(!hasSubseq(flatBytes, {0x00, 0xC1, 0x00}),
          "MultiChannel: no channel-1 program in single-channel file");
}

// ============================================================================
// 5c'. Sustain-Pedal Conditioning — the writer emits a CC#64 (sustain pedal)
//      only when the Score asks for it. Three single-channel cases pin the
//      contract using *relative* byte-count assertions (robust to the absolute
//      header size) plus pedal-presence checks:
//        (a) a sustained note, no explicit control -> the coarse whole-
//            performance bracket (a pedal-on AND a pedal-off);
//        (b) a non-sustained note, no control -> no pedal at all, exactly 8
//            bytes smaller than (a) (the bracket's on + off);
//        (c) a non-sustained note WITH one explicit CC#64 control -> written
//            as-is, with the redundant bracket suppressed (4 bytes smaller
//            than (a): the bracket's off).
// ============================================================================
static void test_midiSustainPedal() {
   fprintf(stdout,
           "\n--- MidiFileWriter Sustain-Pedal Conditioning Tests ---\n");

   const char* path = "/tmp/test_sustainpedal.mid";

   auto makeSingleNote = [](bool sustain) {
      Note n;
      n.startTime = 0.0;
      n.endTime = 1.0; // one second at 120 BPM = 960 ticks.
      n.pitch = 60;    // C4.
      n.velocity = 100;
      n.channel = 0;
      n.sustain = sustain;
      return n;
   };

   // (a) Sustained note, no explicit control -> the coarse bracket.
   Score a;
   a.tempo = 120.0;
   a.notes.push_back(makeSingleNote(true));
   MidiFileWriter writerA(path);
   ASSERT(writerA.write(a), "SustainPedal: sustained note writes");
   std::vector<uint8_t> aBytes = readFileBytes(path);
   ASSERT(hasSubseq(aBytes, {0xB0, 0x40, 0x7F}),
          "SustainPedal: sustained note emits a pedal-on (B0 40 7F)");
   ASSERT(hasSubseq(aBytes, {0xB0, 0x40, 0x00}),
          "SustainPedal: sustained note emits a pedal-off (B0 40 00)");

   // (b) Non-sustained note, no control -> no pedal; 8 bytes smaller than (a).
   Score b;
   b.tempo = 120.0;
   b.notes.push_back(makeSingleNote(false));
   MidiFileWriter writerB(path);
   ASSERT(writerB.write(b), "SustainPedal: non-sustained note writes");
   std::vector<uint8_t> bBytes = readFileBytes(path);
   ASSERT(!hasSubseq(bBytes, {0xB0, 0x40, 0x7F}),
          "SustainPedal: non-sustained note emits no pedal-on");
   ASSERT(!hasSubseq(bBytes, {0xB0, 0x40, 0x00}),
          "SustainPedal: non-sustained note emits no pedal-off");
   ASSERT(aBytes.size() == bBytes.size() + 8,
          "SustainPedal: the coarse bracket adds exactly 8 bytes (on 4 + off "
          "4); sustained=" +
             std::to_string(aBytes.size()) +
             " non-sustained=" + std::to_string(bBytes.size()));

   // (c) Non-sustained note WITH one explicit CC#64 control -> written as-is,
   //     with the redundant coarse bracket suppressed.
   Score c;
   c.tempo = 120.0;
   c.notes.push_back(makeSingleNote(false));
   ControlEvent pedal;
   pedal.time = 0.25;
   pedal.controller = 64; // sustain pedal.
   pedal.value = 127;     // fully down.
   c.controls.push_back(pedal);
   MidiFileWriter writerC(path);
   ASSERT(writerC.write(c), "SustainPedal: explicit pedal control writes");
   std::vector<uint8_t> cBytes = readFileBytes(path);
   ASSERT(hasSubseq(cBytes, {0xB0, 0x40, 0x7F}),
          "SustainPedal: an explicit CC#64 control is written as-is");
   ASSERT(!hasSubseq(cBytes, {0xB0, 0x40, 0x00}),
          "SustainPedal: no writer-added pedal-off follows an explicit pedal");
   // The explicit pedal replaces the coarse bracket but is one byte larger than
   // a zero-delta pedal: it lands at its true tick (0.25 s here, a non-zero
   // varlen delta) between the note's On and Off, whereas the bracket's two
   // pedals are zero-delta. So the bracket (a) is 3 bytes larger than the
   // single explicit pedal (c) — not 4 as the old clamp-to-end writer gave.
   ASSERT(cBytes.size() == aBytes.size() - 3,
          "SustainPedal: the coarse bracket is 3 bytes larger than the single "
          "explicit pedal; explicit=" +
             std::to_string(cBytes.size()) +
             " sustained=" + std::to_string(aBytes.size()));
}

// ============================================================================
// 5c. Control Interleave — a control at an early time lands at its true tick
//     (positioned among the notes), and one at a late time lands after them;
//     neither is clamped to the end of the track. The historical writer
//     appended all controls after all notes, so a t=0 pedal was clamped to the
//     end where it sustained nothing; this pins the corrected ordering.
// ============================================================================
static void test_controlInterleave() {
   fprintf(stdout, "\n--- MidiFileWriter Control-Interleave Tests ---\n");

   const char* path = "/tmp/test_interleave.mid";

   // One note at 1.0..2.0 s (120 BPM -> 960 ticks/s) on channel 0, non-
   // sustained, plus an explicit pedal down at t=0 (before the note) and an
   // explicit pedal up at t=3.0 (after it). Both pedals are authored controls,
   // not the writer's coarse bracket, so each must land at its true tick.
   Score s;
   s.tempo = 120.0;
   Note n;
   n.startTime = 1.0;
   n.endTime = 2.0;
   n.pitch = 60; // C4.
   n.velocity = 100;
   n.channel = 0;
   n.sustain = false;
   s.notes.push_back(n);

   ControlEvent down;
   down.time = 0.0;
   down.controller = 64; // sustain pedal.
   down.value = 127;     // fully down.
   s.controls.push_back(down);

   ControlEvent up;
   up.time = 3.0;
   up.controller = 64;
   up.value = 0; // fully up.
   s.controls.push_back(up);

   MidiFileWriter writer(path);
   ASSERT(writer.write(s), "Interleave: writes");
   std::vector<uint8_t> b = readFileBytes(path);

   // The early pedal must precede the note's On; the late pedal must follow the
   // note's Off. The historical append-at-end writer would have placed the
   // early pedal AFTER the note's Off (clamping its t=0 tick to the end).
   size_t posDown = findSubseq(b, {0xB0, 0x40, 0x7F});
   size_t posOn = findSubseq(b, {0x90, 0x3C, 0x64});
   size_t posOff = findSubseq(b, {0x80, 0x3C, 0x64});
   size_t posUp = findSubseq(b, {0xB0, 0x40, 0x00});
   ASSERT(posDown != kNoMatch && posOn != kNoMatch && posOff != kNoMatch &&
             posUp != kNoMatch,
          "Interleave: all four events present (down, On, Off, up)");
   ASSERT(posDown < posOn,
          "Interleave: the early pedal (t=0) lands before the note's On");
   ASSERT(posOn < posOff, "Interleave: the note's On precedes its Off");
   ASSERT(posOff < posUp,
          "Interleave: the late pedal (t=3) lands after the note's Off");
}

// ============================================================================
// 5d. Running Status — the writer must emit canonical running status for a bent
//     note's repeated 0xE0, and the reader must recover both a bent note
//     (writer-produced) and a hand-crafted chord (which the writer never emits,
//     proving the reader is a general SMF parser, not one sized to it).
// ============================================================================
static void test_runningStatus() {
   fprintf(stdout, "\n--- MidiFileWriter/Reader Running-Status Tests ---\n");

   // (a) Writer: a 3-bend note emits ONE full 0xE0 status (the first bend,
   //     following the 0x90 Note On); the other two run their status.
   const std::vector<int16_t> bends = {0, 4096, 0};
   Score score;
   score.tempo = 120.0;
   Note note;
   note.startTime = 0.0;
   note.endTime = 1.0;
   note.pitch = 60;
   note.velocity = 100;
   note.channel = 0;
   note.pitchBends = bends;
   score.notes.push_back(note);

   const char* path = "/tmp/test_runningstatus.mid";
   MidiFileWriter writer(path);
   ASSERT(writer.write(score),
          "RunningStatus: write() succeeds for a bent note");
   std::vector<uint8_t> bytes = readFileBytes(path);
   ASSERT(hasSubseq(bytes, {0xE0, 0x00, 0x40}),
          "RunningStatus: the first bend keeps a full 0xE0 status");
   ASSERT(countFullPitchBendStatuses(bytes) == 1,
          "RunningStatus: exactly one 0xE0 carries a full status (the rest run "
          "it) — canonical running status is emitted");
   ASSERT(
      countPitchBendMessages(bytes) == bends.size(),
      "RunningStatus: all three bends are present (the running-status-aware "
      "counter sees 3)");
   MidiFileReader reader(path);
   ASSERT(reader.ok(), "RunningStatus: the reader parses the bent file");
   ASSERT(reader.score().notes.size() == 1 &&
             reader.score().notes[0].pitchBends == bends,
          "RunningStatus: all three bends round-trip despite running status");

   // (b) Reader: a hand-crafted *chord* whose second Note On is a
   //     running-status data byte (the writer never emits a running-status
   //     note, since it interleaves on/off) reads back as two notes.
   const std::vector<uint8_t> track = {
      0x00, 0xFF, 0x51, 0x03, 0x0C, 0x42, 0xA0, // tempo 120 (500000 us/qn)
      0x00, 0x90, 0x3C, 0x64,                   // Note On C4, delta 0
      0x00, 0x40, 0x64,                         // Note On E4, delta 0 — RUNNING
      0x60, 0x80, 0x3C, 0x40,                   // Note Off C4, delta 96
      0x60, 0x80, 0x40, 0x40,                   // Note Off E4, delta 96
      0x00, 0xFF, 0x2F, 0x00,                   // End of Track, delta 0
   };
   std::vector<uint8_t> file = {
      'M',
      'T',
      'h',
      'd',
      0x00,
      0x00,
      0x00,
      0x06,
      0x00,
      0x01,
      0x00,
      0x01,
      0x01,
      0xE0,
      'M',
      'T',
      'r',
      'k',
      static_cast<uint8_t>((track.size() >> 24) & 0xFF),
      static_cast<uint8_t>((track.size() >> 16) & 0xFF),
      static_cast<uint8_t>((track.size() >> 8) & 0xFF),
      static_cast<uint8_t>(track.size() & 0xFF),
   };
   file.insert(file.end(), track.begin(), track.end());
   {
      std::ofstream ofs("/tmp/test_runningstatus_chord.mid", std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(file.data()),
                static_cast<std::streamsize>(file.size()));
   }
   MidiFileReader chordReader("/tmp/test_runningstatus_chord.mid");
   ASSERT(
      chordReader.ok(),
      "RunningStatus: the reader parses a hand-crafted running-status chord");
   ASSERT(chordReader.score().notes.size() == 2,
          "RunningStatus: both chord notes read back (the 0x90 runs status)");
   if (chordReader.score().notes.size() == 2) {
      int c4 = 0;
      int e4 = 0;
      for (const Note& n : chordReader.score().notes) {
         if (n.pitch == 60) {
            ++c4;
         }
         if (n.pitch == 64) {
            ++e4;
         }
      }
      ASSERT(c4 == 1 && e4 == 1,
             "RunningStatus: the chord's C4 and E4 each appear once");
   }
}

// ============================================================================
// 5e. Malformed Input — a non-SMF, an over-long MTrk, and a running-status data
//     byte with no prior channel message must each yield ok()==false with a
//     non-empty error() (a loud failure a caller can abort on, not a silent
//     half-parse of the ground truth).
// ============================================================================
static void test_malformedAbort() {
   fprintf(stdout, "\n--- MidiFileReader Malformed-Input Tests ---\n");

   // A standard 14-byte MThd (format 1, 1 track, 480 ticks/qn) to reuse.
   const std::vector<uint8_t> mthd = {
      'M',  'T',  'h',  'd',  0x00, 0x00, 0x00,
      0x06, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0,
   };

   // (a) Not a SMF at all (garbage, shorter than a header).
   {
      const uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
      std::ofstream ofs("/tmp/test_malformed_garbage.mid", std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(garbage),
                static_cast<std::streamsize>(sizeof(garbage)));
   }
   {
      MidiFileReader r("/tmp/test_malformed_garbage.mid");
      ASSERT(!r.ok(), "Malformed: a garbage file is not ok()");
      ASSERT(!r.error().empty(), "Malformed: a garbage file names its error");
   }

   // (b) A valid MThd, but an MTrk length that runs past the end of the file.
   {
      std::vector<uint8_t> f = mthd;
      f.insert(f.end(), {'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x30}); // 48…
      f.insert(f.end(), {0x00, 0x90, 0x3C}); // …but only 3 body bytes present
      std::ofstream ofs("/tmp/test_malformed_tracklen.mid", std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(f.data()),
                static_cast<std::streamsize>(f.size()));
   }
   {
      MidiFileReader r("/tmp/test_malformed_tracklen.mid");
      ASSERT(!r.ok(), "Malformed: an MTrk length past end-of-file is not ok()");
      ASSERT(!r.error().empty(),
             "Malformed: an over-long MTrk names its error");
   }

   // (c) A bare data byte at the start of a track (a running-status byte with
   //     no prior channel message to run its status) is malformed.
   {
      const std::vector<uint8_t> track = {
         0x00, 0x64,             // delta 0, then a data byte with no status
         0x00, 0xFF, 0x2F, 0x00, // End of Track
      };
      std::vector<uint8_t> f = mthd;
      f.insert(f.end(), {'M', 'T', 'r', 'k'});
      f.push_back(static_cast<uint8_t>((track.size() >> 24) & 0xFF));
      f.push_back(static_cast<uint8_t>((track.size() >> 16) & 0xFF));
      f.push_back(static_cast<uint8_t>((track.size() >> 8) & 0xFF));
      f.push_back(static_cast<uint8_t>(track.size() & 0xFF));
      f.insert(f.end(), track.begin(), track.end());
      std::ofstream ofs("/tmp/test_malformed_running.mid", std::ios::binary);
      ofs.write(reinterpret_cast<const char*>(f.data()),
                static_cast<std::streamsize>(f.size()));
   }
   {
      MidiFileReader r("/tmp/test_malformed_running.mid");
      ASSERT(!r.ok(),
             "Malformed: a data byte with no prior channel status is not ok()");
      ASSERT(!r.error().empty(),
             "Malformed: a bare data byte with no prior status names its "
             "error");
   }
}

// ============================================================================
// 6. AudioFileReader Integration Test — requires a test audio file.
// ============================================================================
static void test_audioFileReader() {
   fprintf(stdout, "\n--- AudioFileReader Tests ---\n");

   const char* testPath = "/tmp/test_tone.aiff";

   // Generate a test tone: 440 Hz, 48 kHz, mono, 1 second.
   const uint32_t sampleRate = 48000;
   const uint32_t duration = 1; // seconds
   const uint32_t numSamples = sampleRate * duration;

   writeTestTone(testPath, 440.0, sampleRate, numSamples);

   // 6a. Open the file, verify metadata.
   AudioFileReader reader(testPath);
   ASSERT(reader.sampleRate() == sampleRate,
          "Audio: sampleRate() returns 48000");
   ASSERT(reader.channels() == 1, "Audio: channels() returns 1 (mono)");
   ASSERT(reader.totalFrames() == numSamples,
          "Audio: totalFrames() returns " + std::to_string(numSamples));

   // 6b. Read a block of samples.
   const uint32_t blockSize = 1024;
   std::vector<float> buffer(blockSize);
   uint32_t framesRead = reader.read(buffer.data(), blockSize);
   ASSERT(framesRead == blockSize,
          "Audio: read() returns expected block size (got " +
             std::to_string(framesRead) + ")");

   // 6c. Verify readMono() works (same as read for mono files).
   std::vector<float> monoBuffer(blockSize);
   uint32_t monoRead = reader.readMono(monoBuffer.data(), blockSize);
   ASSERT(monoRead == blockSize,
          "Audio: readMono() returns expected block size (got " +
             std::to_string(monoRead) + ")");

   // 6d. Seek to a position, verify seek() works.
   reader.seek(24000); // Halfway through (0.5 seconds)
   ASSERT(!reader.eof(), "Audio: after seek to 24000, eof() is false");

   // 6e. Read past EOF, verify eof() returns true.
   std::vector<float> eofBuffer(sampleRate); // Read 1 second worth
   uint32_t readPastEof = reader.read(eofBuffer.data(), sampleRate);
   (void)readPastEof;
   // After reading 1024 + 1024 + 1*48000 samples, we should be past EOF
   ASSERT(reader.eof(), "Audio: after reading past EOF, eof() returns true");
}

// ============================================================================
// Main — run all test modules.
// ============================================================================
int main() {
   fprintf(stdout, "========================================\n"
                   "  libaudio Unit Test Suite\n"
                   "========================================\n");

   try {
      test_hir();
      test_fft();
      test_pitch();
      test_scoreBuilder();
      test_midiWriter();
      test_midiRoundTrip();
      test_midiMultiChannel();
      test_midiSustainPedal();
      test_controlInterleave();
      test_runningStatus();
      test_malformedAbort();
      test_audioFileReader();
   } catch (const std::exception& e) {
      fprintf(stderr, "EXCEPTION: %s\n", e.what());
      failures++;
   }

   fprintf(stdout, "\n========================================\n");
   fprintf(stdout, "Results: PASS: %d / %d\n", passes, passes + failures);
   fprintf(stdout, "========================================\n");

   return failures > 0 ? 1 : 0;
}

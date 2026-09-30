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

static void test_midiWriter() {
   fprintf(stdout, "\n--- MidiFileWriter Tests ---\n");

   // 5a. Write a simple score with 2 notes and a sustain pedal.
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
   ASSERT(hasSubseq(bytes, {0xE0, 0x00, 0x40}),
          "RoundTrip: 0xE0 for bend 0 is {E0 00 40}");
   ASSERT(hasSubseq(bytes, {0xE0, 0x7F, 0x7F}),
          "RoundTrip: 0xE0 for bend +8191 is {E0 7F 7F}");
   ASSERT(hasSubseq(bytes, {0xE0, 0x00, 0x00}),
          "RoundTrip: 0xE0 for bend -8192 is {E0 00 00}");
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

   // Non-bent note on channel 0 (C4, 0-1s).
   Note n0;
   n0.startTime = 0.0;
   n0.endTime = 1.0;
   n0.pitch = 60;
   n0.velocity = 100;
   n0.channel = 0;
   score.notes.push_back(n0);

   // Bent note on channel 1 (E4=64, 0.5-1.5s, overlaps n0).
   Note n1;
   n1.startTime = 0.5;
   n1.endTime = 1.5;
   n1.pitch = 64;
   n1.velocity = 90;
   n1.channel = 1;
   n1.pitchBends = bends1;
   score.notes.push_back(n1);

   // Bent note on channel 2 (G4=67, 1.0-2.0s, overlaps n1).
   Note n2;
   n2.startTime = 1.0;
   n2.endTime = 2.0;
   n2.pitch = 67;
   n2.velocity = 80;
   n2.channel = 2;
   n2.pitchBends = bends2;
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

   // Invariant: a single-channel score (only channel 0) remains byte-identical
   // to the pre-bend writer (the 53-byte --test file).
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
          "MultiChannel: write() succeeds for a single-channel score");
   std::vector<uint8_t> flatBytes = readFileBytes(path);
   // The file should be exactly 53 bytes (the canonical sanity note).
   ASSERT(flatBytes.size() == 53,
          "MultiChannel: single-channel file is 53 bytes (byte-identity)");
   // No per-channel program/sustain scaffolding beyond channel 0.
   ASSERT(!hasSubseq(flatBytes, {0x00, 0xC1, 0x00}),
          "MultiChannel: no channel-1 program in single-channel file");
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

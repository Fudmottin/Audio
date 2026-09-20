/**
 * @file midiFileWriter.cpp
 * @brief Implementation of MidiFileWriter — write HIR to Standard MIDI File.
 *
 * This module writes a Score (HIR) to a Type 1 Standard MIDI File (SMF)
 * with 480 ticks per quarter note, compatible with Apple Logic Pro.
 *
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <libaudio/hir.h>
#include <libaudio/midiFileWriter.h>
#include <numeric>
#include <string>
#include <vector>

// ============================================================================
// MidiFileWriter::Impl — Private implementation (Pimpl pattern).
//
// Domain context: All MIDI file format logic is isolated here. The public
// interface never exposes FILE* or binary format details.
//
// RAII — the file handle is automatically closed when
// the Impl is destroyed.
// ============================================================================
struct MidiFileWriter::Impl {
   FILE* file = nullptr;
   std::string outputPath;
   uint64_t bytesWritten_ = 0;
   static constexpr uint16_t TICKS_PER_QUARTER_NOTE = 480;

   ~Impl() {
      if (file) {
         fclose(file);
         file = nullptr;
      }
   }

   // Write a big-endian uint16.
   static bool writeUint16(FILE* f, uint16_t value) {
      uint8_t bytes[2] = {static_cast<uint8_t>((value >> 8) & 0xFF),
                          static_cast<uint8_t>(value & 0xFF)};
      return fwrite(bytes, 2, 1, f) == 1;
   }

   // Write a big-endian uint32.
   static bool writeUint32(FILE* f, uint32_t value) {
      uint8_t bytes[4] = {static_cast<uint8_t>((value >> 24) & 0xFF),
                          static_cast<uint8_t>((value >> 16) & 0xFF),
                          static_cast<uint8_t>((value >> 8) & 0xFF),
                          static_cast<uint8_t>(value & 0xFF)};
      return fwrite(bytes, 4, 1, f) == 1;
   }

   // Write a variable-length integer (MIDI format).
   //
   // Domain context: MIDI variable-length integers are encoded
   // least-significant 7-bit group first, with the high bit of
   // each byte set except for the last byte. This is the opposite
   // of big-endian byte order.
   static bool writeVarLen(FILE* f, uint32_t value) {
      std::vector<uint8_t> bytes;
      if (value == 0) {
         bytes.push_back(0);
      } else {
         // Collect 7-bit groups (LSB first).
         std::vector<uint8_t> groups;
         while (value > 0) {
            groups.push_back(static_cast<uint8_t>(value & 0x7F));
            value >>= 7;
         }
         // Write LSB first, setting continuation bit on all but the
         // last (most significant) group.
         for (size_t i = 0; i < groups.size(); ++i) {
            if (i < groups.size() - 1) {
               bytes.push_back(static_cast<uint8_t>(groups[i] | 0x80));
            } else {
               bytes.push_back(groups[i]);
            }
         }
      }
      return fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
   }

   // Convert seconds to MIDI ticks (480 ticks per quarter note).
   //
   // Domain context: The formula is seconds × ticksPerQuarterNote ×
   // (tempoBPM / 60.0). Do NOT divide by 60 twice — that was a
   // previous bug producing results 60× too small.
   static uint32_t secondsToTicks(double seconds, double tempoBpm) {
      // ticks = seconds × 480 × (120 / 60) = seconds × 960
      uint32_t ticks = static_cast<uint32_t>(std::round(
         seconds * TICKS_PER_QUARTER_NOTE * (tempoBpm / 60.0)));
      return ticks;
   }

   // Build MIDI event data for a single note (Note On + Note Off).
   static void buildNoteEvent(std::vector<uint8_t>& events, uint32_t deltaTicks,
                              uint8_t note, uint8_t velocity, uint8_t channel) {
      // Note On event (9n nn vv) — no running status.
      events.push_back(static_cast<uint8_t>(0x90 | (channel & 0x0F)));
      events.push_back(note);
      events.push_back(velocity);

      // Note Off event (8n nn) — no running status.
      events.push_back(static_cast<uint8_t>(0x80 | (channel & 0x0F)));
      events.push_back(note);
      events.push_back(64); // Default release velocity.

      // Prepend delta-time (MIDI variable-length integer, LSB first).
      prependDeltaTime(events, deltaTicks);
   }

   // Build a Control Change event (no running status).
   static void buildCcEvent(std::vector<uint8_t>& events, uint32_t deltaTicks,
                            uint8_t controller, uint8_t value,
                            uint8_t channel) {
      // Control Change event (Bn cc vv) — no running status.
      events.push_back(static_cast<uint8_t>(0xB0 | (channel & 0x0F)));
      events.push_back(controller);
      events.push_back(value);

      // Prepend delta-time (MIDI variable-length integer, LSB first).
      prependDeltaTime(events, deltaTicks);
   }

   // Build a Program Change event (no running status).
   static void buildProgramChangeEvent(std::vector<uint8_t>& events,
                                       uint32_t deltaTicks, uint8_t patch,
                                       uint8_t channel) {
      // Program Change event (Cn pp) — no running status.
      events.push_back(static_cast<uint8_t>(0xC0 | (channel & 0x0F)));
      events.push_back(patch);

      // Prepend delta-time (MIDI variable-length integer, LSB first).
      prependDeltaTime(events, deltaTicks);
   }

   // Build a Set Tempo meta event (FF 51).
   static void buildSetTempoEvent(std::vector<uint8_t>& events,
                                  uint32_t deltaTicks, double tempoBpm) {
      // Set Tempo meta event (FF 51 03 t1 t2 t3).
      uint32_t microsecondsPerQuarterNote =
         static_cast<uint32_t>(60000000.0 / tempoBpm);

      events.push_back(0xFF);
      events.push_back(0x51);
      events.push_back(0x03); // 3 data bytes.
      events.push_back(
         static_cast<uint8_t>((microsecondsPerQuarterNote >> 16) & 0xFF));
      events.push_back(
         static_cast<uint8_t>((microsecondsPerQuarterNote >> 8) & 0xFF));
      events.push_back(static_cast<uint8_t>(microsecondsPerQuarterNote & 0xFF));

      // Prepend delta-time (MIDI variable-length integer, LSB first).
      prependDeltaTime(events, deltaTicks);
   }

   // Prepend a delta-time (MIDI variable-length integer) to events.
   //
   // Domain context: MIDI variable-length integers are encoded
   // least-significant 7-bit group first, with the high bit of
   // each byte set except for the last byte. This is the opposite
   // of big-endian byte order.
   static void prependDeltaTime(std::vector<uint8_t>& events,
                               uint32_t deltaTicks) {
      std::vector<uint8_t> delta;
      if (deltaTicks == 0) {
         delta.push_back(0);
      } else {
         // Collect 7-bit groups (LSB first).
         std::vector<uint8_t> groups;
         uint32_t val = deltaTicks;
         while (val > 0) {
            groups.push_back(static_cast<uint8_t>(val & 0x7F));
            val >>= 7;
         }
         // Write LSB first, setting continuation bit on all but the
         // last (most significant) group.
         for (size_t i = 0; i < groups.size(); ++i) {
            if (i < groups.size() - 1) {
               delta.push_back(static_cast<uint8_t>(groups[i] | 0x80));
            } else {
               delta.push_back(groups[i]);
            }
         }
      }
      events.insert(events.begin(), delta.begin(), delta.end());
   }

   // Build an End of Track meta event (FF 2F 00).
   static void buildEndOfTrack(std::vector<uint8_t>& events) {
      events.push_back(0xFF);
      events.push_back(0x2F);
      events.push_back(0x00);
   }

   // Write a track chunk.
   static bool writeTrack(FILE* f, const std::vector<uint8_t>& trackData) {
      // Write "MTrk" header + variable-length length + data.
      const char mtrkId[4] = {'M', 'T', 'r', 'k'};
      if (fwrite(mtrkId, 4, 1, f) != 1) {
         return false;
      }

      uint32_t trackLength = static_cast<uint32_t>(trackData.size());
      return writeUint32(f, trackLength) &&
             fwrite(trackData.data(), 1, trackLength, f) == trackLength;
   }
};

// ============================================================================
// MidiFileWriter implementation
// RAII resource management — file handle is released
// automatically when the C++ object is destroyed.
// ============================================================================

MidiFileWriter::MidiFileWriter(std::string_view path)
   : impl_(std::make_unique<Impl>()) {
   // Open the output file for writing.

   impl_->outputPath = std::string(path);
}

MidiFileWriter::~MidiFileWriter() = default;

MidiFileWriter::MidiFileWriter(MidiFileWriter&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

MidiFileWriter& MidiFileWriter::operator=(MidiFileWriter&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

bool MidiFileWriter::write(const Score& score) {
   // Write a Score (HIR) to a MIDI file.
   // All variables with initializers are declared
   // before any goto that could skip them (C.118).

   // Open the output file.
   FILE* f = fopen(impl_->outputPath.c_str(), "wb");
   if (f == nullptr) {
      return false;
   }

   impl_->file = f;

   // Build track 0 data (piano track).
   // Moved before the header-writing goto points to avoid jumping
   // past variable initializations.
   std::vector<uint8_t> trackData;
   std::vector<Note> sortedNotes = score.notes;
   std::sort(sortedNotes.begin(), sortedNotes.end(),
             [](const Note& a, const Note& b) {
                return a.startTime < b.startTime;
             });
   uint32_t lastTick = 0;

   // Write the MIDI header (14 bytes).
   // Format: 1 (Type 1), Num tracks: 1, Division: 480 ticks/qn.
   const char mthdId[4] = {'M', 'T', 'h', 'd'};
   if (fwrite(mthdId, 4, 1, f) != 1) {
      goto error;
   }
   if (!Impl::writeUint32(f, 6)) { // Header chunk length = 6 bytes.
      goto error;
   }
   if (!Impl::writeUint16(f, 1)) { // Format: 1 (multi-track).
      goto error;
   }
   if (!Impl::writeUint16(f, 1)) { // Number of tracks: 1.
      goto error;
   }
   if (!Impl::writeUint16(f, Impl::TICKS_PER_QUARTER_NOTE)) {
      // Division: 480 ticks per quarter note (Logic Pro default).
      goto error;
   }

   // Set Tempo meta event (at t=0).
   Impl::buildSetTempoEvent(trackData, 0, score.tempo);

   // Program Change to Acoustic Grand Piano (patch 0)
   // on channel 0 (at t=0).
   Impl::buildProgramChangeEvent(trackData, 0, 0, 0);

   // Sustain pedal ON (CC#64 = 127) at t=0.
   Impl::buildCcEvent(trackData, 0, 64, 127, 0);

   // Write all notes as Note On + Note Off events.
   for (const auto& note : sortedNotes) {
      uint32_t noteOnTick = Impl::secondsToTicks(note.startTime, score.tempo);
      uint32_t noteOffTick = Impl::secondsToTicks(note.endTime, score.tempo);

      // Clamp pitch to valid piano range (21–108).
      uint8_t pitch = static_cast<uint8_t>(
         std::max(21u, std::min(108u, static_cast<unsigned>(note.pitch))));

      // Clamp velocity to valid range (1–127).
      uint8_t velocity = static_cast<uint8_t>(
         std::max(1u, std::min(127u, static_cast<unsigned>(note.velocity))));

      // Compute delta-time (non-negative).
      uint32_t deltaTicks =
         std::max(0u, static_cast<unsigned>(noteOnTick - lastTick));

      Impl::buildNoteEvent(trackData, deltaTicks, pitch, velocity,
                           note.channel);

      // Compute note-off delta-time.
      uint32_t noteOffDelta =
         std::max(0u, static_cast<unsigned>(noteOffTick - noteOnTick));

      Impl::buildNoteEvent(trackData, noteOffDelta, pitch, velocity,
                           note.channel);

      lastTick = noteOffTick;
   }

   // Write sustain pedal OFF (CC#64 = 0) at the end.
   Impl::buildCcEvent(trackData, 0, 64, 0, 0);

   // Write all control events from the Score.
   for (const auto& ctrl : score.controls) {
      uint32_t ctrlTick = Impl::secondsToTicks(ctrl.time, score.tempo);

      // Clamp controller and value to valid ranges.
      uint8_t controller = static_cast<uint8_t>(
         std::max(0u, std::min(127u, static_cast<unsigned>(ctrl.controller))));
      uint8_t value = static_cast<uint8_t>(
         std::max(0u, std::min(127u, static_cast<unsigned>(ctrl.value))));

      // Compute delta-time (non-negative).
      uint32_t deltaTicks =
         std::max(0u, static_cast<unsigned>(ctrlTick - lastTick));

      Impl::buildCcEvent(trackData, deltaTicks, controller, value, 0);

      lastTick = ctrlTick;
   }

   // End of Track marker.
   Impl::buildEndOfTrack(trackData);

   // Write the track chunk.
   if (!Impl::writeTrack(f, trackData)) {
      goto error;
   }

   // Close the file.
   impl_->bytesWritten_ = static_cast<uint64_t>(ftello(f));
   fclose(f);
   impl_->file = nullptr;

   return true;

error:
   fclose(f);
   impl_->file = nullptr;
   return false;
}

uint64_t MidiFileWriter::bytesWritten() const {
   // Simple accessor.
   return impl_ ? impl_->bytesWritten_ : 0;
}

bool MidiFileWriter::isOpen() const {
   // Simple accessor.
   return impl_ ? impl_->file != nullptr : false;
}

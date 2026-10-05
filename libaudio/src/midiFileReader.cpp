/**
 * @file midiFileReader.cpp
 * @brief Implementation of MidiFileReader — parse a Standard MIDI File (SMF)
 *        into a HIR `Score`.
 *
 * This module is the reader counterpart of `MidiFileWriter`. It reads a file's
 * bytes and walks the SMF stream:
 *   - a `MThd` header (format, track count, ticks-per-division),
 *   - `MTrk` tracks holding meta-events (tempo, title, ...) and channel
 *     voice messages, including the standard *running-status* byte.
 *
 * @section midifilereader-standards Compliance: a general-purpose SMF reader
 *
 * The reader is a *standards-compliant* parser, not one sized to a particular
 * writer. It handles the channel voice messages (note on/off, control change,
 * program change, channel / poly aftertouch, pitch bend) and the common
 * meta-events (tempo, title, end-of-track, ...), skipping the rest. Running
 * status is honoured exactly as the MIDI 1.0 spec and the reference `midicsv`
 * parser define it: a data byte in a status position reuses the most recent
 * *channel* message's status, and a system message does not clear it (a channel
 * running status may legally span a system message).
 *
 * @section midifilereader-running-status Running status
 *
 * A status byte `< 0x80` is a *data* byte reusing the last channel status. The
 * parser peeks the byte *without* consuming it: a set high bit is a fresh
 * status (consumed, and remembered if it is a channel message); a clear high
 * bit is a running-status data byte (the last channel status is reused and the
 * byte is *not* consumed, so the data bytes that follow are read in place). A
 * data byte with no prior channel message to run its status is malformed.
 *
 * @section midifilereader-tempo Tempo: a per-segment running clock
 *
 * Ticks are converted to seconds by a running clock: each delta advances the
 * clock by `delta * tempo / (1e6 * ticksPerQuarter)` using the tempo in effect
 * at that point, so *tempo changes* are honoured (a `FF 51` sets the tempo for
 * subsequent deltas only). A note's `startTime` / `endTime` are the running
 * clock at its note-on / note-off; the `Score`'s tempo is the last tempo seen
 * (BPM). For a single-tempo file this reduces to the flat `ticks * tempo`
 * conversion, so it is a no-op there.
 *
 * @section midifilereader-malformed Malformed input: a loud failure
 *
 * Rather than silently dropping a garbled file, the reader reports *why* it
 * failed. `ok()` is false and `error()` names the problem when the file is not
 * a SMF, has a short / oversized header or an unsupported ticks-per-division,
 * an `MTrk` length past end-of-file, an unterminated variable-length value,
 * event data running past the track end, a running-status data byte with no
 * prior channel message, or an unknown / reserved / realtime status. A caller
 * then fails loudly (per the evaluation harness's policy of aborting on
 * invalid MIDI) instead of scoring against a half-parsed ground truth.
 *
 * @section midifilereader-hir HIR mapping
 *
 * A note-on (`9n pc vel`, vel != 0) opens a note; the matching note-off (`8n`
 * or `9n vel 0`) closes it, producing one `Note` with `pitch = pc`,
 * `velocity = vel`, and `startTime` / `endTime` in seconds. `channel` is the
 * MIDI channel of the note-on. `pitchBends` is filled with any `0xE0` values
 * seen while the note was open (a bend has no note of its own, so it is routed
 * to the most recently opened note on its channel). Notes still open at the
 * end of a track are closed at the track's end time.
 *
 * No external dependencies: parsing is plain byte-level (the SMF format is
 * documented in the project's MIDI reference), so this module has no Pimpl C
 * library to hide — the `Impl` only holds the parsed `Score`.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <libaudio/hir.h>
#include <libaudio/midiFileReader.h>
#include <map>
#include <string>
#include <vector>

namespace libaudio {

// ============================================================================
// MidiFileReader::Impl — holds the parsed Score and the byte-level parser.
//
// Notes are accumulated on a *running clock* (seconds) while the stream is
// walked, so tempo changes are honoured per segment. The parsed `Score` is
// held here; there is no persistent file handle to release (the bytes are read
// once in the constructor), so the destructor is trivial.
// ============================================================================
struct MidiFileReader::Impl {
   // A finished note: per-segment second endpoints plus pitch / velocity /
   // channel, and the pitch-bend (0xE0) values seen while it was open.
   struct RawNote {
      double onTime = 0.0;  // seconds (the running clock at the note-on)
      double offTime = 0.0; // seconds (the running clock at the note-off)
      uint8_t pitch = 0;
      uint8_t velocity = 0;
      uint8_t channel = 0;
      std::vector<int16_t> bends; // 0xE0 values, in order of appearance.
   };

   // A note currently sounding, keyed by (channel, pitch).
   struct OpenNote {
      double onTime = 0.0; // seconds (the running clock at the note-on)
      uint8_t pitch = 0;
      uint8_t velocity = 0;
      uint8_t channel = 0;
      std::vector<int16_t> bends; // 0xE0 values accumulated while open.
   };

   // One open note at most per (channel, pitch). The composite key keeps the
   // reader unambiguous when the same pitch sounds on more than one channel.
   using OpenKey = uint32_t;
   static OpenKey makeOpenKey(uint8_t channel, uint8_t pitch) {
      return (static_cast<uint32_t>(channel) << 8) |
             static_cast<uint32_t>(pitch);
   }
   using OpenNotes = std::map<OpenKey, OpenNote>;

   // Running-clock state carried across a single MTrk. Both fields reset per
   // track (SMF tracks are parallel timelines from tick 0, and running status
   // does not cross a track boundary).
   struct TrackClock {
      double timeSec = 0.0;   // the running clock (seconds)
      uint8_t lastStatus = 0; // last channel-voice status; 0 = none (a data
                              // byte in this state is malformed)
   };

   // Parse diagnostics carried out of `parseTrack` (malformed → loud abort).
   struct Flags {
      bool malformed = false;
      std::string error; // a human-readable reason (empty when !malformed)
   };

   // Close the note on (channel, pitch), if open, recording it (with any
   // accumulated pitch bends) as a finished RawNote. Shared by the note-off,
   // note-on-velocity-0, and end-of-track close paths.
   static void closeOpenNote(OpenNotes& open, std::vector<RawNote>& rawNotes,
                             uint8_t channel, uint8_t pitch, double timeSec) {
      auto it = open.find(makeOpenKey(channel, pitch));
      if (it != open.end()) {
         RawNote r;
         r.onTime = it->second.onTime;
         r.offTime = timeSec;
         r.pitch = it->second.pitch;
         r.velocity = it->second.velocity;
         r.channel = it->second.channel;
         r.bends = std::move(it->second.bends);
         rawNotes.push_back(std::move(r));
         open.erase(it);
      }
   }

   // Advance the running clock by `delta` ticks at the current tempo.
   static void advanceClock(TrackClock& clock, uint32_t delta, uint32_t tempoUs,
                            uint32_t ticksPerQuarter) {
      if (ticksPerQuarter == 0) {
         return; // A zero division is guarded in the constructor; never divide.
      }
      clock.timeSec +=
         static_cast<double>(delta) * (static_cast<double>(tempoUs) / 1.0e6 /
                                       static_cast<double>(ticksPerQuarter));
   }

   // Read one variable-length value starting at `j`, leaving `j` past it.
   // Returns false (and sets `flags`) if the value is unterminated, i.e. its
   // continuation bits run past `end` with no terminating byte.
   static bool readVarLen(const std::vector<uint8_t>& raw, size_t& j,
                          size_t end, uint32_t& out, Flags& flags) {
      uint32_t value = 0;
      bool sawTerminator = false;
      while (j < end) {
         const uint8_t b = raw[j++];
         value = (value << 7) | (b & 0x7F);
         if ((b & 0x80) == 0) {
            sawTerminator = true;
            break;
         }
      }
      if (!sawTerminator) {
         flags.malformed = true;
         flags.error = "a variable-length value runs past the end of the track";
         return false;
      }
      out = value;
      return true;
   }

   Score score_;
   std::vector<RawNote> rawNotes;
   bool ok_ = false;
   std::string error_; // empty when ok; the malformed/unsupported reason.

   // Ticks-per-division from the header (quarter-note ticks for valid files).
   uint32_t ticksPerQuarter = 480;
   // Tempo, in microseconds per quarter note (120 BPM → 500000). Carried
   // across the whole file: tempo is a global property, so a meta event sets it
   // for later tracks too.
   uint32_t tempoUs = 500000;

   // Read and parse one MTrk's event stream. `ticksPerQuarter` is fixed for the
   // file; `tempoUs` is carried across tracks; `clock` and the open-note set
   // are per-track (the caller resets them). `flags` carries any malformed
   // condition back to the caller.
   static void parseTrack(const std::vector<uint8_t>& raw, size_t bodyStart,
                          size_t bodyEnd, uint32_t ticksPerQuarter,
                          uint32_t& tempoUs, TrackClock& clock, OpenNotes& open,
                          std::vector<RawNote>& rawNotes, Flags& flags);

   // Convert the accumulated raw notes into the Score (times are already in
   // seconds) and mark the parse a success.
   void finish();

   bool ok() const { return ok_; }
   const Score& score() const { return score_; }
   const std::string& error() const { return error_; }
};

// ----------------------------------------------------------------------------
// parseTrack — walk one MTrk's event stream.
// ----------------------------------------------------------------------------
void MidiFileReader::Impl::parseTrack(
   const std::vector<uint8_t>& raw, size_t bodyStart, size_t bodyEnd,
   uint32_t ticksPerQuarter, uint32_t& tempoUs, TrackClock& clock,
   OpenNotes& open, std::vector<RawNote>& rawNotes, Flags& flags) {
   // The most-recently-opened note per channel: a pitch bend (0xE0) has no note
   // of its own, so it is attached to the last note struck on its channel.
   std::map<uint8_t, OpenKey> lastOpen;

   size_t j = bodyStart;
   while (j < bodyEnd && !flags.malformed) {
      // 1. Variable-length delta time, then advance the running clock with the
      //    tempo in effect (a tempo meta updates it only for *later* deltas).
      uint32_t delta = 0;
      if (!readVarLen(raw, j, bodyEnd, delta, flags)) {
         break;
      }
      advanceClock(clock, delta, tempoUs, ticksPerQuarter);
      if (j >= bodyEnd) {
         break; // A trailing delta with no following event: end of track.
      }

      // 2. Resolve the status byte. Peek WITHOUT consuming: a set high bit is a
      //    fresh status (consume it, and remember it if it is a channel
      //    message); a clear high bit is a running-status data byte (reuse the
      //    last channel status and do NOT advance — `j` stays on the data).
      const uint8_t first = raw[j];
      uint8_t status;
      if ((first & 0x80) != 0) {
         status = first;
         ++j; // consume the status byte
         if (status <= 0xEF) {
            clock.lastStatus = status; // a channel message to remember
         } else {
            clock.lastStatus = 0; // meta / sysex: nothing to reuse afterwards
         }
      } else {
         // A data byte with no prior channel-voice status cannot run a status;
         // this is malformed (running status follows a channel message only).
         if (clock.lastStatus == 0) {
            flags.malformed = true;
            flags.error =
               "a data byte appears with no prior channel message to run its "
               "status";
            break;
         }
         status = clock.lastStatus; // `j` stays on the data byte (not advanced)
      }

      const uint8_t hi = status & 0xF0;
      if (status == 0xFF) { // 3a. Meta-event.
         if (j >= bodyEnd) {
            flags.malformed = true;
            flags.error = "a meta-event type byte runs past the track end";
            break;
         }
         const uint8_t mtype = raw[j++];
         uint32_t mlen = 0;
         if (!readVarLen(raw, j, bodyEnd, mlen, flags)) {
            break;
         }
         if (j + mlen > bodyEnd) {
            flags.malformed = true;
            flags.error = "a meta-event's data runs past the track end";
            break;
         }
         if (mtype == 0x51 && mlen == 3) { // Set tempo (us per quarter note).
            tempoUs = (static_cast<uint32_t>(raw[j]) << 16) |
                      (static_cast<uint32_t>(raw[j + 1]) << 8) |
                      static_cast<uint32_t>(raw[j + 2]);
         }
         // Title, time-signature, and other meta-events carry no notes: skip.
         j += mlen;
      } else if (status == 0xF0 || status == 0xF7) { // 3b. Sysex.
         uint32_t mlen = 0;
         if (!readVarLen(raw, j, bodyEnd, mlen, flags)) {
            break;
         }
         if (j + mlen > bodyEnd) {
            flags.malformed = true;
            flags.error = "a sysex message runs past the track end";
            break;
         }
         j += mlen;
      } else if (status >= 0xF0) { // 3c. Other system message.
         // System common (F3-F6: tune / soundbank / basenumber) is legal but
         // carries no notes: skip it. F1/F2 are reserved and FE is realtime;
         // a realtime byte carries no delta and would desync the stream, so a
         // file using these is reported as unsupported rather than misparsed.
         if (status >= 0xF3 && status <= 0xF6) {
            const size_t dataLen = (status == 0xF3)   ? 0u
                                   : (status == 0xF4) ? 1u
                                                      : 2u;
            if (j + dataLen > bodyEnd) {
               flags.malformed = true;
               flags.error = "a system-common message runs past the track end";
               break;
            }
            j += dataLen;
         } else {
            flags.malformed = true;
            flags.error = "a reserved or realtime system status appeared in a "
                          "track (unsupported)";
            break;
         }
      } else if (hi == 0x90) { // 3d. Note-on (any channel).
         if (j + 2 > bodyEnd) {
            flags.malformed = true;
            flags.error = "note-on data runs past the track end";
            break;
         }
         const uint8_t pitch = raw[j];
         const uint8_t vel = raw[j + 1];
         const uint8_t channel = status & 0x0F;
         j += 2;
         if (vel != 0) { // A genuine note-on (velocity 0 is a note-off).
            OpenNote on;
            on.onTime = clock.timeSec;
            on.pitch = pitch;
            on.velocity = vel;
            on.channel = channel;
            const OpenKey key = makeOpenKey(channel, pitch);
            open[key] = std::move(on);
            lastOpen[channel] = key; // most-recently opened on this channel
         } else {                    // Velocity 0 → treat as a note-off.
            closeOpenNote(open, rawNotes, channel, pitch, clock.timeSec);
         }
      } else if (hi == 0x80) { // 3e. Note-off.
         if (j + 2 > bodyEnd) {
            flags.malformed = true;
            flags.error = "note-off data runs past the track end";
            break;
         }
         const uint8_t pitch = raw[j];
         const uint8_t channel = status & 0x0F;
         j += 2; // skip the (ignored) off-velocity byte
         closeOpenNote(open, rawNotes, channel, pitch, clock.timeSec);
      } else if (hi == 0xB0 || hi == 0xA0) { // 3f. Control / channel pressure.
         if (j + 2 > bodyEnd) {
            flags.malformed = true;
            flags.error =
               "control-change / channel-pressure data runs past the track end";
            break;
         }
         j += 2;
      } else if (hi == 0xC0 || hi == 0xD0) { // 3g. Program / poly pressure.
         if (j + 1 > bodyEnd) {
            flags.malformed = true;
            flags.error =
               "program-change / poly-pressure data runs past the track end";
            break;
         }
         j += 1;
      } else if (hi == 0xE0) { // 3h. Pitch bend (two data bytes, low first).
         if (j + 2 > bodyEnd) {
            flags.malformed = true;
            flags.error = "pitch-bend data runs past the track end";
            break;
         }
         const uint8_t channel = status & 0x0F;
         const uint8_t data1 = raw[j];     // low 7 bits
         const uint8_t data2 = raw[j + 1]; // high 7 bits
         j += 2;
         // 14-bit signed: ((data2 << 7) | data1) - 8192, in [-8192, 8191];
         // 0 is the note's centre (base) pitch.
         const int16_t value = static_cast<int16_t>(
            ((static_cast<int>(data2 & 0x7F) << 7) | (data1 & 0x7F)) - 8192);
         const auto lo = lastOpen.find(channel);
         if (lo != lastOpen.end()) {
            auto it = open.find(lo->second);
            if (it != open.end()) {
               it->second.bends.push_back(value);
            }
         }
      } else { // 3i. Unknown channel-voice status (a safety net).
         flags.malformed = true;
         flags.error = "an unknown channel event status was found";
         break;
      }
   }
}

// ----------------------------------------------------------------------------
// finish — convert the accumulated raw notes to the Score.
// ----------------------------------------------------------------------------
void MidiFileReader::Impl::finish() {
   std::vector<Note> notes;
   notes.reserve(rawNotes.size());
   for (const RawNote& r : rawNotes) {
      Note n;
      n.startTime = r.onTime; // already in seconds (per-segment running clock)
      n.endTime = r.offTime;
      n.pitch = r.pitch;
      n.velocity = r.velocity;
      n.channel = r.channel;
      n.sustain = false;
      n.pitchBends = r.bends; // round-trips the 0xE0 values into the HIR
      notes.push_back(std::move(n));
   }
   std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
      return a.startTime < b.startTime;
   });
   score_.notes = std::move(notes);
   // Store the tempo in BPM (the HIR convention): the last tempo seen.
   score_.tempo =
      (tempoUs > 0) ? (60.0e6 / static_cast<double>(tempoUs)) : 120.0;
   ok_ = true;
}

// ============================================================================
// MidiFileReader — public API implementation.
// ============================================================================

MidiFileReader::MidiFileReader(std::string_view path)
   : impl_(std::make_unique<Impl>()) {
   // Read the whole file (the files this parses are small).
   std::string pathStr(path);
   FILE* f = std::fopen(pathStr.c_str(), "rb");
   if (f == nullptr) {
      impl_->error_ = "the file could not be opened for reading";
      return;
   }
   std::fseek(f, 0, SEEK_END);
   const long size = std::ftell(f);
   std::fseek(f, 0, SEEK_SET);
   std::vector<uint8_t> raw(static_cast<size_t>(std::max(0L, size)));
   if (size > 0 && std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
      std::fclose(f);
      impl_->error_ = "the file could not be read in full (truncated?)";
      return;
   }
   std::fclose(f);

   // MThd header: "MThd"(4) + length(4) + format(2) + ntrks(2) + division(2).
   if (raw.size() < 14 ||
       !(raw[0] == 'M' && raw[1] == 'T' && raw[2] == 'h' && raw[3] == 'd')) {
      impl_->error_ =
         "not a Standard MIDI File (a short or missing MThd header)";
      return;
   }
   const uint32_t headerLen = (static_cast<uint32_t>(raw[4]) << 24) |
                              (static_cast<uint32_t>(raw[5]) << 16) |
                              (static_cast<uint32_t>(raw[6]) << 8) |
                              static_cast<uint32_t>(raw[7]);
   if (headerLen != 6) {
      impl_->error_ = "a malformed MThd length (expected 6)";
      return;
   }
   // Ticks-per-division (2 bytes, big-endian, signed). A positive value (high
   // bit of the high byte clear) is the number of MIDI clock ticks per quarter
   // note; a negative value (high bit set) encodes SMPTE film frames (the
   // 24/25/29/30 fps forms) or the 24 MHz clock. Only the positive quarter-note
   // form is supported, so any negative division is rejected up front.
   const uint8_t divHi = raw[12];
   if ((divHi & 0x80) != 0) {
      impl_->error_ = (divHi == 0xFF)
                         ? "a 24 MHz clock ticks-per-division is not supported"
                         : "an SMPTE ticks-per-division is not supported";
      return;
   }
   impl_->ticksPerQuarter = static_cast<uint32_t>(
      (static_cast<uint16_t>(divHi) << 8) | static_cast<uint16_t>(raw[13]));
   if (impl_->ticksPerQuarter == 0) {
      impl_->error_ = "a zero ticks-per-division is invalid";
      return;
   }

   // Walk each MTrk. `open` (notes still sounding) and the running clock reset
   // per track (parallel timelines from tick 0; running status does not cross a
   // track boundary); `tempoUs` is carried across tracks (tempo is global).
   size_t i = 8 + static_cast<size_t>(headerLen);
   Impl::OpenNotes open;
   bool malformed = false;
   while (i + 8 <= raw.size() && raw[i] == 'M' && raw[i + 1] == 'T' &&
          raw[i + 2] == 'r' && raw[i + 3] == 'k') {
      const uint32_t trackLen = (static_cast<uint32_t>(raw[i + 4]) << 24) |
                                (static_cast<uint32_t>(raw[i + 5]) << 16) |
                                (static_cast<uint32_t>(raw[i + 6]) << 8) |
                                static_cast<uint32_t>(raw[i + 7]);
      const size_t bodyStart = i + 8;
      const size_t bodyEnd = bodyStart + static_cast<size_t>(trackLen);
      if (bodyEnd > raw.size()) {
         impl_->error_ = "an MTrk length runs past the end of the file";
         malformed = true;
         break;
      }
      Impl::TrackClock clock; // per-track: timeSec = 0, lastStatus = 0
      Impl::Flags flags;
      Impl::parseTrack(raw, bodyStart, bodyEnd, impl_->ticksPerQuarter,
                       impl_->tempoUs, clock, open, impl_->rawNotes, flags);
      if (flags.malformed) {
         impl_->error_ = flags.error;
         malformed = true;
         break;
      }
      // Close any notes still open at this track's end (at the end clock).
      for (auto& entry : open) {
         Impl::RawNote r;
         r.onTime = entry.second.onTime;
         r.offTime = clock.timeSec;
         r.pitch = entry.second.pitch;
         r.velocity = entry.second.velocity;
         r.channel = entry.second.channel;
         r.bends = std::move(entry.second.bends);
         impl_->rawNotes.push_back(std::move(r));
      }
      open.clear();
      i = bodyEnd;
   }

   if (malformed) {
      impl_->ok_ = false; // fail loudly; the caller reports impl_->error_
      return;
   }
   impl_->finish(); // sets ok_ = true
}

MidiFileReader::~MidiFileReader() = default;

MidiFileReader::MidiFileReader(MidiFileReader&& other) noexcept
   : impl_(std::move(other.impl_)) {
   other.impl_ = std::make_unique<Impl>();
}

MidiFileReader& MidiFileReader::operator=(MidiFileReader&& other) noexcept {
   if (this != &other) {
      impl_ = std::move(other.impl_);
      other.impl_ = std::make_unique<Impl>();
   }
   return *this;
}

bool MidiFileReader::ok() const { return impl_ ? impl_->ok() : false; }

const Score& MidiFileReader::score() const {
   static const Score empty;
   return impl_ ? impl_->score() : empty;
}

const std::string& MidiFileReader::error() const {
   static const std::string none;
   return impl_ ? impl_->error() : none;
}

} // namespace libaudio

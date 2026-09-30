/**
 * @file midiFileReader.cpp
 * @brief Implementation of MidiFileReader — parse a Standard MIDI File (SMF)
 *        into a HIR `Score`.
 *
 * This module is the reader counterpart of `MidiFileWriter`. It reads a file's
 * bytes and walks the SMF stream:
 *   - a `MThd` header (ticks-per-division),
 *   - `MTrk` tracks holding tempo / title meta-events and a run of channel
 *     note-on / note-off messages (with the standard running-status byte).
 *
 * Every note is accumulated in MIDI *ticks*; the file's final tempo and the
 * header's ticks-per-division convert those ticks to seconds, matching the HIR
 * convention (`Note.startTime` / `endTime` in seconds). The parsed `Score` is
 * held by the `Impl`; there is no persistent file handle to release (the bytes
 * are read once in the constructor), so the destructor is trivial.
 *
 * @section midifilereader-running-status Running status
 *
 * A status byte `< 0x80` is a *data* byte that reuses the most recent channel
 * message's status (a shortcoming of the SMF grammar). The parser tracks that
 * last channel status and re-uses it, so it stays correct even for a writer
 * that omits a repeated status byte (our writer always emits one, but the
 * reader should not depend on that).
 *
 * @section midifilereader-tempo Tempo: the final value, applied everywhere
 *
 * The parser remembers the *last* tempo meta-event it sees and applies that
 * single tempo to every note (in seconds). A note still open at the end of a
 * track is closed at the track's end time. This mirrors the evaluation
 * harness's ground-truth reader, which the transcription corpus depends on.
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
// A note is accumulated in MIDI ticks while the stream is walked, then
// converted to seconds at the end (once the final tempo is known). This
// matches the evaluation harness, which applies the file's final tempo to the
// whole track.
// ============================================================================
struct MidiFileReader::Impl {
   // A note seen in the file: tick-level endpoints plus pitch / velocity /
   // channel, and the pitch-bend (0xE0) values in order of appearance.
   // Converted to seconds in `finish()`.
   struct RawNote {
      uint32_t onTick = 0;
      uint32_t offTick = 0;
      uint8_t pitch = 0;
      uint8_t velocity = 0;
      uint8_t channel = 0;
      std::vector<int16_t> bends; // 0xE0 values (MIDI ticks), in order.
   };

   // A note currently open (sounding), keyed by (channel, pitch).
   struct OpenNote {
      uint32_t onTick = 0;
      uint8_t pitch = 0;
      uint8_t velocity = 0;
      uint8_t channel = 0;
      std::vector<int16_t> bends; // 0xE0 values accumulated while the note is
                                  // open. Each 0xE0 is routed to the most
                                  // recently opened note on its channel.
   };

   // One open note at most per (channel, pitch). The composite key keeps the
   // reader unambiguous when the same pitch sounds on more than one channel
   // (the writer does this for overlapping bent notes).
   using OpenKey = uint32_t;
   static OpenKey makeOpenKey(uint8_t channel, uint8_t pitch) {
      return (static_cast<uint32_t>(channel) << 8) | pitch;
   }

   // Open notes keyed by (channel, pitch) (the SMF grammar allows the same
   // pitch to be re-struck on a channel while a prior note is still ringing).
   using OpenNotes = std::map<OpenKey, OpenNote>;

   // Close the note on (channel, pitch), if open, recording it (with any
   // accumulated pitch bends) as a finished RawNote. Shared by the note-off,
   // note-on-vel-0, and end-of-track close paths.
   static void closeOpenNote(OpenNotes& open, std::vector<RawNote>& rawNotes,
                             uint8_t channel, uint8_t pitch, uint32_t tick) {
      auto it = open.find(makeOpenKey(channel, pitch));
      if (it != open.end()) {
         RawNote r;
         r.onTick = it->second.onTick;
         r.offTick = tick;
         r.pitch = it->second.pitch;
         r.velocity = it->second.velocity;
         r.channel = it->second.channel;
         r.bends = std::move(it->second.bends);
         rawNotes.push_back(std::move(r));
         open.erase(it);
      }
   }

   Score score_;
   std::vector<RawNote> rawNotes;
   bool ok_ = false;

   // Ticks-per-division from the header (quarter-note ticks for our files).
   uint16_t ticksPerQuarter = 480;
   // Final tempo, in microseconds per quarter note (120 BPM → 500000).
   uint32_t tempoUs = 500000;

   // Read and parse one MTrk's event stream. `tick`, `lastStatus`, `tempoUs`
   // and `open` are carried across the call so a caller keeps a running
   // position and open-note state (SMF has no per-track reset guarantee).
   static void parseTrack(const std::vector<uint8_t>& raw, size_t bodyStart,
                          size_t bodyEnd, uint32_t& tick, uint8_t& lastStatus,
                          uint32_t& tempoUs, OpenNotes& open,
                          std::vector<RawNote>& rawNotes) {
      size_t j = bodyStart;
      // The most-recently-opened note per channel: a pitch bend (0xE0) has no
      // note of its own, so it is attached to the last note struck on its
      // channel. Local to this track so a fresh track starts with none.
      std::map<uint8_t, OpenKey> lastOpen;
      while (j < bodyEnd) {
         // Variable-length delta time (7 bits per byte, high bit = continue).
         uint32_t delta = 0;
         while (j < bodyEnd) {
            const uint8_t b = raw[j++];
            delta = (delta << 7) | (b & 0x7F);
            if ((b & 0x80) == 0) {
               break;
            }
         }
         tick += delta;
         if (j >= bodyEnd) {
            break;
         }

         uint8_t status = raw[j++];
         if (status < 0x80) {
            // Running status: a data byte reusing the last channel status.
            if (lastStatus == 0) {
               break; // No prior channel message to reuse — bail.
            }
            status = lastStatus;
         } else if (status <= 0xEF) {
            lastStatus = status; // A channel message to remember.
         } else {
            lastStatus = 0; // Meta / sysex: nothing to reuse afterwards.
         }

         const uint8_t hi = status & 0xF0;
         if (status == 0xFF) { // Meta-event.
            if (j >= bodyEnd) {
               break;
            }
            const uint8_t mtype = raw[j++];
            uint32_t mlen = 0;
            while (j < bodyEnd) {
               const uint8_t b = raw[j++];
               mlen = (mlen << 7) | (b & 0x7F);
               if ((b & 0x80) == 0) {
                  break;
               }
            }
            if (mtype == 0x51 && mlen == 3) { // Set tempo (us / quarter).
               tempoUs = (static_cast<uint32_t>(raw[j]) << 16) |
                         (static_cast<uint32_t>(raw[j + 1]) << 8) |
                         static_cast<uint32_t>(raw[j + 2]);
            }
            // Title (0x04) and other meta-events carry no notes: skip.
            j += mlen;
         } else if (status == 0xF0 || status == 0xF7) { // Sysex.
            uint32_t mlen = 0;
            while (j < bodyEnd) {
               const uint8_t b = raw[j++];
               mlen = (mlen << 7) | (b & 0x7F);
               if ((b & 0x80) == 0) {
                  break;
               }
            }
            j += mlen;
         } else if (hi == 0x90) { // Note-on (any channel).
            if (j + 2 > bodyEnd) {
               break;
            }
            const uint8_t pitch = raw[j];
            const uint8_t vel = raw[j + 1];
            const uint8_t channel = status & 0x0F;
            j += 2;
            if (vel != 0) { // A genuine note-on (vel 0 is a note-off).
               OpenNote on;
               on.onTick = tick;
               on.pitch = pitch;
               on.velocity = vel;
               on.channel = channel;
               const OpenKey key = makeOpenKey(channel, pitch);
               open[key] = std::move(on);
               lastOpen[channel] = key; // most-recently opened on this channel
            } else {                    // vel == 0 → a note-off.
               closeOpenNote(open, rawNotes, channel, pitch, tick);
            }
         } else if (hi == 0x80) { // Note-off (pitch + velocity).
            if (j + 2 > bodyEnd) {
               break;
            }
            const uint8_t pitch = raw[j];
            const uint8_t channel = status & 0x0F;
            j += 2; // Skip the (ignored) velocity byte.
            closeOpenNote(open, rawNotes, channel, pitch, tick);
         } else if (hi == 0xB0 || hi == 0xA0) { // Control / ch aftertouch.
            j += 2;
         } else if (hi == 0xC0 || hi == 0xD0) { // Program / poly aftertouch.
            j += 1;
         } else if (hi == 0xE0) { // Pitch bend (two data bytes, low first).
            if (j + 2 > bodyEnd) {
               break;
            }
            const uint8_t channel = status & 0x0F;
            const uint8_t data1 = raw[j];     // low 7 bits
            const uint8_t data2 = raw[j + 1]; // high 7 bits
            j += 2;
            // 14-bit signed value: ((data2 << 7) | data1) - 8192,
            // range [-8192, 8191]; 0 is the note's centre pitch.
            const int16_t value = static_cast<int16_t>(
               ((static_cast<int>(data2 & 0x7F) << 7) | (data1 & 0x7F)) - 8192);
            // Attach to the most-recently-opened note on this channel, if any.
            const auto lo = lastOpen.find(channel);
            if (lo != lastOpen.end()) {
               auto it = open.find(lo->second);
               if (it != open.end()) {
                  it->second.bends.push_back(value);
               }
            }
         } else {
            break; // Unknown — stop rather than misparse.
         }
      }
   }

   // Convert accumulated raw notes to seconds and finish the Score.
   void finish() {
      if (ticksPerQuarter == 0) {
         ticksPerQuarter = 1; // A 0 division is invalid; treat as 1.
      }
      // Seconds per tick for the file's (final) tempo.
      const double ticksToSec = static_cast<double>(tempoUs) / 1.0e6 /
                                static_cast<double>(ticksPerQuarter);

      std::vector<Note> notes;
      notes.reserve(rawNotes.size());
      for (const RawNote& r : rawNotes) {
         Note n;
         n.startTime = static_cast<double>(r.onTick) * ticksToSec;
         n.endTime = static_cast<double>(r.offTick) * ticksToSec;
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
      // Store the tempo in BPM (the HIR convention).
      score_.tempo =
         (tempoUs > 0) ? (60.0e6 / static_cast<double>(tempoUs)) : 120.0;
      ok_ = true;
   }

   bool ok() const { return ok_; }
   const Score& score() const { return score_; }
};

// ============================================================================
// MidiFileReader — public API implementation.
// ============================================================================

MidiFileReader::MidiFileReader(std::string_view path)
   : impl_(std::make_unique<Impl>()) {
   // Read the whole file (our test files are small).
   std::string pathStr(path);
   FILE* f = std::fopen(pathStr.c_str(), "rb");
   if (f == nullptr) {
      return; // Unreadable: ok() stays false, score() stays empty.
   }
   std::fseek(f, 0, SEEK_END);
   const long size = std::ftell(f);
   std::fseek(f, 0, SEEK_SET);
   std::vector<uint8_t> raw(static_cast<size_t>(std::max(0L, size)));
   if (size > 0 && std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
      std::fclose(f);
      return; // Truncated / partial read: treat as a failure.
   }
   std::fclose(f);

   // Header: MThd(4) + length(4) + format(2) + ntrks(2) + division(2).
   if (raw.size() < 14 ||
       !(raw[0] == 'M' && raw[1] == 'T' && raw[2] == 'h' && raw[3] == 'd')) {
      return; // Not an SMF.
   }
   const uint32_t headerLen = (static_cast<uint32_t>(raw[4]) << 24) |
                              (static_cast<uint32_t>(raw[5]) << 16) |
                              (static_cast<uint32_t>(raw[6]) << 8) |
                              static_cast<uint32_t>(raw[7]);
   // Ticks-per-division sits at offset 12 (2 bytes, big-endian).
   impl_->ticksPerQuarter =
      static_cast<uint16_t>((static_cast<uint16_t>(raw[12]) << 8) | raw[13]);

   // Walk each MTrk. `open` (notes still sounding) is shared across tracks and
   // is closed at each track's end, matching the evaluation harness.
   size_t i = 8 + static_cast<size_t>(headerLen);
   Impl::OpenNotes open;
   uint32_t tick = 0;
   uint8_t lastStatus = 0;
   while (i + 8 <= raw.size() && raw[i] == 'M' && raw[i + 1] == 'T' &&
          raw[i + 2] == 'r' && raw[i + 3] == 'k') {
      const uint32_t trackLen = (static_cast<uint32_t>(raw[i + 4]) << 24) |
                                (static_cast<uint32_t>(raw[i + 5]) << 16) |
                                (static_cast<uint32_t>(raw[i + 6]) << 8) |
                                static_cast<uint32_t>(raw[i + 7]);
      const size_t bodyStart = i + 8;
      const size_t bodyEnd =
         std::min(raw.size(), bodyStart + static_cast<size_t>(trackLen));
      Impl::parseTrack(raw, bodyStart, bodyEnd, tick, lastStatus,
                       impl_->tempoUs, open, impl_->rawNotes);
      // Close any notes still open at this track's end.
      for (const auto& [key, o] : open) {
         Impl::RawNote r;
         r.onTick = o.onTick;
         r.offTick = tick;
         r.pitch = o.pitch;
         r.velocity = o.velocity;
         r.channel = o.channel;
         r.bends = std::move(o.bends);
         impl_->rawNotes.push_back(std::move(r));
      }
      open.clear();
      i = bodyEnd;
   }

   impl_->finish();
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

} // namespace libaudio

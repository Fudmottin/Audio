/**
 * @file midiFileWriter.cpp
 * @brief Implementation of MidiFileWriter — write a Score (HIR) to a
 *        Standard MIDI File (Type 1, 480 ticks per quarter note).
 *
 * This module is a *renderer*: it converts the HIR `Score` into a binary
 * SMF. Correctness is the whole job — a Score must always produce a file
 * that external tools (midicsv, timidity, Logic Pro) can parse and play,
 * regardless of how many notes it contains (even zero).
 *
 * Invariants:
 * - Every event is preceded by a variable-length delta-time (>= 0).
 * - Channel events always include a status byte (no running status).
 * - Multi-byte fields are big-endian; the `MTrk` length equals the actual
 *   number of event bytes that follow.
 * - Each note emits exactly one Note On and one Note Off.
 * - A note with a non-empty pitch-bend vector emits its 0xE0 messages
 *   evenly across its duration (between the Note On and the Note Off);
 *   a constant-pitch note (empty vector) emits none.
 * - Per-channel scaffolding: at t=0 the writer emits program change +
 *   sustain-on for each distinct channel used by the notes (sorted); at the
 *   end it emits sustain-off for each. When only channel 0 is used (the
 *   common case: aubio path, test corpus), the output is byte-identical to
 *   the single-channel writer.
 * - The track always ends with an End-of-Track meta event (FF 2F 00).
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <libaudio/hir.h>
#include <libaudio/midiFileWriter.h>
#include <string>
#include <vector>

namespace libaudio {

// ============================================================================
// MidiFileWriter::Impl — Private implementation (Pimpl pattern).
//
// All SMF byte-format logic is isolated here. The public interface never
// exposes FILE* or binary details. RAII: the file handle is closed when the
// Impl is destroyed, so write() can return early on failure without leaks.
// ============================================================================
struct MidiFileWriter::Impl {
   static constexpr uint16_t TICKS_PER_QUARTER_NOTE = 480;

   FILE* file = nullptr;
   std::string outputPath;
   uint64_t bytesWritten_ = 0;

   ~Impl() {
      if (file) {
         fclose(file);
         file = nullptr;
      }
   }

   // ------------------------------------------------------------------
   // Big-endian primitives.
   // ------------------------------------------------------------------

   static bool writeUint16(FILE* f, uint16_t value) {
      uint8_t bytes[2] = {static_cast<uint8_t>((value >> 8) & 0xFF),
                          static_cast<uint8_t>(value & 0xFF)};
      return fwrite(bytes, 2, 1, f) == 1;
   }

   static bool writeUint32(FILE* f, uint32_t value) {
      uint8_t bytes[4] = {static_cast<uint8_t>((value >> 24) & 0xFF),
                          static_cast<uint8_t>((value >> 16) & 0xFF),
                          static_cast<uint8_t>((value >> 8) & 0xFF),
                          static_cast<uint8_t>(value & 0xFF)};
      return fwrite(bytes, 4, 1, f) == 1;
   }

   // ------------------------------------------------------------------
   // Variable-length integer (MIDI delta-times).
   //
   // Each byte carries 7 data bits with the high bit as a continuation
   // flag. Groups are written most-significant first, so the final byte
   // (least significant group) has the continuation bit cleared.
   // ------------------------------------------------------------------
   static void appendVarLen(std::vector<uint8_t>& out, uint32_t value) {
      std::vector<uint8_t> groups;
      if (value == 0) {
         groups.push_back(0);
      } else {
         uint32_t v = value;
         while (v > 0) {
            groups.push_back(static_cast<uint8_t>(v & 0x7F));
            v >>= 7;
         }
         std::reverse(groups.begin(), groups.end()); // MSB group first.
      }
      for (size_t i = 0; i < groups.size(); ++i) {
         if (i + 1 < groups.size()) {
            out.push_back(static_cast<uint8_t>(groups[i] | 0x80));
         } else {
            out.push_back(groups[i]);
         }
      }
   }

   // Append a full event: [delta-time varlen][payload bytes].
   static void emit(std::vector<uint8_t>& track, uint32_t deltaTicks,
                    std::vector<uint8_t> payload) {
      appendVarLen(track, deltaTicks);
      for (uint8_t b : payload) {
         track.push_back(b);
      }
   }

   // ------------------------------------------------------------------
   // Timing and range helpers.
   // ------------------------------------------------------------------

   // Convert seconds to MIDI ticks: seconds * ticksPerQuarterNote * (bpm/60).
   // With 480 ticks/qn and 120 BPM this is seconds * 960.
   static uint32_t secondsToTicks(double seconds, double tempoBpm) {
      if (seconds <= 0.0) {
         return 0;
      }
      double ticks = seconds * static_cast<double>(TICKS_PER_QUARTER_NOTE) *
                     (tempoBpm / 60.0);
      return static_cast<uint32_t>(std::llround(ticks));
   }

   static uint8_t clamp7(uint8_t value) { return value & 0x7F; }

   // ------------------------------------------------------------------
   // Event payload builders (no delta; the delta is added by emit()).
   // No running status — every event carries its own status byte.
   // ------------------------------------------------------------------

   static std::vector<uint8_t> noteOn(uint8_t pitch, uint8_t velocity,
                                      uint8_t channel) {
      return {static_cast<uint8_t>(0x90 | (channel & 0x0F)), pitch, velocity};
   }

   static std::vector<uint8_t> noteOff(uint8_t pitch, uint8_t velocity,
                                       uint8_t channel) {
      return {static_cast<uint8_t>(0x80 | (channel & 0x0F)), pitch, velocity};
   }

   static std::vector<uint8_t> controlChange(uint8_t controller, uint8_t value,
                                             uint8_t channel) {
      return {static_cast<uint8_t>(0xB0 | (channel & 0x0F)), controller, value};
   }

   static std::vector<uint8_t> programChange(uint8_t patch, uint8_t channel) {
      return {static_cast<uint8_t>(0xC0 | (channel & 0x0F)), patch};
   }

   // Pitch Bend (0xE0): a 14-bit signed value in [-8192, 8191], where 0 is the
   // note's centre (base) pitch and, by General MIDI convention, 4096 is one
   // semitone. Encoded as two data bytes with the low 7 bits first (the SMF
   // convention; a 14-bit value `s` in [0, 16383] is value + 8192).
   static std::vector<uint8_t> pitchBend(int16_t value, uint8_t channel) {
      const int32_t s = static_cast<int32_t>(value) + 8192;     // [0, 16383]
      const uint8_t lo = static_cast<uint8_t>(s & 0x7F);        // low 7 bits
      const uint8_t hi = static_cast<uint8_t>((s >> 7) & 0x7F); // high 7 bits
      return {static_cast<uint8_t>(0xE0 | (channel & 0x0F)), lo, hi};
   }

   // Set Tempo meta event (FF 51 03). Tempo is encoded as
   // microseconds per quarter note = 60,000,000 / bpm.
   static std::vector<uint8_t> setTempo(double tempoBpm) {
      double bpm = (tempoBpm > 0.0) ? tempoBpm : 120.0;
      uint32_t microseconds =
         static_cast<uint32_t>(std::llround(60000000.0 / bpm));
      return {0xFF,
              0x51,
              0x03,
              static_cast<uint8_t>((microseconds >> 16) & 0xFF),
              static_cast<uint8_t>((microseconds >> 8) & 0xFF),
              static_cast<uint8_t>(microseconds & 0xFF)};
   }

   static std::vector<uint8_t> endOfTrack() { return {0xFF, 0x2F, 0x00}; }

   // ------------------------------------------------------------------
   // buildTrack — assemble the full event byte stream for a Score.
   // ------------------------------------------------------------------
   static std::vector<uint8_t> buildTrack(const Score& score) {
      std::vector<uint8_t> track;
      const double tempo = score.tempo;
      uint32_t lastTick = 0;

      // Notes, sorted by start time. Each emits exactly one Note On
      // followed by one Note Off.
      std::vector<Note> notes = score.notes;
      std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
         return a.startTime < b.startTime;
      });

      // Compute the set of distinct channels used by the notes (for the
      // per-channel program/sustain scaffolding). Channel 0 is always in the
      // set when there are notes (the common case); additional channels
      // appear when the basic-pitch channel policy routes bent notes to them.
      bool channelUsed[16] = {};
      for (const Note& n : notes) {
         channelUsed[n.channel & 0x0F] = true;
      }

      // t=0 scaffolding: tempo, then per-channel program + sustain on.
      // Byte-identity invariant: when only channel 0 is used (the default for
      // the aubio Tier-1 path and the test corpus) this emits exactly the
      // pre-bend writer's bytes (one program + one sustain per channel 0).
      emit(track, 0, setTempo(tempo));
      for (uint8_t ch = 0; ch < 16; ++ch) {
         if (channelUsed[ch]) {
            emit(track, 0, programChange(0, ch));
            emit(track, 0, controlChange(64, 127, ch));
         }
      }

      for (const Note& note : notes) {
         uint8_t pitch = clamp7(note.pitch);
         // A Note On with velocity 0 is a "silent note off" by the MIDI
         // spec; enforce a minimum of 1 so a detected note always sounds.
         uint8_t velocity = static_cast<uint8_t>(
            std::min(127u, std::max(1u, static_cast<unsigned>(note.velocity))));
         uint8_t channel = note.channel & 0x0F;

         uint32_t onTick = secondsToTicks(note.startTime, tempo);
         uint32_t offTick = secondsToTicks(note.endTime, tempo);
         if (offTick <= onTick) {
            offTick = onTick + 1; // Guarantee a non-zero-length note.
         }

         // Note On at the note's start.
         uint32_t onDelta = (onTick > lastTick) ? (onTick - lastTick) : 0;
         emit(track, onDelta, noteOn(pitch, velocity, channel));
         if (onTick > lastTick) {
            lastTick = onTick;
         }

         // Pitch bends (0xE0), evenly spread across the note's duration and
         // placed between the Note On and the Note Off. A note with no bends
         // (an empty vector — every constant-pitch note, including the whole
         // aubio Tier-1 path and the test corpus) emits none, so such a Score
         // is byte-identical to the pre-bend writer. Each bend's time is a
         // point on the [start, end] grid (np.linspace in the reference) in
         // seconds; its delta is against the running timeline so absolute
         // ticks stay monotonic.
         if (!note.pitchBends.empty()) {
            const size_t n = note.pitchBends.size();
            for (size_t i = 0; i < n; ++i) {
               // Evenly spaced grid from start to end (inclusive endpoints).
               const double frac =
                  (n > 1u)
                     ? (static_cast<double>(i) / static_cast<double>(n - 1u))
                     : 0.0;
               const double tSec =
                  note.startTime + (note.endTime - note.startTime) * frac;
               const uint32_t tick = secondsToTicks(tSec, tempo);
               const uint32_t delta = (tick > lastTick) ? (tick - lastTick) : 0;
               emit(track, delta, pitchBend(note.pitchBends[i], channel));
               if (tick > lastTick) {
                  lastTick = tick;
               }
            }
         }

         // Note Off at the note's end (delta against the running timeline).
         uint32_t offDelta = (offTick > lastTick) ? (offTick - lastTick) : 0;
         emit(track, offDelta, noteOff(pitch, velocity, channel));

         if (offTick > lastTick) {
            lastTick = offTick; // Keep the timeline monotonic.
         }
      }

      // Score control events (pedals, etc.), sorted by time.
      std::vector<ControlEvent> controls = score.controls;
      std::sort(controls.begin(), controls.end(),
                [](const ControlEvent& a, const ControlEvent& b) {
                   return a.time < b.time;
                });
      for (const ControlEvent& control : controls) {
         uint32_t tick = secondsToTicks(control.time, tempo);
         uint32_t delta = (tick > lastTick) ? (tick - lastTick) : 0;
         emit(track, delta,
              controlChange(clamp7(control.controller), clamp7(control.value),
                            0));
         if (tick > lastTick) {
            lastTick = tick;
         }
      }

      // Release the sustain pedal on every used channel, then terminate.
      for (uint8_t ch = 0; ch < 16; ++ch) {
         if (channelUsed[ch]) {
            emit(track, 0, controlChange(64, 0, ch));
         }
      }
      emit(track, 0, endOfTrack());

      return track;
   }
};

// ============================================================================
// MidiFileWriter — public API implementation.
// ============================================================================

MidiFileWriter::MidiFileWriter(std::string_view path)
   : impl_(std::make_unique<Impl>()) {
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
   // Open the output file for binary writing.
   FILE* f = fopen(impl_->outputPath.c_str(), "wb");
   if (f == nullptr) {
      return false;
   }
   impl_->file = f; // Owned; closed in the Impl destructor on any path.

   // Header chunk: "MThd" + length(6) + format(1) + ntrks(1) + division(480).
   const uint8_t mthdId[] = {'M', 'T', 'h', 'd'};
   if (fwrite(mthdId, 4, 1, f) != 1) {
      return false;
   }
   if (!Impl::writeUint32(f, 6)) {
      return false;
   }
   if (!Impl::writeUint16(f, 1)) { // Format: 1 (multi-track).
      return false;
   }
   if (!Impl::writeUint16(f, 1)) { // Number of tracks: 1.
      return false;
   }
   if (!Impl::writeUint16(f, Impl::TICKS_PER_QUARTER_NOTE)) {
      return false; // Division: 480 ticks per quarter note.
   }

   // Track chunk: "MTrk" + length + event bytes. The length must equal the
   // number of event bytes that actually follow.
   const std::vector<uint8_t> track = Impl::buildTrack(score);

   const uint8_t mtrkId[] = {'M', 'T', 'r', 'k'};
   if (fwrite(mtrkId, 4, 1, f) != 1) {
      return false;
   }
   if (!Impl::writeUint32(f, static_cast<uint32_t>(track.size()))) {
      return false;
   }
   if (!track.empty() &&
       fwrite(track.data(), 1, track.size(), f) != track.size()) {
      return false;
   }

   // Total file size = 14 (header chunk) + 8 (MTrk id + length) + events.
   impl_->bytesWritten_ = 14 + 8 + static_cast<uint64_t>(track.size());

   // The file is complete. fclose flushes the stdio buffer to disk and
   // releases the handle. On success we clear impl_->file so the destructor
   // does not close it a second time. On failure we keep it owned by impl_
   // so the destructor closes the (partial) file.
   if (fclose(f) != 0) {
      return false;
   }
   impl_->file = nullptr;
   return true;
}

uint64_t MidiFileWriter::bytesWritten() const {
   return impl_ ? impl_->bytesWritten_ : 0;
}

bool MidiFileWriter::isOpen() const {
   return impl_ ? (impl_->file != nullptr) : false;
}

} // namespace libaudio

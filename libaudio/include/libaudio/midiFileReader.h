/**
 * @file midiFileReader.h
 * @brief Read Standard MIDI File (SMF) → HIR `Score` — the reader side of the
 *        writer.
 *
 * This is the inverse of `MidiFileWriter`: it parses a Standard MIDI File
 * (SMF) into the high-level `Score` (HIR). It is the *ground-truth* reader the
 * transcription evaluation harness needs: read a known-correct `.mid`, compare
 * it against what an analyzer detected, and score the difference.
 *
 * @section midifilereader-scope Scope: a standards-compliant SMF reader
 *
 * This is a general-purpose *standards-compliant* SMF reader, not one sized to
 * a particular writer. It handles:
 *   - a `MThd` header (format, track count, ticks-per-division),
 *   - `MTrk` tracks holding meta-events (tempo `FF 51`, title `FF 04`,
 *     time-signature, end-of-track, ...) and channel voice messages (note
 *     on/off, control change, program change, channel / poly aftertouch, pitch
 *     bend), skipping the rest.
 *
 * It honours the standard *running-status* byte (a status `< 0x80` reuses the
 * most recent channel message) exactly as the MIDI 1.0 spec and the reference
 * `midicsv` parser define it. Pitch bends (`0xE0`) are read back and attached
 * to the most recently opened note on their channel (so a written
 * `Note::pitchBends` round-trips).
 *
 * @section midifilereader-timing Timing: a per-segment running clock
 *
 * The reader converts event ticks to seconds with a *running clock* that
 * honours tempo changes: each delta advances the clock using the tempo in
 * effect at that point (a `FF 51` sets the tempo for subsequent deltas). A
 * note's `startTime` / `endTime` are the running clock at its note-on /
 * note-off; the `Score`'s tempo is the last tempo seen (BPM). For a
 * single-tempo file this reduces to the flat `ticks * tempo` conversion.
 *
 * @section midifilereader-malformed Malformed input: a loud failure
 *
 * Rather than silently dropping a garbled file, the reader reports *why* it
 * failed. `ok()` is false and `error()` names the problem for: a non-SMF, a
 * short / oversized header, an unsupported ticks-per-division, an `MTrk`
 * length past end-of-file, an unterminated variable-length value, event data
 * running past the track end, a running-status data byte with no prior channel
 * message, or an unknown / reserved / realtime status. Callers are expected to
 * fail loudly on `!ok()` (the evaluation harness aborts on invalid MIDI rather
 * than scoring against a half-parsed ground truth).
 *
 * @section midifilereader-hir HIR mapping
 *
 * A note-on (`9n pc vel`, vel != 0) opens a note; the matching note-off (`8n`
 * or `9n vel 0`) closes it, producing one `Note` with `pitch = pc`,
 * `velocity = vel`, and `startTime` / `endTime` in seconds. `channel` is the
 * MIDI channel of the note-on. `pitchBends` is filled with any `0xE0` values
 * seen while the note was open. Notes still open at the end of a track are
 * closed at the track's end time.
 *
 * No external dependencies: parsing is plain byte-level (the SMF format is
 * documented in the project's MIDI reference), so this module has no Pimpl C
 * library to hide — the `Impl` only holds the parsed `Score`.
 */

#ifndef LIBAUDIO_MIDIFILEREADER_H
#define LIBAUDIO_MIDIFILEREADER_H

#include <cstdint>
#include <memory>
#include <string>

namespace libaudio {

// Forward declaration of Score (from hir.h).
struct Score;

// ============================================================================
// MidiFileReader — parse a Standard MIDI File into a HIR Score.
//
// Domain context: the ground-truth reader for transcription evaluation. It is
// the reader counterpart to `MidiFileWriter` (which goes Score → SMF); together
// they round-trip a Score through the SMF format. It is standards-compliant
// (running status, per-segment tempo) and fails loudly on malformed input.
//
// RAII: the file is read in the constructor; there is no persistent handle to
// release, but the parsed `Score` is owned by the `Impl`.
// ============================================================================
class MidiFileReader {
 public:
   // Read and parse a SMF file.
   //
   // The file is read and parsed immediately. On success `ok()` is true and
   // `score()` holds the parsed notes. On failure (unreadable file, not a SMF,
   // or a malformed / unsupported structure) `ok()` is false, `score()` is
   // empty, and `error()` names the reason.
   //
   // @param path Path to the `.mid` file.
   explicit MidiFileReader(std::string_view path);

   // Destructor.
   ~MidiFileReader();

   // Non-copyable (owns the parsed Score).
   MidiFileReader(const MidiFileReader&) = delete;
   MidiFileReader& operator=(const MidiFileReader&) = delete;

   // Movable.
   MidiFileReader(MidiFileReader&& other) noexcept;
   MidiFileReader& operator=(MidiFileReader&& other) noexcept;

   // Whether the file was read and parsed successfully.
   [[nodiscard]] bool ok() const;

   // The parsed Score. Only meaningful when `ok()` is true; otherwise empty.
   [[nodiscard]] const Score& score() const;

   // A human-readable reason for failure (empty when `ok()` is true). Lets a
   // caller fail loudly on malformed input rather than silently skipping it.
   [[nodiscard]] const std::string& error() const;

 private:
   // Private implementation — holds the parsed Score and the byte-level parser.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_MIDIFILEREADER_H

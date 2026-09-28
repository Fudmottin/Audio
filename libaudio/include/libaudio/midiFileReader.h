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
 * @section midifilereader-scope Scope: sized to the writer's output
 *
 * This is deliberately a *minimal* SMF reader, not a general-purpose one. It
 * understands exactly the structure `MidiFileWriter` emits and that our
 * generated test corpus uses:
 *   - a `MThd` header (format, track count, ticks-per-division),
 *   - `MTrk` tracks holding: a tempo meta-event (`FF 51 03`), a title meta
 *     event (`FF 04`), and a run of note-on / note-off channel messages.
 * It handles the standard *running-status* byte (a status `< 0x80` reuses the
 * previous channel message) so it stays correct even if a writer ever omits a
 * repeated status byte. Control changes, program changes, pitch bends, and
 * sysex are skipped (not notes). Everything else is ignored.
 *
 * @section midifilereader-timing Timing: ticks → seconds
 *
 * The reader converts event *ticks* to *seconds* using the track's tempo. A
 * note's `startTime` / `endTime` in the resulting `Score` are in seconds,
 * matching the HIR convention (see `hir.h`). The tempo (BPM) is also stored on
 * the `Score`.
 *
 * @section midifilereader-hir HIR mapping
 *
 * A note-on (`9n pc vel`, vel != 0) opens a note; the matching note-off (`8n`
 * or `9n vel 0`) closes it, producing one `Note` with `pitch = pc`,
 * `velocity = vel`, and `startTime` / `endTime` in seconds. `channel` is the
 * MIDI channel of the note-on. Notes still open at the end of a track are
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
// they round-trip a Score through the SMF format. See the file header for the
// (deliberately minimal) scope and the HIR mapping.
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
   // or a parse error) `ok()` is false and `score()` is empty.
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

 private:
   // Private implementation — holds the parsed Score.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_MIDIFILEREADER_H

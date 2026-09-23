/**
 * @file midiFileWriter.h
 * @brief Write HIR to Standard MIDI File (SMF) — Type 1, 480 ticks/qn.
 *
 * This module writes a `Score` (HIR) to a Standard MIDI File (SMF).
 * It generates Type 1 files (multi-track) with 480 ticks per quarter
 * note, compatible with Apple Logic Pro.
 *
 * @section midi-specifications MIDI Specifications
 *
 * Key specifications for Logic Pro compatibility:
 * - **Type 1** multi-track files (multiple independent tracks).
 * - **480 ticks per quarter note** (Logic Pro default).
 * - **Acoustic Grand Piano = patch 0** (GM patch #1, channel 0).
 * - **Sustain pedal (CC#64)** messages are included.
 * - **Set Tempo** meta event is present (even if just 120 BPM).
 * - **End of Track** (`FF 2F 00`) at the end of every track.
 * - All multi-byte integers are **big-endian**.
 * - Delta-times are **non-negative** variable-length integers.
 * - No **running status** (for maximum parser compatibility).
 *
 * @section midi-file-structure File Structure
 *
 * ```
 * Header:  MThd  000006  Format:1  Tracks:N  Division:480
 *
 * Track 0 (Tempo + Piano):
 *   00  MTrk  [data]
 *     00  FF 51 03 0C 42 A0    // t=0, 120 BPM (500,000 µs/qn)
 *     00  C0 00                 // t=0, Program Change = 0 (Acoustic Grand)
 *     00  B0 40 7F              // t=0, sustain pedal ON (127)
 *     00  90 3C 64              // t=0, ch1, C4 (60), vel 100
 *    240  80 3C 40              // t=0.5s, ch1, C4 off, vel 64
 *     00  B0 40 00              // t=0, sustain pedal OFF (0)
 *     00  FF 2F 00              // t=0, End of Track
 * ```
 *
 */

#ifndef LIBAUDIO_MIDIFILEWRITER_H
#define LIBAUDIO_MIDIFILEWRITER_H

#include <cstdint>
#include <string>
#include <vector>

namespace libaudio {

// Forward declaration of Score (from hir.h).
struct Score;

// ============================================================================
// MidiFileWriter — Write HIR to Standard MIDI File (SMF).
//
// Domain context: This class converts a `Score` (HIR) to a Type 1
// Standard MIDI File (SMF) with 480 ticks per quarter note. It handles:
// - Header chunk (Format 1, 480 ticks/qn)
// - Track chunks (one track for piano, one for bass if present)
// - Note On/Off events with proper delta-times
// - Control Change events (sustain pedal CC#64, etc.)
// - Program Change (Acoustic Grand Piano = patch 0)
// - Set Tempo meta event (120 BPM default)
// - End of Track marker (FF 2F 00)
//
// Key design decisions:
// - Type 1 files (multi-track) for Logic Pro compatibility.
// - 480 ticks per quarter note (Logic Pro default).
// - No running status (for maximum parser compatibility).
// - Big-endian byte order for all multi-byte integers.
// - Variable-length integers for chunk lengths and delta-times.
//
// RAII resource management — file handle is released
// automatically when the C++ object is destroyed.
// ============================================================================
class MidiFileWriter {
 public:
   // Create a MidiFileWriter for the given output file path.
   //
   // @param path Path to the output MIDI file (e.g., "output.mid").
   explicit MidiFileWriter(std::string_view path);

   // Destructor. Closes the file handle.
   // RAII — resources are released automatically.
   ~MidiFileWriter();

   // Non-copyable (file handles are non-copyable).
   MidiFileWriter(const MidiFileWriter&) = delete;
   MidiFileWriter& operator=(const MidiFileWriter&) = delete;

   // Movable (file handles can be moved).
   MidiFileWriter(MidiFileWriter&& other) noexcept;
   MidiFileWriter& operator=(MidiFileWriter&& other) noexcept;

   // Write a Score (HIR) to a MIDI file.
   //
   // Converts the Score's notes and control events to MIDI events,
   // writes them as a Type 1 SMF with 480 ticks per quarter note.
   //
   // @param score The Score (HIR) to write.
   // @return true if the file was written successfully, false otherwise.
   bool write(const Score& score);

   // Get the number of bytes written to the output file.
   //
   // @return Number of bytes written, or 0 if no file was written.
   [[nodiscard]] uint64_t bytesWritten() const;

   // Check if the file is currently open.
   //
   // @return true if the file is open, false otherwise.
   [[nodiscard]] bool isOpen() const;

 private:
   // Private implementation — all MIDI file format logic is isolated here.
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace libaudio

#endif // LIBAUDIO_MIDIFILEWRITER_H

/**
 * @file hir.cpp
 * @brief Implementation of the High-level Instrumentation Representation (HIR).
 *
 * The HIR defines three aggregate structures:
 * - `Note` — A single note event (pitch, velocity, timing, channel, sustain).
 * - `ControlEvent` — A control change event (pedals, tempo changes, etc.).
 * - `Score` — A complete score (notes + controls + metadata).
 *
 * These are pure C++ data structures with no dependencies on aubio,
 * Core Audio, AIFF, MIDI, or LilyPond. They are the intermediate
 * language between audio analysis and both MIDI file output and
 * LilyPond source output.
 *
 */

#include <libaudio/hir.h>

// Note, ControlEvent, and Score are pure aggregate types with no
// implementation needed. All data members are public and default
// constructible. This file exists only to satisfy the build system
// and to serve as documentation for the HIR structures.

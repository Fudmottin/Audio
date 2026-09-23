/**
 * @file libaudio.h
 * @brief Public API summary — include this header to use libaudio.
 *
 * This is the summary header that includes all public module headers.
 * Include this single header to use all libaudio functionality.
 *
 * @section namespace Namespace
 *
 * All public types in libaudio live in `namespace libaudio`. The
 * `libaudio` namespace is deliberately separate from the project's
 * per-module namespaces (aiffcapture, midicapture, waterfall) so that
 * tools can pick and choose libaudio types by qualified name.
 *
 * @section pimpl-design Pimpl Pattern
 *
 * All libaudio classes use `std::unique_ptr<Impl>` to hide C library
 * internals. The benefits:
 *
 * 1. **RAII** — Resources (aubio handles, file descriptors) are
 *    automatically freed when the C++ object is destroyed.
 *
 * 2. **Encapsulation** — The rest of the codebase never sees C
 *    library types (no `aubio_pitchyin_t*`, `fvec_t*`, `SNDFILE*`).
 *
 * 3. **Swappability** — If a library's API changes, only the `Impl`
 *    struct needs updating (not every caller).
 *
 * 4. **Testability** — The C++ interface is clean and mockable
 *    (no library dependencies in tests).
 *
 * 5. **Compile-time** — Header files don't need library includes,
 *    giving faster compilation and fewer dependency issues.
 *
 */

#ifndef LIBAUDIO_LIBAUDIO_H
#define LIBAUDIO_LIBAUDIO_H

#include "audioFile.h"
#include "beat.h"
#include "controlEventExtractor.h"
#include "fft.h"
#include "hir.h"
#include "midiFileWriter.h"
#include "noteTrimmer.h"
#include "notes.h"
#include "onset.h"
#include "pitch.h"
#include "scoreBuilder.h"
#include "spectral.h"
#include "temporal.h"
#include "velocityEstimator.h"

#ifdef LIBAUDIO_HAS_RUBBERBAND
#include "rubberband.h"
#endif

#endif // LIBAUDIO_LIBAUDIO_H

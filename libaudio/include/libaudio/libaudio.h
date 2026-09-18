/**
 * @file libaudio.h
 * @brief Public API summary — include this header to use libaudio.
 *
 * This is the summary header that includes all public module headers.
 * Include this single header to use all libaudio functionality.
 *
 */

#ifndef LIBAUDIO_LIBAUDIO_H
#define LIBAUDIO_LIBAUDIO_H

#include "audioFile.h"
#include "fft.h"
#include "pitch.h"
#include "onset.h"
#include "beat.h"
#include "notes.h"
#include "spectral.h"
#include "temporal.h"
#include "hir.h"
#include "midiFileWriter.h"
#include "controlEventExtractor.h"
#include "scoreBuilder.h"
#include "velocityEstimator.h"
#include "noteTrimmer.h"

#ifdef LIBAUDIO_HAS_RUBBERBAND
#include "rubberband.h"
#endif

#endif // LIBAUDIO_LIBAUDIO_H

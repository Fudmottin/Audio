#!/bin/bash
# analyze-capture.sh — Analyze a captured AIFF file for pitch and channel separation.
#
# Usage: ./analyze-capture.sh <captured.aiff> [expected-side:left|right]
#   expected-side: "left" or "right" (default: "left")

set -euo pipefail

if [ $# -lt 1 ]; then
   echo "Usage: $0 <captured.aiff> [expected-side:left|right]" >&2
   exit 1
fi

INPUT="$1"
SIDE="${2:-left}"

if [ ! -f "$INPUT" ]; then
   echo "Error: file not found: $INPUT" >&2
   exit 1
fi

# Convert to mono WAV at 48kHz for sox analysis
WAV="/tmp/analyze-$$-left.wav"
WAVR="/tmp/analyze-$$-right.wav"

# Extract left and right channels as mono WAV at 48kHz
ffmpeg -y -i "$INPUT" -ar 48000 -ac 1 "$WAV" 2>/dev/null
ffmpeg -y -i "$INPUT" -ar 48000 -ac 1 -map 0:a:0 -channel_layout mono "$WAVR" 2>/dev/null

echo "========================================================================"
echo "CAPTURE ANALYSIS — expected side: $SIDE"
echo "========================================================================"

# Get source file info for comparison
SOURCE_FILE=""
if [ "$SIDE" = "left" ]; then
   SOURCE_FILE="/Users/david/Projects/cpp/Audio/aiffcapture/test-audio/03 Test Signals For Basic Checks_ 1001 Hz Sine Wave_ - 16 Db, Left.m4a"
else
   SOURCE_FILE="/Users/david/Projects/cpp/Audio/aiffcapture/test-audio/04 Test Signals For Basic Checks_ 1001 Hz Sine Wave_ - 16 Db, Right.m4a"
fi

if [ -f "$SOURCE_FILE" ]; then
   echo ""
   echo "--- Source file (reference) ---"
   SOURCE_WAV="/tmp/source-ref.wav"
   ffmpeg -y -i "$SOURCE_FILE" -ar 48000 -ac 1 "$SOURCE_WAV" 2>/dev/null
   sox "$SOURCE_WAV" -n stat 2>&1 | grep -E "Rough frequency|Mean amplitude|Maximum amplitude|RMS"
   rm -f "$SOURCE_WAV"
fi

echo ""
echo "--- Captured file ---"

# Get sox stats for the captured file
echo ""
echo "Left channel:"
sox "$WAV" -n stat 2>&1 | grep -E "Rough frequency|Mean amplitude|Maximum amplitude|RMS|Samples read|Length"

echo ""
echo "Right channel:"
# Extract right channel using sox (more reliable than ffmpeg channelsplit)
ffmpeg -y -i "$INPUT" -ar 48000 -ac 2 -c:a pcm_s16le "/tmp/analyze-$$-stereo.wav" 2>/dev/null
sox "/tmp/analyze-$$-stereo.wav" -c 1 -r 48000 /tmp/analyze-$$-right-ch.wav remix 2 2>/dev/null
sox "/tmp/analyze-$$-right-ch.wav" -n stat 2>&1 | grep -E "Rough frequency|Mean amplitude|Maximum amplitude|RMS|Samples read|Length"
echo "  (Right channel should be near-zero for a $SIDE-channel source)"
rm -f "/tmp/analyze-$$-stereo.wav" "/tmp/analyze-$$-right-ch.wav"

# Duration check
echo ""
CAPTURED_DUR=$(ffprobe "$INPUT" 2>&1 | grep Duration | sed 's/.*Duration: \([^,]*\).*/\1/')
echo "Captured duration: $CAPTURED_DUR"

# Clean up
rm -f "$WAV" "$WAVR" "/tmp/analyze-$$-stereo.wav"

echo ""
echo "========================================================================"
echo "To play back: ./aiff2wav.sh $INPUT && ffplay $(dirname $INPUT)/$(basename ${INPUT%.aiff}).wav"
echo "========================================================================"

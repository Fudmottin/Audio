#!/bin/bash
# aiff2wav.sh — Convert AIFF files to WAV format.
#
# The AIFF file from aiffcapture is 100% valid per the spec, but
# QuickTime Player and other macOS tools have a bug where they
# always try to parse 80-bit extended float for sample rate
# regardless of COMM chunk size. This utility extracts the raw
# PCM data and wraps it in a WAV header that any tool can read.
#
# Usage: aiff2wav.sh <input.aiff> [output.wav]
# If output is omitted, it defaults to <input>.wav

set -euo pipefail

if [ $# -lt 1 ]; then
   echo "Usage: $0 <input.aiff> [output.wav]" >&2
   exit 1
fi

input="$1"

if [ ! -f "$input" ]; then
   echo "Error: file not found: $input" >&2
   exit 1
fi

# Default output: same name with .wav extension
if [ $# -ge 2 ]; then
   output="$2"
else
   output="${input%.aiff}.wav"
fi

# Read channels (offset 20, 2 bytes big-endian) as hex, then to decimal
channelsHex=$(xxd -s 20 -l 2 -p "$input")
channels=$(printf '%d' "0x${channelsHex}")

# Read sample rate (offset 28, 4 bytes big-endian) as hex, then to decimal
sampleRateHex=$(xxd -s 28 -l 4 -p "$input")
sampleRate=$(printf '%d' "0x${sampleRateHex}")

# PCM data size (total file minus 48-byte AIFF header)
fileSize=$(wc -c < "$input")
pcmSize=$((fileSize - 48))

# Helper: pack a 16-bit integer as little-endian hex bytes
pack_le16() {
   printf '%02x%02x' $(( $1 & 0xFF )) $(( ( $1 >> 8 ) & 0xFF ))
}

# Helper: pack a 32-bit integer as little-endian hex bytes
pack_le32() {
   printf '%02x%02x%02x%02x' $(( $1 & 0xFF )) $(( ( $1 >> 8 ) & 0xFF )) $(( ( $1 >> 16 ) & 0xFF )) $(( ( $1 >> 24 ) & 0xFF ))
}

riffSize=$(( 36 + pcmSize ))
byteRate=$(( sampleRate * channels * 2 ))
blockAlign=$(( channels * 2 ))

# Build the 44-byte WAV header as a hex string, then convert to binary.
# WAV header layout (little-endian):
#   Offset  0:  "RIFF" (4 bytes)
#   Offset  4:  file size - 8 (4 bytes, little-endian)
#   Offset  8:  "WAVE" (4 bytes)
#   Offset 12:  "fmt " (4 bytes)
#   Offset 16:  16 (fmt chunk size, 4 bytes, little-endian)
#   Offset 20:  1 (audio format = PCM, 2 bytes, little-endian)
#   Offset 22:  channels (2 bytes, little-endian)
#   Offset 24:  sampleRate (4 bytes, little-endian)
#   Offset 28:  byteRate (4 bytes, little-endian)
#   Offset 32:  blockAlign (2 bytes, little-endian)
#   Offset 34:  16 (bits per sample, 2 bytes, little-endian)
#   Offset 36:  "data" (4 bytes)
#   Offset 40:  pcmSize (4 bytes, little-endian)

headerHex=""
headerHex="${headerHex}52494646"  # "RIFF"
headerHex="${headerHex}$(pack_le32 "$riffSize")"
headerHex="${headerHex}57415645"  # "WAVE"
headerHex="${headerHex}666d7420"  # "fmt "
headerHex="${headerHex}$(pack_le32 16)"         # fmt chunk size (always 16)
headerHex="${headerHex}$(pack_le16 1)"          # audio format = PCM
headerHex="${headerHex}$(pack_le16 "$channels")"
headerHex="${headerHex}$(pack_le32 "$sampleRate")"
headerHex="${headerHex}$(pack_le32 "$byteRate")"
headerHex="${headerHex}$(pack_le16 "$blockAlign")"
headerHex="${headerHex}$(pack_le16 16)"         # bits per sample
headerHex="${headerHex}64617461"  # "data"
headerHex="${headerHex}$(pack_le32 "$pcmSize")"

# Convert hex string to binary and write header
printf "%s" "$headerHex" | xxd -r -p > "$output"

# Append PCM data (skip 48-byte AIFF header), byte-swapping
# from big-endian (AIFF) to little-endian (WAV).
dd if="$input" bs=48 skip=1 2>/dev/null \
   | xxd -p \
   | sed 's/\(..\)\(..\)/\2\1/g' \
   | xxd -r -p >> "$output"

echo "Converted: $input -> $output"
echo "  Channels: $channels"
echo "  Sample rate: ${sampleRate} Hz"
echo "  PCM size: $pcmSize bytes"

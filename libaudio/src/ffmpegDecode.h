/**
 * @file ffmpegDecode.h
 * @brief In-process audio decode + resample using the FFmpeg C API.
 *
 * This is an **internal** (non-public) unit. It provides the streaming decode
 * that replaces the prior `std::system("ffmpeg ...")` shell-out.
 *
 * `decodeToMonoFloat` opens any audio container, decodes it, and resamples +
 * downmixes to mono float32 at the target rate — all in-process, no temp file,
 * no subprocess. The output is the full resampled buffer (unavoidable for a
 * sequence model; 1 min ≈ 5.3 MB at 22050 Hz mono f32).
 *
 * The implementation is the canonical FFmpeg C API pipeline:
 *   avformat_open_input → av_find_best_stream → avcodec_open2
 *   → swr_alloc_set_opts2 → {av_read_frame → avcodec_send_packet
 *   → avcodec_receive_frame → swr_convert}* → flush → teardown
 *
 * Requires libavformat, libavcodec, libswresample, libavutil (linked via
 * pkg-config in CMakeLists.txt, Tier-2 section).
 */

#ifndef LIBAUDIO_INTERNAL_FFMPEG_DECODE_H
#define LIBAUDIO_INTERNAL_FFMPEG_DECODE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace libaudio::detail {

/**
 * Decode `inputPath` to mono float32 at `targetRate` Hz, in-process.
 *
 * Streaming: reads packets, decodes frames, resamples via libswresample —
 * no temp file, no subprocess. The entire decoded + resampled audio is
 * returned as a flat vector of samples.
 *
 * @param inputPath   Path to the audio file (any container FFmpeg supports).
 * @param targetRate  Desired output sample rate in Hz (e.g. 22050).
 *
 * @return A vector of mono float32 samples at `targetRate` Hz.
 *
 * @throws std::runtime_error with a descriptive message (including FFmpeg's
 *         own log output) if the file cannot be opened, decoded, or if no
 *         audio stream is present.
 */
std::vector<float> decodeToMonoFloat(std::string_view inputPath,
                                     uint32_t targetRate);

} // namespace libaudio::detail

#endif // LIBAUDIO_INTERNAL_FFMPEG_DECODE_H

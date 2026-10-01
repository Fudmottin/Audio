/**
 * @file ffmpegDecode.cpp
 * @brief In-process audio decode + resample using the FFmpeg C API.
 *
 * Replaces the prior `std::system("ffmpeg ...")` shell-out. The canonical
 * FFmpeg C API pipeline: demux → decode → resample/downmix → flat float32
 * buffer. No temp file, no subprocess.
 *
 * License note: libavformat/libavcodec/libswresample/libavutil are LGPL 2.1+.
 * We link them dynamically (Homebrew .dylib) from our Apache-2.0 code, which
 * is fully compliant.
 */

#include "ffmpegDecode.h"

// FFmpeg's own headers (libavutil/common.h in particular) do implicit
// int→uint8_t / int64_t→uint64_t conversions that trip -Wsign-conversion /
// -Wconversion. Suppress those for the includes only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libswresample/swresample.h>
}
#pragma GCC diagnostic pop

#include <cmath>
#include <cstdarg>
#include <cstring>
#include <stdexcept>
#include <string>

namespace libaudio::detail {
namespace {

// ============================================================================
// FFmpeg log capture — redirect av_log into a thread-local string for error
// messages. Reset before each decode call.
// ============================================================================
thread_local std::string tl_ffmpegLog;

void ffmpegLogCallback(void*, int level, const char* fmt, va_list vl) {
   if (level > AV_LOG_WARNING) return;
   char buf[1024];
   vsnprintf(buf, sizeof(buf), fmt, vl);
   tl_ffmpegLog += buf;
}

/**
 * Build an error message from an FFmpeg return code, including the log.
 */
[[noreturn]] void throwFFmpegError(const char* what, const std::string& path,
                                   int ret) {
   char buf[AV_ERROR_MAX_STRING_SIZE];
   av_strerror(ret, buf, sizeof(buf));
   std::string msg = std::string(what) + " '" + path + "': " + buf;
   if (!tl_ffmpegLog.empty()) msg += "\n" + tl_ffmpegLog;
   throw std::runtime_error(msg);
}

} // namespace

// ============================================================================
// decodeToMonoFloat — the public entry point.
// ============================================================================
std::vector<float> decodeToMonoFloat(std::string_view inputPath,
                                     uint32_t targetRate) {
   const std::string path(inputPath);
   tl_ffmpegLog.clear();
   av_log_set_callback(ffmpegLogCallback);
   av_log_set_level(AV_LOG_ERROR); // Only capture errors/warnings.

   AVFormatContext* fmtCtx = nullptr;
   AVCodecContext* codecCtx = nullptr;
   SwrContext* swr = nullptr;
   AVFrame* frame = nullptr;
   AVPacket* pkt = nullptr;
   const AVCodec* decoder = nullptr;
   int audioStreamIdx = -1;

   auto cleanup = [&]() {
      if (pkt) av_packet_free(&pkt);
      if (frame) av_frame_free(&frame);
      if (swr) swr_free(&swr);
      if (codecCtx) avcodec_free_context(&codecCtx);
      if (fmtCtx) avformat_close_input(&fmtCtx);
   };

   // --- 1. Open the container ------------------------------------------
   int ret = avformat_open_input(&fmtCtx, path.c_str(), nullptr, nullptr);
   if (ret < 0) throwFFmpegError("avformat_open_input failed", path, ret);

   // --- 2. Find stream info + best audio stream ------------------------
   ret = avformat_find_stream_info(fmtCtx, nullptr);
   if (ret < 0) throwFFmpegError("avformat_find_stream_info failed", path, ret);

   ret = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
   if (ret < 0) throwFFmpegError("no audio stream found", path, ret);
   audioStreamIdx = ret;

   AVStream* stream = fmtCtx->streams[audioStreamIdx];

   // --- 3. Open the decoder --------------------------------------------
   codecCtx = avcodec_alloc_context3(decoder);
   if (!codecCtx) {
      cleanup();
      throw std::runtime_error("avcodec_alloc_context3 failed");
   }
   ret = avcodec_parameters_to_context(codecCtx, stream->codecpar);
   if (ret < 0)
      throwFFmpegError("avcodec_parameters_to_context failed", path, ret);
   ret = avcodec_open2(codecCtx, decoder, nullptr);
   if (ret < 0) {
      cleanup();
      throwFFmpegError("avcodec_open2 failed", path, ret);
   }

   // --- 4. Pre-allocate the output buffer (if duration is known) --------
   std::vector<float> out;
   if (fmtCtx->duration > 0) {
      double seconds = static_cast<double>(fmtCtx->duration) /
                       static_cast<double>(AV_TIME_BASE);
      size_t estimated =
         static_cast<size_t>(seconds * static_cast<double>(targetRate));
      out.reserve(estimated + 16); // margin for resampler
   } else {
      out.reserve(22050 * 300); // ~5 min fallback
   }

   // --- 5. Set up the resampler ----------------------------------------
   // Output: mono, float32, targetRate.
   AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;

   // Input: from the codec context. Some codecs report an unspecified layout;
   // fall back to a default layout with the right channel count.
   AVChannelLayout inLayout = codecCtx->ch_layout;
   if (inLayout.order == AV_CHANNEL_ORDER_UNSPEC) {
      int nb = codecCtx->ch_layout.nb_channels;
      if (nb <= 0) nb = 2; // assume stereo if unknown
      av_channel_layout_default(&inLayout, nb);
   }

   ret = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT,
                             static_cast<int>(targetRate), &inLayout,
                             codecCtx->sample_fmt, codecCtx->sample_rate, 0,
                             nullptr);
   if (ret < 0 || !swr) {
      std::string msg = "swr_alloc_set_opts2 failed for '" + path + "'";
      if (!tl_ffmpegLog.empty()) msg += "\n" + tl_ffmpegLog;
      cleanup();
      throw std::runtime_error(msg);
   }
   ret = swr_init(swr);
   if (ret < 0) {
      std::string msg = "swr_init failed for '" + path + "'";
      if (!tl_ffmpegLog.empty()) msg += "\n" + tl_ffmpegLog;
      cleanup();
      throw std::runtime_error(msg);
   }

   // --- 6. The decode + resample loop ----------------------------------
   frame = av_frame_alloc();
   pkt = av_packet_alloc();
   if (!frame || !pkt) {
      cleanup();
      throw std::runtime_error("Failed to allocate AVFrame/AVPacket");
   }

   // A small stack buffer for swr_convert output (one frame at a time).
   // 8192 samples is well above any typical frame size (usually ≤ 2048).
   float outBuf[8192];
   const int outCapacity = 8192;

   // swr_convert expects `uint8_t* const*` (out) and `const uint8_t* const*`
   // (in). AVFrame.data is `uint8_t*[]`; the standard C++/FFmpeg idiom is a
   // reinterpret_cast (safe: we never write through the input pointers).
   auto convertFrame = [&](AVFrame* f) {
      auto in = reinterpret_cast<const uint8_t* const*>(f->data);
      auto outPlane = reinterpret_cast<uint8_t*>(outBuf);
      uint8_t* const outPtrs[1] = {outPlane};
      int converted = swr_convert(swr, outPtrs, outCapacity, in, f->nb_samples);
      if (converted > 0) out.insert(out.end(), outBuf, outBuf + converted);
   };

   while (true) {
      ret = av_read_frame(fmtCtx, pkt);
      if (ret < 0) break; // EOF or error

      // Only process the audio stream.
      if (pkt->stream_index != audioStreamIdx) {
         av_packet_unref(pkt);
         continue;
      }

      ret = avcodec_send_packet(codecCtx, pkt);
      av_packet_unref(pkt);
      if (ret < 0 && ret != EAGAIN) break; // Decoder error.

      // Drain all frames from the decoder.
      while ((ret = avcodec_receive_frame(codecCtx, frame)) == 0) {
         convertFrame(frame);
         av_frame_unref(frame);
      }
      // ret == AVERROR(EAGAIN) means "send more packets"; not an error.
   }

   // --- 7. Flush the decoder + resampler -------------------------------
   avcodec_send_packet(codecCtx, nullptr);
   while ((ret = avcodec_receive_frame(codecCtx, frame)) == 0) {
      convertFrame(frame);
      av_frame_unref(frame);
   }

   // Flush the resampler (drain any internal buffer).
   {
      auto outPlane = reinterpret_cast<uint8_t*>(outBuf);
      uint8_t* const outPtrs[1] = {outPlane};
      for (int converted = 0;
           (converted = swr_convert(swr, outPtrs, outCapacity, nullptr, 0)) >
           0;) {
         out.insert(out.end(), outBuf, outBuf + converted);
      }
   }

   // --- 8. Teardown ----------------------------------------------------
   cleanup();

   if (out.empty()) {
      std::string msg = "decode produced no samples from '" + path + "'";
      if (!tl_ffmpegLog.empty()) msg += "\n" + tl_ffmpegLog;
      throw std::runtime_error(msg);
   }

   return out;
}

} // namespace libaudio::detail

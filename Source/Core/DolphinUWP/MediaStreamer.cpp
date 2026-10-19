// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/MediaStreamer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <Windows.h>
#include <combaseapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <fmt/format.h>

#include "AudioCommon/Mixer.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "DolphinUWP/GBAScreens.h"
#include "VideoCommon/FrameDumper.h"

namespace DolphinUWP::MediaStreamer
{
namespace
{
namespace Frontend = UwpImGuiFrontend;
using Clock = std::chrono::steady_clock;

constexpr u32 DEFAULT_WIDTH = 1280;
constexpr u32 DEFAULT_HEIGHT = 720;
constexpr u32 DEFAULT_FPS = 30;
constexpr u32 DEFAULT_BITRATE_KBPS = 4000;
constexpr u32 MIN_BITRATE_KBPS = 500;
constexpr u32 GBA_WIDTH = 240;
constexpr u32 GBA_HEIGHT = 160;
constexpr u32 DEFAULT_GBA_SCALE = 4;
constexpr u32 MAX_GBA_SCALE = 8;
constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr std::size_t MAX_QUEUED_AUDIO_SAMPLES = AUDIO_SAMPLE_RATE * 2;
// Seven transport stream packets, the payload of one UDP datagram.
constexpr int PACKET_SIZE = 1316;

void LogCallback(void*, int level, const char* format, va_list args)
{
  if (level > AV_LOG_INFO)
    return;

  using Common::Log::LogLevel;
  const LogLevel log_level = level <= AV_LOG_ERROR   ? LogLevel::LERROR :
                             level <= AV_LOG_WARNING ? LogLevel::LWARNING :
                                                       LogLevel::LINFO;
  const auto* log_manager = Common::Log::LogManager::GetInstance();
  if (!log_manager || !log_manager->IsEnabled(Common::Log::LogType::FRAMEDUMP, log_level))
    return;

  char message[1024];
  CharArrayFromFormatV(message, sizeof(message), format, args);
  const std::string_view text = StripWhitespace(message);
  if (!text.empty())
    GENERIC_LOG_FMT(Common::Log::LogType::FRAMEDUMP, log_level, "FFmpeg: {}", text);
}

class FfmpegApi
{
public:
  bool Load()
  {
    std::call_once(m_once, [this] { m_loaded = LoadImpl(); });
    return m_loaded;
  }

  std::string Error(int code) const
  {
    char text[AV_ERROR_MAX_STRING_SIZE]{};
    if (p_av_strerror && p_av_strerror(code, text, sizeof(text)) == 0)
      return text;
    return fmt::format("error {}", code);
  }

#define FFMPEG_PROC(name) decltype(&::name) p_##name = nullptr
  FFMPEG_PROC(av_channel_layout_copy);
  FFMPEG_PROC(av_channel_layout_default);
  FFMPEG_PROC(av_channel_layout_uninit);
  FFMPEG_PROC(av_dict_free);
  FFMPEG_PROC(av_dict_set);
  FFMPEG_PROC(av_frame_alloc);
  FFMPEG_PROC(av_frame_free);
  FFMPEG_PROC(av_frame_get_buffer);
  FFMPEG_PROC(av_frame_make_writable);
  FFMPEG_PROC(av_freep);
  FFMPEG_PROC(av_log_set_callback);
  FFMPEG_PROC(av_malloc);
  FFMPEG_PROC(av_strerror);

  FFMPEG_PROC(av_packet_alloc);
  FFMPEG_PROC(av_packet_free);
  FFMPEG_PROC(av_packet_rescale_ts);
  FFMPEG_PROC(av_packet_unref);
  FFMPEG_PROC(avcodec_alloc_context3);
  FFMPEG_PROC(avcodec_find_encoder_by_name);
  FFMPEG_PROC(avcodec_free_context);
  FFMPEG_PROC(avcodec_open2);
  FFMPEG_PROC(avcodec_parameters_from_context);
  FFMPEG_PROC(avcodec_receive_packet);
  FFMPEG_PROC(avcodec_send_frame);

  FFMPEG_PROC(av_interleaved_write_frame);
  FFMPEG_PROC(av_write_trailer);
  FFMPEG_PROC(avformat_alloc_output_context2);
  FFMPEG_PROC(avformat_free_context);
  FFMPEG_PROC(avformat_network_init);
  FFMPEG_PROC(avformat_new_stream);
  FFMPEG_PROC(avformat_write_header);
  FFMPEG_PROC(avio_alloc_context);
  FFMPEG_PROC(avio_closep);
  FFMPEG_PROC(avio_context_free);
  FFMPEG_PROC(avio_flush);
  FFMPEG_PROC(avio_open2);
  FFMPEG_PROC(avio_write);

  FFMPEG_PROC(swr_alloc_set_opts2);
  FFMPEG_PROC(swr_convert);
  FFMPEG_PROC(swr_free);
  FFMPEG_PROC(swr_init);

  FFMPEG_PROC(sws_freeContext);
  FFMPEG_PROC(sws_getContext);
  FFMPEG_PROC(sws_scale);
#undef FFMPEG_PROC

private:
  bool LoadImpl()
  {
    m_avutil = LoadPackagedLibrary(L"avutil-58.dll", 0);
    m_swresample = LoadPackagedLibrary(L"swresample-4.dll", 0);
    m_swscale = LoadPackagedLibrary(L"swscale-7.dll", 0);
    m_avcodec = LoadPackagedLibrary(L"avcodec-60.dll", 0);
    m_avformat = LoadPackagedLibrary(L"avformat-60.dll", 0);
    if (!m_avutil || !m_swresample || !m_swscale || !m_avcodec || !m_avformat)
    {
      ERROR_LOG_FMT(FRAMEDUMP, "Could not load the packaged FFmpeg runtime: {}",
                    Common::GetLastErrorString());
      return false;
    }

#define LOAD_FFMPEG(module, name)                                                                  \
  p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(module, #name));                  \
  if (!p_##name)                                                                                   \
  {                                                                                                \
    ERROR_LOG_FMT(FRAMEDUMP, "The FFmpeg runtime is missing {}", #name);                           \
    return false;                                                                                  \
  }
    LOAD_FFMPEG(m_avutil, av_channel_layout_copy);
    LOAD_FFMPEG(m_avutil, av_channel_layout_default);
    LOAD_FFMPEG(m_avutil, av_channel_layout_uninit);
    LOAD_FFMPEG(m_avutil, av_dict_free);
    LOAD_FFMPEG(m_avutil, av_dict_set);
    LOAD_FFMPEG(m_avutil, av_frame_alloc);
    LOAD_FFMPEG(m_avutil, av_frame_free);
    LOAD_FFMPEG(m_avutil, av_frame_get_buffer);
    LOAD_FFMPEG(m_avutil, av_frame_make_writable);
    LOAD_FFMPEG(m_avutil, av_freep);
    LOAD_FFMPEG(m_avutil, av_log_set_callback);
    LOAD_FFMPEG(m_avutil, av_malloc);
    LOAD_FFMPEG(m_avutil, av_strerror);

    LOAD_FFMPEG(m_avcodec, av_packet_alloc);
    LOAD_FFMPEG(m_avcodec, av_packet_free);
    LOAD_FFMPEG(m_avcodec, av_packet_rescale_ts);
    LOAD_FFMPEG(m_avcodec, av_packet_unref);
    LOAD_FFMPEG(m_avcodec, avcodec_alloc_context3);
    LOAD_FFMPEG(m_avcodec, avcodec_find_encoder_by_name);
    LOAD_FFMPEG(m_avcodec, avcodec_free_context);
    LOAD_FFMPEG(m_avcodec, avcodec_open2);
    LOAD_FFMPEG(m_avcodec, avcodec_parameters_from_context);
    LOAD_FFMPEG(m_avcodec, avcodec_receive_packet);
    LOAD_FFMPEG(m_avcodec, avcodec_send_frame);

    LOAD_FFMPEG(m_avformat, av_interleaved_write_frame);
    LOAD_FFMPEG(m_avformat, av_write_trailer);
    LOAD_FFMPEG(m_avformat, avformat_alloc_output_context2);
    LOAD_FFMPEG(m_avformat, avformat_free_context);
    LOAD_FFMPEG(m_avformat, avformat_network_init);
    LOAD_FFMPEG(m_avformat, avformat_new_stream);
    LOAD_FFMPEG(m_avformat, avformat_write_header);
    LOAD_FFMPEG(m_avformat, avio_alloc_context);
    LOAD_FFMPEG(m_avformat, avio_closep);
    LOAD_FFMPEG(m_avformat, avio_context_free);
    LOAD_FFMPEG(m_avformat, avio_flush);
    LOAD_FFMPEG(m_avformat, avio_open2);
    LOAD_FFMPEG(m_avformat, avio_write);

    LOAD_FFMPEG(m_swresample, swr_alloc_set_opts2);
    LOAD_FFMPEG(m_swresample, swr_convert);
    LOAD_FFMPEG(m_swresample, swr_free);
    LOAD_FFMPEG(m_swresample, swr_init);

    LOAD_FFMPEG(m_swscale, sws_freeContext);
    LOAD_FFMPEG(m_swscale, sws_getContext);
    LOAD_FFMPEG(m_swscale, sws_scale);
#undef LOAD_FFMPEG

    p_av_log_set_callback(LogCallback);
    p_avformat_network_init();
    return true;
  }

  std::once_flag m_once;
  bool m_loaded = false;
  HMODULE m_avutil = nullptr;
  HMODULE m_swresample = nullptr;
  HMODULE m_swscale = nullptr;
  HMODULE m_avcodec = nullptr;
  HMODULE m_avformat = nullptr;
};

FfmpegApi& GetFfmpegApi()
{
  static FfmpegApi api;
  return api;
}

bool Fail(std::string_view context, int code, std::string* error)
{
  *error =
      code < 0 ? fmt::format("{}: {}", context, GetFfmpegApi().Error(code)) : std::string(context);
  return false;
}

struct StreamSettings
{
  u32 width = 0;
  u32 height = 0;
  u32 fps = 0;
  u32 bitrate_kbps = 0;
  bool audio = false;
};

struct VideoFrame
{
  std::vector<u8> pixels;
  u32 width = 0;
  u32 height = 0;
  u32 stride = 0;
  Clock::time_point time;
};

class Encoder
{
public:
  using WriteCallback = int (*)(void* opaque, uint8_t* data, int size);

  ~Encoder() { Close(); }

  bool Open(const StreamSettings& settings, int scale_flags, void* opaque, WriteCallback write,
            std::string* error)
  {
    FfmpegApi& api = GetFfmpegApi();
    if (!api.Load())
    {
      *error = "The packaged FFmpeg runtime could not be loaded";
      return false;
    }

    int result = api.p_avformat_alloc_output_context2(&m_format, nullptr, "mpegts", nullptr);
    if (result < 0 || !m_format)
      return Fail("Could not create the MPEG-TS output", result, error);
    m_format->flags |= AVFMT_FLAG_FLUSH_PACKETS | AVFMT_FLAG_CUSTOM_IO;
    m_format->max_delay = 0;

    // The muxer fills one datagram at a time and hands it to every destination.
    auto* const buffer = static_cast<unsigned char*>(api.p_av_malloc(PACKET_SIZE));
    if (!buffer)
    {
      *error = "Could not allocate the stream buffer";
      return false;
    }
    m_format->pb =
        api.p_avio_alloc_context(buffer, PACKET_SIZE, 1, opaque, nullptr, write, nullptr);
    if (!m_format->pb)
    {
      void* unused = buffer;
      api.p_av_freep(&unused);
      *error = "Could not create the stream output";
      return false;
    }

    m_scale_flags = scale_flags;
    if (!OpenVideo(settings, error))
      return false;
    if (settings.audio && !OpenAudio(error))
      return false;

    AVDictionary* format_options = nullptr;
    api.p_av_dict_set(&format_options, "flush_packets", "1", 0);
    api.p_av_dict_set(&format_options, "mpegts_flags", "resend_headers", 0);
    result = api.p_avformat_write_header(m_format, &format_options);
    api.p_av_dict_free(&format_options);
    if (result < 0)
      return Fail("Could not start the MPEG-TS stream", result, error);
    m_header_written = true;

    m_fps = settings.fps;
    m_start = Clock::now();
    return true;
  }

  bool EncodeVideo(const u8* pixels, u32 width, u32 height, u32 stride, Clock::time_point time,
                   std::string* error)
  {
    // Timestamps follow the wall clock so the video stays in sync with the audio output.
    const s64 pts = std::llround(std::chrono::duration<double>(time - m_start).count() * m_fps);
    if (pts <= m_last_video_pts)
      return true;

    FfmpegApi& api = GetFfmpegApi();
    if (!m_scaler || m_source_width != width || m_source_height != height)
    {
      api.p_sws_freeContext(m_scaler);
      m_scaler =
          api.p_sws_getContext(static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_RGBA,
                               m_video_context->width, m_video_context->height,
                               m_video_context->pix_fmt, m_scale_flags, nullptr, nullptr, nullptr);
      m_source_width = width;
      m_source_height = height;
      if (!m_scaler)
      {
        *error = "Could not create the video converter";
        return false;
      }
    }

    int result = api.p_av_frame_make_writable(m_video_frame);
    if (result < 0)
      return Fail("Could not acquire a video frame", result, error);

    const u8* const source_planes[4]{pixels, nullptr, nullptr, nullptr};
    const int source_strides[4]{static_cast<int>(stride), 0, 0, 0};
    result = api.p_sws_scale(m_scaler, source_planes, source_strides, 0, static_cast<int>(height),
                             m_video_frame->data, m_video_frame->linesize);
    if (result <= 0)
    {
      *error = "Video conversion produced no output";
      return false;
    }

    m_video_frame->pts = pts;
    m_last_video_pts = pts;
    return SendFrame(m_video_context, m_video_stream, m_video_frame, error);
  }

  bool EncodeAudio(const std::vector<s16>& samples, u32 sample_rate, std::string* error)
  {
    if (!m_audio_context || samples.size() < 2 || sample_rate == 0)
      return true;
    if (sample_rate != m_resampler_rate && !OpenResampler(sample_rate, error))
      return false;

    if (m_audio_pts < 0)
    {
      m_audio_pts = std::llround(std::chrono::duration<double>(Clock::now() - m_start).count() *
                                 AUDIO_SAMPLE_RATE);
    }

    FfmpegApi& api = GetFfmpegApi();
    const int frames = static_cast<int>(samples.size() / 2);
    const int capacity = static_cast<int>(s64{frames} * AUDIO_SAMPLE_RATE / sample_rate) + 256;
    const std::size_t offset = m_audio_left.size();
    m_audio_left.resize(offset + capacity);
    m_audio_right.resize(offset + capacity);
    u8* output[2]{reinterpret_cast<u8*>(m_audio_left.data() + offset),
                  reinterpret_cast<u8*>(m_audio_right.data() + offset)};
    const u8* input[1]{reinterpret_cast<const u8*>(samples.data())};
    const int converted = api.p_swr_convert(m_resampler, output, capacity, input, frames);
    m_audio_left.resize(offset + std::max(converted, 0));
    m_audio_right.resize(offset + std::max(converted, 0));
    if (converted < 0)
      return Fail("Could not convert the stream audio", converted, error);

    const std::size_t frame_size = static_cast<std::size_t>(m_audio_frame->nb_samples);
    std::size_t consumed = 0;
    bool success = true;
    while (success && m_audio_left.size() - consumed >= frame_size)
    {
      const int result = api.p_av_frame_make_writable(m_audio_frame);
      if (result < 0)
      {
        success = Fail("Could not acquire an audio frame", result, error);
        break;
      }
      std::memcpy(m_audio_frame->data[0], m_audio_left.data() + consumed,
                  frame_size * sizeof(float));
      std::memcpy(m_audio_frame->data[1], m_audio_right.data() + consumed,
                  frame_size * sizeof(float));
      m_audio_frame->pts = m_audio_pts;
      m_audio_pts += static_cast<s64>(frame_size);
      consumed += frame_size;
      success = SendFrame(m_audio_context, m_audio_stream, m_audio_frame, error);
    }
    m_audio_left.erase(m_audio_left.begin(), m_audio_left.begin() + consumed);
    m_audio_right.erase(m_audio_right.begin(), m_audio_right.begin() + consumed);
    return success;
  }

private:
  bool OpenVideo(const StreamSettings& settings, std::string* error)
  {
    FfmpegApi& api = GetFfmpegApi();
    const AVCodec* codec = api.p_avcodec_find_encoder_by_name("h264_mf");
    if (!codec)
    {
      *error = "The Media Foundation H.264 encoder is unavailable";
      return false;
    }
    m_video_context = api.p_avcodec_alloc_context3(codec);
    if (!m_video_context)
    {
      *error = "Could not allocate the H.264 encoder";
      return false;
    }
    const int fps = static_cast<int>(settings.fps);
    m_video_context->width = static_cast<int>(settings.width);
    m_video_context->height = static_cast<int>(settings.height);
    m_video_context->pix_fmt = AV_PIX_FMT_NV12;
    m_video_context->time_base = {1, fps};
    m_video_context->framerate = {fps, 1};
    m_video_context->bit_rate = s64{settings.bitrate_kbps} * 1000;
    // Companions that join a running stream wait at most a second for a key frame.
    m_video_context->gop_size = fps;
    m_video_context->max_b_frames = 0;
    if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
      m_video_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    AVDictionary* options = nullptr;
    api.p_av_dict_set(&options, "rate_control", "cbr", 0);
    api.p_av_dict_set(&options, "scenario", "live_streaming", 0);
    int result = api.p_avcodec_open2(m_video_context, codec, &options);
    api.p_av_dict_free(&options);
    if (result < 0)
      return Fail("Could not open the H.264 encoder", result, error);

    m_video_stream = api.p_avformat_new_stream(m_format, nullptr);
    if (!m_video_stream)
    {
      *error = "Could not create the video stream";
      return false;
    }
    m_video_stream->time_base = m_video_context->time_base;
    result = api.p_avcodec_parameters_from_context(m_video_stream->codecpar, m_video_context);
    if (result < 0)
      return Fail("Could not set the video stream parameters", result, error);

    m_video_frame = api.p_av_frame_alloc();
    m_packet = api.p_av_packet_alloc();
    if (!m_video_frame || !m_packet)
    {
      *error = "Could not allocate the video buffers";
      return false;
    }
    m_video_frame->format = m_video_context->pix_fmt;
    m_video_frame->width = m_video_context->width;
    m_video_frame->height = m_video_context->height;
    result = api.p_av_frame_get_buffer(m_video_frame, 0);
    if (result < 0)
      return Fail("Could not allocate the video frame", result, error);
    return true;
  }

  bool OpenAudio(std::string* error)
  {
    FfmpegApi& api = GetFfmpegApi();
    const AVCodec* codec = api.p_avcodec_find_encoder_by_name("aac");
    if (!codec)
    {
      *error = "The AAC encoder is unavailable";
      return false;
    }
    m_audio_context = api.p_avcodec_alloc_context3(codec);
    if (!m_audio_context)
    {
      *error = "Could not allocate the AAC encoder";
      return false;
    }
    m_audio_context->sample_rate = AUDIO_SAMPLE_RATE;
    m_audio_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_audio_context->time_base = {1, AUDIO_SAMPLE_RATE};
    m_audio_context->bit_rate = 128000;
    api.p_av_channel_layout_default(&m_audio_context->ch_layout, 2);
    if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
      m_audio_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int result = api.p_avcodec_open2(m_audio_context, codec, nullptr);
    if (result < 0)
      return Fail("Could not open the AAC encoder", result, error);

    m_audio_stream = api.p_avformat_new_stream(m_format, nullptr);
    if (!m_audio_stream)
    {
      *error = "Could not create the audio stream";
      return false;
    }
    m_audio_stream->time_base = m_audio_context->time_base;
    result = api.p_avcodec_parameters_from_context(m_audio_stream->codecpar, m_audio_context);
    if (result < 0)
      return Fail("Could not set the audio stream parameters", result, error);

    m_audio_frame = api.p_av_frame_alloc();
    if (!m_audio_frame)
    {
      *error = "Could not allocate the audio frame";
      return false;
    }
    m_audio_frame->format = m_audio_context->sample_fmt;
    m_audio_frame->sample_rate = m_audio_context->sample_rate;
    m_audio_frame->nb_samples = std::max(m_audio_context->frame_size, 1);
    result = api.p_av_channel_layout_copy(&m_audio_frame->ch_layout, &m_audio_context->ch_layout);
    if (result < 0)
      return Fail("Could not set the audio channel layout", result, error);
    result = api.p_av_frame_get_buffer(m_audio_frame, 0);
    if (result < 0)
      return Fail("Could not allocate the audio buffers", result, error);
    return true;
  }

  bool OpenResampler(u32 sample_rate, std::string* error)
  {
    FfmpegApi& api = GetFfmpegApi();
    api.p_swr_free(&m_resampler);
    m_resampler_rate = 0;

    AVChannelLayout source_layout{};
    api.p_av_channel_layout_default(&source_layout, 2);
    int result = api.p_swr_alloc_set_opts2(
        &m_resampler, &m_audio_context->ch_layout, m_audio_context->sample_fmt, AUDIO_SAMPLE_RATE,
        &source_layout, AV_SAMPLE_FMT_S16, static_cast<int>(sample_rate), 0, nullptr);
    api.p_av_channel_layout_uninit(&source_layout);
    if (result < 0 || !m_resampler)
      return Fail("Could not create the audio converter", result, error);
    result = api.p_swr_init(m_resampler);
    if (result < 0)
      return Fail("Could not initialize the audio converter", result, error);
    m_resampler_rate = sample_rate;
    return true;
  }

  bool SendFrame(AVCodecContext* codec, AVStream* stream, AVFrame* frame, std::string* error)
  {
    FfmpegApi& api = GetFfmpegApi();
    int result = api.p_avcodec_send_frame(codec, frame);
    if (result < 0)
      return Fail("The encoder rejected a frame", result, error);
    while (true)
    {
      result = api.p_avcodec_receive_packet(codec, m_packet);
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
        return true;
      if (result < 0)
        return Fail("Could not receive an encoded packet", result, error);
      api.p_av_packet_rescale_ts(m_packet, codec->time_base, stream->time_base);
      m_packet->stream_index = stream->index;
      result = api.p_av_interleaved_write_frame(m_format, m_packet);
      api.p_av_packet_unref(m_packet);
      if (result < 0)
        return Fail("Could not send stream data", result, error);
    }
  }

  void Close()
  {
    FfmpegApi& api = GetFfmpegApi();
    if (m_header_written)
    {
      std::string ignored;
      SendFrame(m_video_context, m_video_stream, nullptr, &ignored);
      if (m_audio_context)
        SendFrame(m_audio_context, m_audio_stream, nullptr, &ignored);
      api.p_av_write_trailer(m_format);
      m_header_written = false;
    }
    if (m_format && m_format->pb)
    {
      api.p_avio_flush(m_format->pb);
      api.p_av_freep(&m_format->pb->buffer);
      api.p_avio_context_free(&m_format->pb);
    }
    if (m_scaler)
      api.p_sws_freeContext(std::exchange(m_scaler, nullptr));
    if (m_resampler)
      api.p_swr_free(&m_resampler);
    if (m_video_frame)
      api.p_av_frame_free(&m_video_frame);
    if (m_audio_frame)
      api.p_av_frame_free(&m_audio_frame);
    if (m_packet)
      api.p_av_packet_free(&m_packet);
    if (m_video_context)
      api.p_avcodec_free_context(&m_video_context);
    if (m_audio_context)
      api.p_avcodec_free_context(&m_audio_context);
    if (m_format)
      api.p_avformat_free_context(std::exchange(m_format, nullptr));
    m_video_stream = nullptr;
    m_audio_stream = nullptr;
  }

  AVFormatContext* m_format = nullptr;
  AVCodecContext* m_video_context = nullptr;
  AVCodecContext* m_audio_context = nullptr;
  AVStream* m_video_stream = nullptr;
  AVStream* m_audio_stream = nullptr;
  AVFrame* m_video_frame = nullptr;
  AVFrame* m_audio_frame = nullptr;
  AVPacket* m_packet = nullptr;
  SwsContext* m_scaler = nullptr;
  SwrContext* m_resampler = nullptr;
  bool m_header_written = false;
  int m_scale_flags = SWS_BILINEAR;

  u32 m_fps = DEFAULT_FPS;
  Clock::time_point m_start;
  u32 m_source_width = 0;
  u32 m_source_height = 0;
  s64 m_last_video_pts = -1;

  u32 m_resampler_rate = 0;
  s64 m_audio_pts = -1;
  std::vector<float> m_audio_left;
  std::vector<float> m_audio_right;
};

struct Destination
{
  u32 peer = 0;
  u8 screen = 0;
  u32 stream_type = 0;
  std::string url;
  AVIOContext* io = nullptr;
};

AVIOContext* OpenDestination(const std::string& url, std::string* error)
{
  FfmpegApi& api = GetFfmpegApi();
  if (!api.Load())
  {
    *error = "The packaged FFmpeg runtime could not be loaded";
    return nullptr;
  }
  AVIOContext* io = nullptr;
  AVDictionary* options = nullptr;
  api.p_av_dict_set(&options, "buffer_size", "65536", 0);
  const int result = api.p_avio_open2(&io, url.c_str(), AVIO_FLAG_WRITE, nullptr, &options);
  api.p_av_dict_free(&options);
  if (result < 0)
  {
    Fail("Could not open the stream target", result, error);
    return nullptr;
  }
  return io;
}

void CloseDestination(Destination& destination)
{
  if (destination.io)
    GetFfmpegApi().p_avio_closep(&destination.io);
}

// Guards the sessions and everything the video, audio and encoder threads exchange with them.
std::mutex s_mutex;

// One encoder for a source, with the companions that watch it.
class Session
{
public:
  Session(Source source, const StreamSettings& settings) : m_source(source), m_settings(settings) {}

  ~Session()
  {
    Stop();
    for (Destination& destination : m_destinations)
      CloseDestination(destination);
  }

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  bool Start(std::string* error)
  {
    std::promise<std::optional<std::string>> opened;
    std::future<std::optional<std::string>> result = opened.get_future();
    m_worker = std::thread(&Session::WorkerMain, this, std::move(opened));
    if (std::optional<std::string> open_error = result.get())
    {
      m_worker.join();
      *error = std::move(*open_error);
      return false;
    }
    m_next_capture = Clock::now();
    return true;
  }

  void Stop()
  {
    {
      std::lock_guard lock(s_mutex);
      m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable())
      m_worker.join();
  }

  Source GetSource() const { return m_source; }
  const StreamSettings& GetSettings() const { return m_settings; }

  void AddDestination(Destination destination)
  {
    std::lock_guard lock(m_destinations_mutex);
    m_destinations.push_back(std::move(destination));
  }

  bool RemoveDestination(u32 peer, u8 screen)
  {
    std::optional<Destination> removed;
    {
      std::lock_guard lock(m_destinations_mutex);
      const auto it = std::ranges::find_if(m_destinations, [&](const Destination& destination) {
        return destination.peer == peer && destination.screen == screen;
      });
      if (it == m_destinations.end())
        return false;
      removed = std::move(*it);
      m_destinations.erase(it);
    }
    CloseDestination(*removed);
    return true;
  }

  std::optional<Destination> FindDestination(u32 peer, u8 screen)
  {
    std::lock_guard lock(m_destinations_mutex);
    for (const Destination& destination : m_destinations)
    {
      if (destination.peer == peer && destination.screen == screen)
        return Destination{destination.peer, destination.screen, destination.stream_type,
                           destination.url};
    }
    return std::nullopt;
  }

  std::vector<std::pair<u32, u8>> GetWatchers()
  {
    std::vector<std::pair<u32, u8>> watchers;
    std::lock_guard lock(m_destinations_mutex);
    for (const Destination& destination : m_destinations)
      watchers.emplace_back(destination.peer, destination.screen);
    return watchers;
  }

  bool IsWatched()
  {
    std::lock_guard lock(m_destinations_mutex);
    return !m_destinations.empty();
  }

  // These run with s_mutex held.
  bool IsAcceptingInput() const { return !m_stop; }
  std::optional<std::string> GetFailureLocked() const { return m_failure; }

  std::optional<std::pair<u32, u32>> RequestFrameLocked()
  {
    // Presents arrive with some jitter, so accept them slightly ahead of the frame interval.
    const Clock::time_point now = Clock::now();
    const auto interval = FrameInterval();
    const auto tolerance = interval / 4;
    if (now + tolerance < m_next_capture)
      return std::nullopt;

    m_next_capture = std::max(m_next_capture + interval, now - tolerance);
    m_capture_time = now;
    return std::pair{m_settings.width, m_settings.height};
  }

  std::vector<u8> TakeSparePixelsLocked() { return std::move(m_spare_pixels); }
  Clock::time_point GetCaptureTimeLocked() const { return m_capture_time; }

  void SubmitFrameLocked(VideoFrame frame)
  {
    if (m_latest_frame)
      m_spare_pixels = std::move(m_latest_frame->pixels);
    m_latest_frame = std::move(frame);
    m_wake.notify_one();
  }

  void SubmitAudioLocked(const s16* samples, std::size_t num_samples, u32 sample_rate)
  {
    if (!m_settings.audio || sample_rate == 0)
      return;
    if (sample_rate != m_audio_rate)
    {
      m_audio_samples.clear();
      m_audio_rate = sample_rate;
    }
    m_audio_samples.insert(m_audio_samples.end(), samples, samples + num_samples * 2);
    if (m_audio_samples.size() > MAX_QUEUED_AUDIO_SAMPLES)
    {
      m_audio_samples.erase(m_audio_samples.begin(),
                            m_audio_samples.end() - MAX_QUEUED_AUDIO_SAMPLES);
    }
    m_wake.notify_one();
  }

private:
  static int WritePacket(void* opaque, uint8_t* data, int size)
  {
    static_cast<Session*>(opaque)->Send(data, size);
    return size;
  }

  void Send(const u8* data, int size)
  {
    FfmpegApi& api = GetFfmpegApi();
    std::lock_guard lock(m_destinations_mutex);
    for (Destination& destination : m_destinations)
    {
      api.p_avio_write(destination.io, data, size);
      api.p_avio_flush(destination.io);
    }
  }

  Clock::duration FrameInterval() const
  {
    return std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / m_settings.fps));
  }

  std::optional<std::size_t> GBAPort() const
  {
    if (m_source == Source::Game)
      return std::nullopt;
    return static_cast<std::size_t>(m_source) - static_cast<std::size_t>(Source::GBA1);
  }

  // The Media Foundation encoder must be created and used on a multithreaded COM apartment.
  void WorkerMain(std::promise<std::optional<std::string>> opened)
  {
    Common::SetCurrentThreadName(GBAPort() ? "GBA streamer" : "Media streamer");
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
      Encoder encoder;
      std::string error;
      if (encoder.Open(m_settings, GBAPort() ? SWS_POINT : SWS_BILINEAR, this, &WritePacket,
                       &error))
      {
        opened.set_value(std::nullopt);
        EncodeLoop(encoder);
      }
      else
      {
        opened.set_value(std::move(error));
      }
    }
    if (SUCCEEDED(com_result))
      CoUninitialize();
  }

  void EncodeLoop(Encoder& encoder)
  {
    const std::optional<std::size_t> gba_port = GBAPort();
    std::vector<s16> audio;
    std::vector<u32> gba_pixels;
    u64 gba_frame = 0;
    Clock::time_point next_gba_capture = Clock::now();
    while (true)
    {
      std::optional<VideoFrame> video;
      u32 audio_rate = 0;
      {
        std::unique_lock lock(s_mutex);
        const auto ready = [this] {
          return m_stop || m_latest_frame.has_value() || !m_audio_samples.empty();
        };
        if (gba_port)
          m_wake.wait_until(lock, next_gba_capture, ready);
        else
          m_wake.wait(lock, ready);
        if (m_stop)
          break;
        video = std::exchange(m_latest_frame, std::nullopt);
        audio.swap(m_audio_samples);
        audio_rate = m_audio_rate;
      }

      // The GBA screens are sampled at the stream rate.
      const Clock::time_point now = Clock::now();
      if (gba_port && now >= next_gba_capture)
      {
        next_gba_capture = std::max(next_gba_capture + FrameInterval(), now);
        const u64 previous_frame = gba_frame;
        u32 width = 0;
        u32 height = 0;
        if (GBAScreens::CopyScreen(*gba_port, &gba_frame, &width, &height, &gba_pixels) &&
            gba_frame != previous_frame &&
            gba_pixels.size() == static_cast<std::size_t>(width) * height)
        {
          video = VideoFrame{{}, width, height, width * 4, now};
        }
      }

      std::string error;
      bool success = true;
      if (video)
      {
        const u8* const pixels =
            gba_port ? reinterpret_cast<const u8*>(gba_pixels.data()) : video->pixels.data();
        success = encoder.EncodeVideo(pixels, video->width, video->height, video->stride,
                                      video->time, &error);
      }
      if (success && !audio.empty())
        success = encoder.EncodeAudio(audio, audio_rate, &error);
      audio.clear();

      std::lock_guard lock(s_mutex);
      if (video && !gba_port && m_spare_pixels.capacity() == 0)
        m_spare_pixels = std::move(video->pixels);
      if (!success)
      {
        ERROR_LOG_FMT(FRAMEDUMP, "The media stream stopped: {}", error);
        m_failure = std::move(error);
        m_stop = true;
        break;
      }
    }
  }

  const Source m_source;
  const StreamSettings m_settings;
  std::thread m_worker;
  std::condition_variable m_wake;

  std::mutex m_destinations_mutex;
  std::vector<Destination> m_destinations;

  // Guarded by s_mutex.
  bool m_stop = false;
  std::optional<std::string> m_failure;
  std::optional<VideoFrame> m_latest_frame;
  std::vector<u8> m_spare_pixels;
  Clock::time_point m_next_capture;
  Clock::time_point m_capture_time;
  std::vector<s16> m_audio_samples;
  u32 m_audio_rate = 0;
};

std::mutex s_control_mutex;
std::array<std::unique_ptr<Session>, SOURCE_COUNT> s_sessions;
std::map<std::pair<u32, u8>, std::string> s_failures;
std::atomic<u32> s_streamed_sources{0};

Session* GetLiveSessionLocked(Source source)
{
  Session* const session = s_sessions[static_cast<std::size_t>(source)].get();
  return session && session->IsAcceptingInput() ? session : nullptr;
}

class VideoSink final : public FrameDumper::StreamSink
{
public:
  std::optional<std::pair<u32, u32>> RequestFrame() override
  {
    std::lock_guard lock(s_mutex);
    Session* const session = GetLiveSessionLocked(Source::Game);
    return session ? session->RequestFrameLocked() : std::nullopt;
  }

  void SubmitFrame(const u8* data, u32 width, u32 height, u32 stride) override
  {
    std::vector<u8> pixels;
    Clock::time_point time;
    {
      std::lock_guard lock(s_mutex);
      Session* const session = GetLiveSessionLocked(Source::Game);
      if (!session)
        return;
      pixels = session->TakeSparePixelsLocked();
      time = session->GetCaptureTimeLocked();
    }

    const std::size_t row_size = std::size_t{width} * 4;
    pixels.resize(row_size * height);
    for (u32 y = 0; y < height; ++y)
      std::memcpy(pixels.data() + row_size * y, data + std::size_t{stride} * y, row_size);

    std::lock_guard lock(s_mutex);
    if (Session* const session = GetLiveSessionLocked(Source::Game))
      session->SubmitFrameLocked(
          {std::move(pixels), width, height, static_cast<u32>(row_size), time});
  }
};

VideoSink s_video_sink;

void SubmitGameAudio(const s16* samples, std::size_t num_samples, u32 sample_rate)
{
  std::lock_guard lock(s_mutex);
  if (Session* const session = GetLiveSessionLocked(Source::Game))
    session->SubmitAudioLocked(samples, num_samples, sample_rate);
}

void SubmitGBAAudio(std::size_t device_number, const s16* samples, std::size_t num_samples,
                    u32 sample_rate)
{
  if (device_number >= GBAScreens::MAX_SCREENS)
    return;
  std::lock_guard lock(s_mutex);
  if (Session* const session = GetLiveSessionLocked(GBASource(device_number)))
    session->SubmitAudioLocked(samples, num_samples, sample_rate);
}

// Routes the frames and the sound to the sessions that exist.
void UpdateTaps()
{
  u32 streamed = 0;
  u32 gba_audio = 0;
  bool game_audio = false;
  for (std::size_t i = 0; i < SOURCE_COUNT; ++i)
  {
    const Session* const session = s_sessions[i].get();
    if (!session)
      continue;
    streamed |= 1u << i;
    if (!session->GetSettings().audio)
      continue;
    if (session->GetSource() == Source::Game)
      game_audio = true;
    else
      gba_audio |= 1u << (i - static_cast<std::size_t>(Source::GBA1));
  }
  s_streamed_sources.store(streamed);

  FrameDumper::SetStreamSink((streamed & 1) != 0 ? &s_video_sink : nullptr);
  Mixer::SetOutputTap(game_audio ? Mixer::OutputTap(SubmitGameAudio) : Mixer::OutputTap());
  Mixer::SetGBATap(gba_audio != 0 ? Mixer::GBATap(SubmitGBAAudio) : Mixer::GBATap(), gba_audio);
}

void DestroySession(Source source)
{
  std::unique_ptr<Session> session;
  {
    std::lock_guard lock(s_mutex);
    session = std::move(s_sessions[static_cast<std::size_t>(source)]);
  }
  UpdateTaps();
  session.reset();
}

// Ends sessions whose encoder failed and keeps the failure for each companion that watched them.
void ReapFailedSessions()
{
  for (std::size_t i = 0; i < SOURCE_COUNT; ++i)
  {
    Session* const session = s_sessions[i].get();
    if (!session)
      continue;
    std::optional<std::string> failure;
    {
      std::lock_guard lock(s_mutex);
      failure = session->GetFailureLocked();
    }
    if (!failure)
      continue;
    for (const auto& watcher : session->GetWatchers())
      s_failures[watcher] = *failure;
    DestroySession(static_cast<Source>(i));
  }
}

void RemoveDestination(u32 peer, u8 screen)
{
  for (std::size_t i = 0; i < SOURCE_COUNT; ++i)
  {
    Session* const session = s_sessions[i].get();
    if (!session || !session->RemoveDestination(peer, screen))
      continue;
    if (!session->IsWatched())
      DestroySession(static_cast<Source>(i));
    return;
  }
}

StreamSettings MakeSettings(Source source, const Frontend::StreamRequest& request)
{
  StreamSettings settings;
  settings.fps =
      std::clamp(request.framesPerSecond ? request.framesPerSecond : DEFAULT_FPS, 1u, 60u);
  settings.bitrate_kbps =
      request.bitrateKbps ? std::max(request.bitrateKbps, MIN_BITRATE_KBPS) : DEFAULT_BITRATE_KBPS;
  settings.audio = request.audioEnabled;
  if (source == Source::Game)
  {
    settings.width = std::clamp((request.width ? request.width : DEFAULT_WIDTH) & ~1u, 16u, 4096u);
    settings.height =
        std::clamp((request.height ? request.height : DEFAULT_HEIGHT) & ~1u, 16u, 4096u);
    return settings;
  }

  // GBA screens keep square pixels at the largest whole scale that fits the requested size.
  u32 scale = DEFAULT_GBA_SCALE;
  if (request.width && request.height)
    scale = std::min(request.width / GBA_WIDTH, request.height / GBA_HEIGHT);
  scale = std::clamp(scale, 1u, MAX_GBA_SCALE);
  settings.width = GBA_WIDTH * scale;
  settings.height = GBA_HEIGHT * scale;
  return settings;
}
}  // namespace

bool Start(u32 peer, u8 screen, Source source, const Frontend::StreamRequest& request,
           std::string* error)
{
  std::lock_guard control_lock(s_control_mutex);
  ReapFailedSessions();
  RemoveDestination(peer, screen);
  s_failures.erase({peer, screen});

  if (!request.url.starts_with("udp://"))
  {
    *error = "Only UDP stream targets are supported";
    return false;
  }

  AVIOContext* const io = OpenDestination(request.url, error);
  if (!io)
    return false;
  Destination destination{peer, screen, request.streamType, request.url, io};

  std::unique_ptr<Session>& slot = s_sessions[static_cast<std::size_t>(source)];
  if (!slot)
  {
    auto session = std::make_unique<Session>(source, MakeSettings(source, request));
    if (!session->Start(error))
    {
      CloseDestination(destination);
      return false;
    }
    session->AddDestination(std::move(destination));
    {
      std::lock_guard lock(s_mutex);
      slot = std::move(session);
    }
    UpdateTaps();
  }
  else
  {
    slot->AddDestination(std::move(destination));
  }

  const StreamSettings& settings = slot->GetSettings();
  INFO_LOG_FMT(FRAMEDUMP, "Streaming source {} at {}x{}, {} fps and {} kbps to {}",
               static_cast<int>(source), settings.width, settings.height, settings.fps,
               settings.bitrate_kbps, request.url);
  return true;
}

void Stop(u32 peer, u8 screen)
{
  std::lock_guard control_lock(s_control_mutex);
  ReapFailedSessions();
  RemoveDestination(peer, screen);
}

void StopAll()
{
  std::lock_guard control_lock(s_control_mutex);
  for (std::size_t i = 0; i < SOURCE_COUNT; ++i)
  {
    if (s_sessions[i])
      DestroySession(static_cast<Source>(i));
  }
  s_failures.clear();
}

std::optional<Frontend::ActiveStream> GetActiveStream(u32 peer, u8 screen)
{
  std::lock_guard control_lock(s_control_mutex);
  ReapFailedSessions();
  for (const std::unique_ptr<Session>& session : s_sessions)
  {
    if (!session)
      continue;
    const std::optional<Destination> destination = session->FindDestination(peer, screen);
    if (!destination)
      continue;
    const StreamSettings& settings = session->GetSettings();
    return Frontend::ActiveStream{
        .secondaryScreen = screen != 0,
        .streamType = destination->stream_type,
        .url = destination->url,
        .width = settings.width,
        .height = settings.height,
        .framesPerSecond = settings.fps,
        .audioEnabled = settings.audio,
        .screenId = screen,
    };
  }
  return std::nullopt;
}

std::optional<std::string> TakeFailure(u32 peer, u8 screen)
{
  std::lock_guard control_lock(s_control_mutex);
  ReapFailedSessions();
  const auto it = s_failures.find({peer, screen});
  if (it == s_failures.end())
    return std::nullopt;
  std::string failure = std::move(it->second);
  s_failures.erase(it);
  return failure;
}

bool IsStreaming(Source source)
{
  return (s_streamed_sources.load() & (1u << static_cast<u32>(source))) != 0;
}
}  // namespace DolphinUWP::MediaStreamer

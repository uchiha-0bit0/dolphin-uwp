// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/XAudio2Stream.h"

#include <thread>

#include <mmreg.h>
#include <xaudio2.h>

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Devices.h>

#include "AudioCommon/Mixer.h"
#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/System.h"

namespace
{
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
constexpr GUID SUBTYPE_IEEE_FLOAT{
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
}  // namespace

class XAudio2Stream::VoiceCallback final : public IXAudio2VoiceCallback
{
public:
  explicit VoiceCallback(XAudio2Stream& stream) : m_stream(stream) {}

  void STDMETHODCALLTYPE OnBufferEnd(void*) override
  {
    if (m_stream.m_running.load(std::memory_order_acquire))
      m_stream.SubmitBuffer();
  }

  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}

  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override
  {
    ERROR_LOG_FMT(AUDIO, "XAudio2 voice error: {}", Common::HRWrap(error));
  }

private:
  XAudio2Stream& m_stream;
};

class XAudio2Stream::RoutedOutput final : public IXAudio2VoiceCallback
{
public:
  using Mix = std::function<void(s16* samples, u32 frames)>;

  // An empty device ID is the default output device.
  static std::unique_ptr<RoutedOutput> Create(const std::string& device_id, u32 sample_rate,
                                              Mix mix)
  {
    auto output = std::unique_ptr<RoutedOutput>(new RoutedOutput(std::move(mix)));
    HRESULT hr = XAudio2Create(output->m_xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
    {
      const std::wstring device = UTF8ToWString(device_id);
      hr = output->m_xaudio2->CreateMasteringVoice(
          &output->m_mastering_voice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0,
          device.empty() ? nullptr : device.c_str());
    }
    if (SUCCEEDED(hr))
    {
      WAVEFORMATEX format{};
      format.wFormatTag = WAVE_FORMAT_PCM;
      format.nChannels = CHANNEL_COUNT;
      format.nSamplesPerSec = sample_rate;
      format.wBitsPerSample = 16;
      format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
      format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
      hr = output->m_xaudio2->CreateSourceVoice(&output->m_source_voice, &format, 0,
                                                XAUDIO2_DEFAULT_FREQ_RATIO, output.get());
    }
    if (FAILED(hr))
    {
      ERROR_LOG_FMT(AUDIO, "Audio routing to {} failed: {}",
                    device_id.empty() ? "the default device" : device_id, Common::HRWrap(hr));
      return nullptr;
    }
    return output;
  }

  ~RoutedOutput()
  {
    m_running.store(false, std::memory_order_release);
    if (m_source_voice)
    {
      m_source_voice->Stop();
      m_source_voice->FlushSourceBuffers();
      m_source_voice->DestroyVoice();
    }
    if (m_mastering_voice)
      m_mastering_voice->DestroyVoice();
    m_xaudio2.Reset();
  }

  void SetRunning(bool running)
  {
    if (running == m_running.load(std::memory_order_acquire))
      return;
    m_running.store(running, std::memory_order_release);
    if (running)
    {
      for (u32 i = 0; i < BUFFER_COUNT; ++i)
        Submit();
      m_source_voice->Start();
      return;
    }
    m_source_voice->Stop();
    m_source_voice->FlushSourceBuffers();
  }

  void STDMETHODCALLTYPE OnBufferEnd(void*) override
  {
    if (m_running.load(std::memory_order_acquire))
      Submit();
  }

  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}

  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override
  {
    ERROR_LOG_FMT(AUDIO, "XAudio2 routed voice error: {}", Common::HRWrap(error));
  }

private:
  explicit RoutedOutput(Mix mix) : m_mix(std::move(mix)) {}

  void Submit()
  {
    auto& buffer = m_buffers[m_next_buffer];
    m_next_buffer = (m_next_buffer + 1) % BUFFER_COUNT;
    m_mix(buffer.data(), BUFFER_FRAMES);
    XAUDIO2_BUFFER xaudio2_buffer{};
    xaudio2_buffer.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(s16));
    xaudio2_buffer.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
    m_source_voice->SubmitSourceBuffer(&xaudio2_buffer);
  }

  Mix m_mix;
  Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
  IXAudio2MasteringVoice* m_mastering_voice = nullptr;
  IXAudio2SourceVoice* m_source_voice = nullptr;
  std::array<std::array<s16, BUFFER_FRAMES * CHANNEL_COUNT>, BUFFER_COUNT> m_buffers{};
  u32 m_next_buffer = 0;
  std::atomic<bool> m_running = false;
};

XAudio2Stream::XAudio2Stream() : m_callback(std::make_unique<VoiceCallback>(*this))
{
}

std::vector<std::pair<std::string, std::string>> XAudio2Stream::ListOutputDevices()
{
  namespace Enumeration = winrt::Windows::Devices::Enumeration;

  // Windows lists the devices asynchronously, which a single-threaded apartment can't wait for.
  std::vector<std::pair<std::string, std::string>> devices;
  std::thread([&devices] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try
    {
      const Enumeration::DeviceInformationCollection collection =
          Enumeration::DeviceInformation::FindAllAsync(
              winrt::Windows::Media::Devices::MediaDevice::GetAudioRenderSelector())
              .get();
      for (const Enumeration::DeviceInformation& device : collection)
      {
        if (device.IsEnabled())
          devices.emplace_back(winrt::to_string(device.Id()), winrt::to_string(device.Name()));
      }
    }
    catch (const winrt::hresult_error& error)
    {
      ERROR_LOG_FMT(AUDIO, "The audio output devices could not be listed: {}",
                    winrt::to_string(error.message()));
    }
    winrt::uninit_apartment();
  }).join();
  return devices;
}

XAudio2Stream::~XAudio2Stream()
{
  DestroyVoices();
}

bool XAudio2Stream::Init()
{
  HRESULT hr = XAudio2Create(m_xaudio2.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(AUDIO, "XAudio2Create failed: {}", Common::HRWrap(hr));
    return false;
  }

  hr = m_xaudio2->CreateMasteringVoice(&m_mastering_voice);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(AUDIO, "CreateMasteringVoice failed: {}", Common::HRWrap(hr));
    DestroyVoices();
    return false;
  }

  m_surround = Config::ShouldUseDPL2Decoder() && CreateSourceVoice(true);
  if (!m_surround && !CreateSourceVoice(false))
  {
    DestroyVoices();
    return false;
  }
  CreateRoutedOutputs();
  return true;
}

void XAudio2Stream::CreateRoutedOutputs()
{
  const u32 sample_rate = m_mixer->GetSampleRate();
  // A device that is gone falls back to the default device.
  const auto add = [this, sample_rate](const std::string& device, RoutedOutput::Mix mix) {
    std::unique_ptr<RoutedOutput> output = RoutedOutput::Create(device, sample_rate, mix);
    if (!output && !device.empty())
      output = RoutedOutput::Create({}, sample_rate, std::move(mix));
    if (output)
      m_routed_outputs.push_back(std::move(output));
  };

  if (Core::System::GetInstance().IsWii() &&
      Config::Get(Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED))
  {
    for (u32 i = 0; i < Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED.size(); ++i)
    {
      if (!Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]))
        continue;
      add(Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_DEVICE[i]),
          [this, i](s16* samples, u32 frames) { m_mixer->MixWiimoteSpeaker(i, samples, frames); });
    }
  }

  if (Config::Get(Config::MAIN_GBA_AUDIO_ROUTING_ENABLED))
  {
    for (u32 i = 0; i < Config::MAIN_GBA_AUDIO_OUTPUT_ENABLED.size(); ++i)
    {
      if (!Config::Get(Config::MAIN_GBA_AUDIO_OUTPUT_ENABLED[i]))
        continue;
      add(Config::Get(Config::MAIN_GBA_AUDIO_OUTPUT_DEVICE[i]),
          [this, i](s16* samples, u32 frames) { m_mixer->MixGBA(i, samples, frames); });
    }
  }
}

bool XAudio2Stream::CreateSourceVoice(bool surround)
{
  WAVEFORMATEXTENSIBLE format{};
  format.Format.nChannels = surround ? SURROUND_CHANNEL_COUNT : CHANNEL_COUNT;
  format.Format.nSamplesPerSec = m_mixer->GetSampleRate();
  if (surround)
  {
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.wBitsPerSample = 32;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                           SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    format.SubFormat = SUBTYPE_IEEE_FLOAT;
  }
  else
  {
    format.Format.wFormatTag = WAVE_FORMAT_PCM;
    format.Format.wBitsPerSample = 16;
  }
  format.Format.nBlockAlign = format.Format.nChannels * format.Format.wBitsPerSample / 8;
  format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;

  const HRESULT hr = m_xaudio2->CreateSourceVoice(&m_source_voice, &format.Format, 0,
                                                  XAUDIO2_DEFAULT_FREQ_RATIO, m_callback.get());
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(AUDIO, "CreateSourceVoice ({} channels) failed: {}", format.Format.nChannels,
                  Common::HRWrap(hr));
    m_source_voice = nullptr;
    return false;
  }
  return true;
}

bool XAudio2Stream::SetRunning(bool running)
{
  if (!m_source_voice)
    return false;

  if (running == m_running.load(std::memory_order_acquire))
    return true;

  for (const std::unique_ptr<RoutedOutput>& output : m_routed_outputs)
    output->SetRunning(running);

  if (running)
  {
    m_running.store(true, std::memory_order_release);
    for (u32 i = 0; i < BUFFER_COUNT; ++i)
      SubmitBuffer();
    return SUCCEEDED(m_source_voice->Start());
  }

  m_running.store(false, std::memory_order_release);
  const HRESULT hr = m_source_voice->Stop();
  m_source_voice->FlushSourceBuffers();
  return SUCCEEDED(hr);
}

void XAudio2Stream::SetVolume(int volume)
{
  if (m_source_voice)
    m_source_voice->SetVolume(volume / 100.0f);
}

void XAudio2Stream::SubmitBuffer()
{
  const u32 index = m_next_buffer;
  m_next_buffer = (m_next_buffer + 1) % BUFFER_COUNT;

  XAUDIO2_BUFFER xaudio2_buffer{};
  if (m_surround)
  {
    auto& buffer = m_surround_buffers[index];
    m_mixer->MixSurround(buffer.data(), BUFFER_FRAMES);
    xaudio2_buffer.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(float));
    xaudio2_buffer.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
  }
  else
  {
    auto& buffer = m_buffers[index];
    m_mixer->Mix(buffer.data(), BUFFER_FRAMES);
    xaudio2_buffer.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(s16));
    xaudio2_buffer.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
  }
  m_source_voice->SubmitSourceBuffer(&xaudio2_buffer);
}

void XAudio2Stream::DestroyVoices()
{
  m_routed_outputs.clear();
  m_running.store(false, std::memory_order_release);
  if (m_source_voice)
  {
    m_source_voice->Stop();
    m_source_voice->FlushSourceBuffers();
    m_source_voice->DestroyVoice();
    m_source_voice = nullptr;
  }
  if (m_mastering_voice)
  {
    m_mastering_voice->DestroyVoice();
    m_mastering_voice = nullptr;
  }
  m_xaudio2.Reset();
}

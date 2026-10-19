// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "AudioCommon/SoundStream.h"

#ifdef DOLPHIN_UWP
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <wrl/client.h>

#include "Common/CommonTypes.h"

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;
#endif

class XAudio2Stream final : public SoundStream
{
#ifdef DOLPHIN_UWP
public:
  XAudio2Stream();
  ~XAudio2Stream() override;

  bool Init() override;
  bool SetRunning(bool running) override;
  void SetVolume(int volume) override;

  static bool IsValid() { return true; }

  // Pairs of device ID and name. Blocks while Windows lists them.
  static std::vector<std::pair<std::string, std::string>> ListOutputDevices();

private:
  static constexpr u32 CHANNEL_COUNT = 2;
  static constexpr u32 SURROUND_CHANNEL_COUNT = 6;
  static constexpr u32 BUFFER_FRAMES = 512;
  static constexpr u32 BUFFER_COUNT = 3;

  class VoiceCallback;
  class RoutedOutput;

  bool CreateSourceVoice(bool surround);
  // Plays each Wii Remote speaker and GBA that the audio routing settings name on its own device.
  void CreateRoutedOutputs();
  void SubmitBuffer();
  void DestroyVoices();

  std::unique_ptr<VoiceCallback> m_callback;
  Microsoft::WRL::ComPtr<IXAudio2> m_xaudio2;
  IXAudio2MasteringVoice* m_mastering_voice = nullptr;
  IXAudio2SourceVoice* m_source_voice = nullptr;
  std::array<std::array<s16, BUFFER_FRAMES * CHANNEL_COUNT>, BUFFER_COUNT> m_buffers{};
  std::array<std::array<float, BUFFER_FRAMES * SURROUND_CHANNEL_COUNT>, BUFFER_COUNT>
      m_surround_buffers{};
  u32 m_next_buffer = 0;
  bool m_surround = false;
  std::atomic<bool> m_running = false;
  std::vector<std::unique_ptr<RoutedOutput>> m_routed_outputs;
#endif
};

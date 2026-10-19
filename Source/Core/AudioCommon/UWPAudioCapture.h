// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <thread>

#include "Common/CommonTypes.h"

namespace AudioCommon
{
// Captures mono 16-bit samples from the default microphone of a UWP app.
class UWPAudioCapture
{
public:
  using SampleCallback = std::function<void(const s16* samples, u32 count)>;
  using ErrorCallback = std::function<void(std::string message)>;

  UWPAudioCapture() = default;
  ~UWPAudioCapture();
  UWPAudioCapture(const UWPAudioCapture&) = delete;
  UWPAudioCapture& operator=(const UWPAudioCapture&) = delete;

  // Captures on a thread of its own. The first use asks the user for access to the microphone.
  void Start(u32 sample_rate, SampleCallback on_samples, ErrorCallback on_error = {});
  void Stop();

private:
  void Run(u32 sample_rate);

  SampleCallback m_on_samples;
  ErrorCallback m_on_error;
  std::thread m_thread;
  void* m_stop_event = nullptr;
};
}  // namespace AudioCommon

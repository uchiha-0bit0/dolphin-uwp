// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "AudioCommon/UWPAudioCapture.h"

#include <memory>
#include <vector>

#include <unknwn.h>

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.UI.Core.h>

#include <fmt/format.h>

#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/Thread.h"

#pragma warning(push)
// C++/WinRT templates instantiated with the classes and lambdas below.
#pragma warning(disable : 4265 4946)

namespace AudioCommon
{
namespace
{
// 200 ms, in units of 100 nanoseconds.
constexpr REFERENCE_TIME BUFFER_DURATION = 2'000'000;

winrt::handle CreateManualResetEvent()
{
  return winrt::handle(
      CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS));
}

struct ActivationHandler
    : winrt::implements<ActivationHandler, IActivateAudioInterfaceCompletionHandler>
{
  winrt::handle completed = CreateManualResetEvent();

  HRESULT STDMETHODCALLTYPE
  ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) noexcept override
  {
    SetEvent(completed.get());
    return S_OK;
  }
};

struct Activation
{
  winrt::com_ptr<ActivationHandler> handler = winrt::make_self<ActivationHandler>();
  winrt::com_ptr<IActivateAudioInterfaceAsyncOperation> operation;
  HRESULT result = E_PENDING;
  winrt::handle dispatched = CreateManualResetEvent();
};

// Opening the microphone can ask the user for permission, which only the UI thread can do.
std::shared_ptr<Activation> BeginActivation(const winrt::hstring& device)
{
  auto activation = std::make_shared<Activation>();
  const auto activate = [activation, device] {
    activation->result =
        ActivateAudioInterfaceAsync(device.c_str(), __uuidof(IAudioClient), nullptr,
                                    activation->handler.get(), activation->operation.put());
    SetEvent(activation->dispatched.get());
  };

  const auto dispatcher =
      winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
  if (dispatcher.HasThreadAccess())
    activate();
  else
    dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, activate);
  return activation;
}

// Returns false when the stop event is signaled first.
bool WaitFor(HANDLE stop, HANDLE event)
{
  const HANDLE events[] = {stop, event};
  return WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}
}  // namespace

UWPAudioCapture::~UWPAudioCapture()
{
  Stop();
}

void UWPAudioCapture::Start(u32 sample_rate, SampleCallback on_samples, ErrorCallback on_error)
{
  Stop();
  m_on_samples = std::move(on_samples);
  m_on_error = std::move(on_error);
  m_stop_event = CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
  m_thread = std::thread(&UWPAudioCapture::Run, this, sample_rate);
}

void UWPAudioCapture::Stop()
{
  if (m_thread.joinable())
  {
    SetEvent(m_stop_event);
    m_thread.join();
  }
  if (m_stop_event)
  {
    CloseHandle(m_stop_event);
    m_stop_event = nullptr;
  }
}

void UWPAudioCapture::Run(u32 sample_rate)
{
  Common::SetCurrentThreadName("Microphone");
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  Common::ScopeGuard apartment_guard([] { winrt::uninit_apartment(); });

  const HANDLE stop = m_stop_event;
  const auto fail = [this](std::string message) {
    ERROR_LOG_FMT(AUDIO, "{}", message);
    if (m_on_error)
      m_on_error(std::move(message));
  };

  try
  {
    using winrt::Windows::Media::Devices::AudioDeviceRole;
    using winrt::Windows::Media::Devices::MediaDevice;
    const winrt::hstring device = MediaDevice::GetDefaultAudioCaptureId(AudioDeviceRole::Default);
    if (device.empty())
    {
      fail("No microphone was found");
      return;
    }

    const std::shared_ptr<Activation> activation = BeginActivation(device);
    if (!WaitFor(stop, activation->dispatched.get()))
      return;
    winrt::check_hresult(activation->result);
    if (!WaitFor(stop, activation->handler->completed.get()))
      return;

    HRESULT result = E_FAIL;
    winrt::com_ptr<IUnknown> unknown;
    winrt::check_hresult(activation->operation->GetActivateResult(&result, unknown.put()));
    if (result == E_ACCESSDENIED)
    {
      fail("Dolphin does not have permission to use the microphone");
      return;
    }
    winrt::check_hresult(result);
    const auto client = unknown.as<IAudioClient>();

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = sample_rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    winrt::check_hresult(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                            BUFFER_DURATION, 0, &format, nullptr));

    const winrt::handle ready(CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS));
    winrt::check_hresult(client->SetEventHandle(ready.get()));
    winrt::com_ptr<IAudioCaptureClient> capture;
    winrt::check_hresult(client->GetService(IID_PPV_ARGS(capture.put())));
    winrt::check_hresult(client->Start());

    std::vector<s16> silence;
    while (WaitFor(stop, ready.get()))
    {
      UINT32 frames = 0;
      winrt::check_hresult(capture->GetNextPacketSize(&frames));
      while (frames != 0)
      {
        BYTE* data = nullptr;
        DWORD flags = 0;
        winrt::check_hresult(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr));
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
          silence.assign(frames, 0);
          m_on_samples(silence.data(), frames);
        }
        else
        {
          m_on_samples(reinterpret_cast<const s16*>(data), frames);
        }
        winrt::check_hresult(capture->ReleaseBuffer(frames));
        winrt::check_hresult(capture->GetNextPacketSize(&frames));
      }
    }
    client->Stop();
  }
  catch (const winrt::hresult_error& error)
  {
    fail(fmt::format("The microphone stopped working: {}", winrt::to_string(error.message())));
  }
}
}  // namespace AudioCommon

#pragma warning(pop)

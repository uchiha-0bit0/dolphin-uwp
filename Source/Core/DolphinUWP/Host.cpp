// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Host.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.UI.Core.h>

#include "Common/CommonTypes.h"
#include "DolphinUWP/GBAScreens.h"
#include "DolphinUWP/HostState.h"

namespace DolphinUWP::HostState
{
namespace
{
std::atomic<bool> s_renderer_focus = true;
std::atomic<bool> s_fullscreen = true;
std::atomic<bool> s_controller_input_blocked = false;
std::atomic<bool> s_stop_requested = false;
std::atomic<bool> s_debugger_update = false;
std::atomic<bool> s_title_changed = false;
std::atomic<u64> s_window_size_request = 0;
}  // namespace

void SetRendererFocus(bool focused)
{
  s_renderer_focus = focused;
}

bool HasRendererFocus()
{
  return s_renderer_focus;
}

void SetFullscreen(bool fullscreen)
{
  s_fullscreen = fullscreen;
}

bool IsFullscreen()
{
  return s_fullscreen;
}

void SetControllerInputBlocked(bool blocked)
{
  s_controller_input_blocked = blocked;
}

bool IsControllerInputBlocked()
{
  return s_controller_input_blocked;
}

void RequestStop()
{
  s_stop_requested = true;
}

bool ConsumeStopRequest()
{
  return s_stop_requested.exchange(false);
}

void NotifyDebuggerUpdate()
{
  s_debugger_update = true;
}

bool ConsumeDebuggerUpdate()
{
  return s_debugger_update.exchange(false);
}

void NotifyTitleChanged()
{
  s_title_changed = true;
}

bool ConsumeTitleChanged()
{
  return s_title_changed.exchange(false);
}

void RequestWindowSize(int width, int height)
{
  if (width > 0 && height > 0)
    s_window_size_request = (u64{static_cast<u32>(width)} << 32) | static_cast<u32>(height);
}

std::optional<std::pair<int, int>> ConsumeWindowSizeRequest()
{
  const u64 request = s_window_size_request.exchange(0);
  if (request == 0)
    return std::nullopt;
  return std::pair{static_cast<int>(request >> 32), static_cast<int>(request & 0xFFFFFFFF)};
}
}  // namespace DolphinUWP::HostState

std::vector<std::string> Host_GetPreferredLocales()
{
  std::vector<std::string> locales;
  try
  {
    for (const winrt::hstring& language :
         winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages())
    {
      locales.push_back(winrt::to_string(language));
    }
  }
  catch (const winrt::hresult_error&)
  {
  }
  return locales;
}

bool Host_UIBlocksControllerState()
{
  return DolphinUWP::HostState::IsControllerInputBlocked();
}

bool Host_RendererHasFocus()
{
  return DolphinUWP::HostState::HasRendererFocus();
}

bool Host_RendererHasFullFocus()
{
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return DolphinUWP::HostState::IsFullscreen();
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_Message(HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    DolphinUWP::HostState::RequestStop();
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
  DolphinUWP::HostState::RequestWindowSize(width, height);
}

void Host_UpdateDisasmDialog()
{
  DolphinUWP::HostState::NotifyDebuggerUpdate();
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_UpdateTitle(const std::string&)
{
}

void Host_YieldToUI()
{
  try
  {
    const auto window = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread();
    if (window)
    {
      window.Dispatcher().ProcessEvents(
          winrt::Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
    }
  }
  catch (const winrt::hresult_error&)
  {
  }
}

void Host_TitleChanged()
{
  DolphinUWP::HostState::NotifyTitleChanged();
}

void Host_UpdateDiscordClientID(const std::string&)
{
}

bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   const int64_t, const int64_t, const int, const int)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return DolphinUWP::GBAScreens::CreateHost(std::move(core));
}

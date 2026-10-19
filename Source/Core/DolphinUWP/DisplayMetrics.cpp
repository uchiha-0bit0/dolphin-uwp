// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/DisplayMetrics.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <windows.h>

#include <gamingdeviceinformation.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Graphics.Display.h>

#include "Common/UWP/Device.h"

namespace DolphinUWP
{
namespace
{
namespace Display = winrt::Windows::Graphics::Display;

GAMING_DEVICE_MODEL_INFORMATION GetModelInformation()
{
  GAMING_DEVICE_MODEL_INFORMATION info{};
  if (FAILED(GetGamingDeviceModelInformation(&info)))
    info = {};
  return info;
}

std::optional<OutputSize> GetMaximumOutputSize()
{
  const GAMING_DEVICE_MODEL_INFORMATION info = GetModelInformation();
  if (info.vendorId != GAMING_DEVICE_VENDOR_ID_MICROSOFT)
    return std::nullopt;

  switch (info.deviceId)
  {
  case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
  case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
    return OutputSize{1920, 1080};
  case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
    // A 4K swap chain fails to be created on Series S even when the TV accepts 4K.
    return OutputSize{2560, 1440};
  default:
    return OutputSize{3840, 2160};
  }
}

std::optional<OutputSize> GetHdmiOutputSize()
{
  try
  {
    if (!winrt::try_get_activation_factory<Display::Core::HdmiDisplayInformation>())
      return std::nullopt;

    const auto hdmi = Display::Core::HdmiDisplayInformation::GetForCurrentView();
    if (!hdmi)
      return std::nullopt;

    const auto mode = hdmi.GetCurrentDisplayMode();
    if (!mode || mode.ResolutionWidthInRawPixels() == 0 || mode.ResolutionHeightInRawPixels() == 0)
      return std::nullopt;

    const double refresh_rate = mode.RefreshRate();
    return OutputSize{mode.ResolutionWidthInRawPixels(), mode.ResolutionHeightInRawPixels(),
                      refresh_rate > 0.0 ? static_cast<float>(refresh_rate) : 60.0f};
  }
  catch (const winrt::hresult_error&)
  {
    return std::nullopt;
  }
}
}  // namespace

float GetPixelScale()
{
  try
  {
    const auto display = Display::DisplayInformation::GetForCurrentView();
    double scale = display.RawPixelsPerViewPixel();
    if (!std::isfinite(scale) || scale <= 0.0)
      scale = display.LogicalDpi() / 96.0;
    if (std::isfinite(scale) && scale > 0.0)
      return static_cast<float>(scale);
  }
  catch (const winrt::hresult_error&)
  {
  }
  return 1.0f;
}

OutputSize GetWindowPixelSize(const winrt::Windows::UI::Core::CoreWindow& window)
{
  const auto bounds = window.Bounds();
  const float scale = GetPixelScale();
  return {std::max(1u, static_cast<u32>(std::lround(bounds.Width * scale))),
          std::max(1u, static_cast<u32>(std::lround(bounds.Height * scale)))};
}

OutputSize GetOutputSize(const winrt::Windows::UI::Core::CoreWindow& window)
{
  OutputSize size = GetHdmiOutputSize().value_or(GetWindowPixelSize(window));
  if (const auto maximum = GetMaximumOutputSize())
  {
    size.width = std::min(size.width, maximum->width);
    size.height = std::min(size.height, maximum->height);
  }
  return size;
}

bool IsXbox()
{
  return Common::UWP::IsXboxConsole();
}
}  // namespace DolphinUWP

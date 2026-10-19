// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <winrt/Windows.UI.Core.h>

#include "Common/CommonTypes.h"

namespace DolphinUWP
{
struct OutputSize
{
  u32 width = 1280;
  u32 height = 720;
  float refresh_rate = 60.0f;
};

// All functions must be called from the CoreWindow thread.
float GetPixelScale();
OutputSize GetWindowPixelSize(const winrt::Windows::UI::Core::CoreWindow& window);
OutputSize GetOutputSize(const winrt::Windows::UI::Core::CoreWindow& window);

bool IsXbox();
}  // namespace DolphinUWP

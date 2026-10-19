// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "VideoCommon/VideoConfig.h"

// Choices shared by the global and per-game settings pages.
namespace DolphinUWP
{
template <typename T>
struct Choice
{
  T value;
  const char* label;
};

inline constexpr std::array<Choice<int>, 9> RESOLUTIONS{{
    {0, "Auto (Window Size)"},
    {1, "Native (640x528)"},
    {2, "2x Native (1280x1056) for 720p"},
    {3, "3x Native (1920x1584) for 1080p"},
    {4, "4x Native (2560x2112) for 1440p"},
    {5, "5x Native (3200x2640)"},
    {6, "6x Native (3840x3168) for 4K"},
    {7, "7x Native (4480x3696)"},
    {8, "8x Native (5120x4224) for 5K"},
}};

inline constexpr std::array<Choice<AspectMode>, 6> ASPECT_MODES{{
    {AspectMode::Auto, "Auto"},
    {AspectMode::ForceWide, "Force 16:9"},
    {AspectMode::ForceStandard, "Force 4:3"},
    {AspectMode::Stretch, "Stretch to Window"},
    {AspectMode::Custom, "Custom"},
    {AspectMode::CustomStretch, "Custom (Stretch)"},
}};

inline constexpr std::array<Choice<ShaderCompilationMode>, 4> SHADER_MODES{{
    {ShaderCompilationMode::Synchronous, "Specialized (Default)"},
    {ShaderCompilationMode::SynchronousUberShaders, "Exclusive Ubershaders"},
    {ShaderCompilationMode::AsynchronousUberShaders, "Hybrid Ubershaders"},
    {ShaderCompilationMode::AsynchronousSkipRendering, "Skip Drawing"},
}};

inline constexpr std::array<Choice<int>, 3> TEXTURE_CACHE_MODES{{
    {0, "Safe"},
    {512, "Balanced"},
    {128, "Fast"},
}};
}  // namespace DolphinUWP

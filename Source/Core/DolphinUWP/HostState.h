// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <utility>

namespace DolphinUWP::HostState
{
void SetRendererFocus(bool focused);
bool HasRendererFocus();

void SetFullscreen(bool fullscreen);
bool IsFullscreen();

void SetControllerInputBlocked(bool blocked);
bool IsControllerInputBlocked();

void RequestStop();
bool ConsumeStopRequest();

// Set when the emulated CPU stops or steps, for example at a breakpoint.
void NotifyDebuggerUpdate();
bool ConsumeDebuggerUpdate();

// Set when the running title changes, for example when the Wii Menu starts a game.
void NotifyTitleChanged();
bool ConsumeTitleChanged();

// The size in pixels that the game's output would fill without scaling.
void RequestWindowSize(int width, int height);
std::optional<std::pair<int, int>> ConsumeWindowSizeRequest();
}  // namespace DolphinUWP::HostState

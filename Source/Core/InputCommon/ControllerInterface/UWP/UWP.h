// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/CommonTypes.h"
#include "InputCommon/ControllerInterface/InputBackend.h"

namespace ciface::UWP
{
std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);

void SetKeyState(u32 virtual_key, bool pressed);
bool IsKeyPressed(u32 virtual_key);
void ResetKeyStates();

// Mouse state reported by the window. The cursor position ranges from -1 to 1 across the window.
void SetCursorPosition(float x, float y);
void SetMouseButtons(u32 buttons);
void AddMouseMovement(float x, float y, float wheel);
void ResetMouseState();

// True once after a hotkey asks for the cursor to move to the center of the window.
bool ConsumeCursorCenterRequest();
}  // namespace ciface::UWP

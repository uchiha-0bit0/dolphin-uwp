// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

#include <winrt/Windows.System.h>

// Text editing for the in-game overlays through the system's text services, which also serve the
// on-screen keyboard. Every function runs on the UI thread.
namespace DolphinUWP::GameTextInput
{
enum class Result
{
  None,
  Submitted,
  Cancelled,
};

// Starts editing. A single line is submitted with Enter; several lines are finished with Escape.
void Begin(std::string text, bool multiline, bool show_keyboard);
void End();
bool IsActive();
bool IsMultiline();

void ShowKeyboard();
// True while the on-screen keyboard covers the screen and takes the controller.
bool IsKeyboardVisible();

std::string GetText();
// The position of the caret in the UTF-8 text.
size_t GetCaret();

// Handles the keys that the text services leave to the app, such as Backspace and the arrows.
// Returns true when the key belongs to the text.
bool HandleKey(winrt::Windows::System::VirtualKey key);
void MoveCaret(int delta);
void Submit();
void Cancel();
Result TakeResult();
}  // namespace DolphinUWP::GameTextInput

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Resolves exports of System32 libraries whose declarations the app partition headers omit.
namespace Common::UWP
{
void* GetSystemProcAddress(const wchar_t* library, const char* name);

template <typename Function>
Function GetSystemFunction(const wchar_t* library, const char* name)
{
  return reinterpret_cast<Function>(GetSystemProcAddress(library, name));
}
}  // namespace Common::UWP

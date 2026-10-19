// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/UWP/SystemLibrary.h"

#include <map>
#include <mutex>
#include <string>

#include <windows.h>

namespace Common::UWP
{
void* GetSystemProcAddress(const wchar_t* library, const char* name)
{
  static std::mutex mutex;
  static std::map<std::wstring, HMODULE> modules;

  HMODULE module;
  {
    std::lock_guard lock(mutex);
    const auto [iter, inserted] = modules.try_emplace(library, nullptr);
    if (inserted)
      iter->second = LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    module = iter->second;
  }

  return module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
}
}  // namespace Common::UWP

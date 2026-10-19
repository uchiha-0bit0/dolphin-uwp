// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Common::UWP
{
bool IsXboxConsole();
std::string_view GetDeviceName();
std::string GetPackageVersion();
}  // namespace Common::UWP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace DolphinUWP
{
// Writes a minidump and a short report to LocalState\CrashDumps when the app crashes.
void InstallCrashHandler();
}  // namespace DolphinUWP

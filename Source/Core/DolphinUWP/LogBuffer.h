// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

// Keeps the most recent log messages so the log page can show them.
namespace DolphinUWP::LogBuffer
{
struct Entry
{
  Common::Log::LogLevel level;
  std::string text;
};

// Registers the log window listener. Call once after the log manager is initialized.
void Install();

std::vector<Entry> GetEntries();
// Increases whenever a message is added or the buffer is cleared.
u64 GetVersion();
void Clear();
}  // namespace DolphinUWP::LogBuffer

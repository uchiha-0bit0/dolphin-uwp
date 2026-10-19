// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Decodes the graphics commands of the FIFO log that is playing.
namespace DolphinUWP::FifoAnalysis
{
struct Object
{
  std::string label;
  u32 frame = 0;
  u32 first_part = 0;
  u32 last_part = 0;
};

struct Command
{
  u32 offset = 0;
  std::string text;
};

bool IsAvailable();
u32 GetFrameCount();
u32 GetObjectCount(u32 frame);
std::vector<Object> GetObjects(u32 frame);
std::vector<Command> GetCommands(const Object& object);
std::string Describe(const Object& object, u32 offset);
}  // namespace DolphinUWP::FifoAnalysis

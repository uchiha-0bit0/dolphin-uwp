// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

namespace Core
{
class System;
}

namespace DolphinUWP::DebuggerNetwork
{
struct Socket
{
  std::string label;
  std::string value;
};

// Call with the CPU paused. Lists the sockets of a Wii or Triforce game that are open.
std::vector<Socket> List(Core::System& system);
}  // namespace DolphinUWP::DebuggerNetwork

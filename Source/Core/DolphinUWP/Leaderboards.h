// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace DolphinUWP::Leaderboards
{
struct Board
{
  u32 id = 0;
  std::string group;
  std::string title;
  std::string description;
  // The current value of a leaderboard that is being attempted.
  std::string tracker;
};

struct Entry
{
  u32 rank = 0;
  std::string user;
  std::string score;
  bool player = false;
};

struct Entries
{
  bool loading = false;
  bool failed = false;
  // The top entries, followed by the entries around the player.
  std::vector<Entry> entries;
};

bool Available();
std::vector<Board> List();
// Starts fetching the top entries of a leaderboard and the entries around the player.
void Fetch(u32 id);
Entries Get(u32 id);
// True once since entries arrived.
bool ConsumeChanged();
}  // namespace DolphinUWP::Leaderboards

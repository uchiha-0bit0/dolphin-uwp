// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/ActionReplay.h"
#include "Core/GeckoCode.h"
#include "Core/PatchEngine.h"

namespace DolphinUWP
{
// The Action Replay codes, Gecko codes and patches of one game, with the user's game INI.
class GameCheats
{
public:
  void Load(std::string game_id, u16 revision);
  void Save() const;
  void ApplyToRunningGame() const;

  const std::string& GetGameId() const { return m_game_id; }
  bool IsLoaded() const { return !m_game_id.empty(); }

  std::vector<Gecko::GeckoCode> gecko;
  std::vector<ActionReplay::ARCode> action_replay;
  std::vector<PatchEngine::Patch> patches;

private:
  std::string m_game_id;
  u16 m_revision = 0;
};

// Code text is accepted with any whitespace between values, so it can be typed on one line.
std::optional<std::vector<ActionReplay::AREntry>> ParseActionReplayCode(std::string_view text,
                                                                        std::string* error);
std::optional<std::vector<Gecko::GeckoCode::Code>> ParseGeckoCode(std::string_view text,
                                                                  std::string* error);
std::optional<std::vector<PatchEngine::PatchEntry>> ParsePatch(std::string_view text,
                                                               std::string* error);

std::string FormatActionReplayCode(const ActionReplay::ARCode& code);
std::string FormatGeckoCode(const Gecko::GeckoCode& code);
std::string FormatPatch(const PatchEngine::Patch& patch);
}  // namespace DolphinUWP

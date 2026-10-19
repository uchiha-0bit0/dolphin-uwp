// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

class GraphicsModGroupConfig;

namespace DolphinUWP
{
// The graphics mods available to one game. Changes are saved, and applied when the game runs.
class GraphicsModList
{
public:
  struct Mod
  {
    std::string path;
    std::string title;
    std::string author;
    std::string description;
    bool enabled = false;
  };

  GraphicsModList();
  ~GraphicsModList();

  void Load(std::string game_id);
  void Clear();
  bool IsLoaded() const { return m_group != nullptr; }
  const std::string& GetGameId() const { return m_game_id; }

  std::vector<Mod> GetMods() const;
  void SetEnabled(const std::string& path, bool enabled);

private:
  std::unique_ptr<GraphicsModGroupConfig> m_group;
  std::string m_game_id;
};
}  // namespace DolphinUWP

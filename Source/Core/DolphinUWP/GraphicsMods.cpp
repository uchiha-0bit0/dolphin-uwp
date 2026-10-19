// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GraphicsMods.h"

#include <algorithm>
#include <atomic>
#include <set>

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsMod.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/VideoConfig.h"

namespace DolphinUWP
{
namespace
{
// The video thread reloads mods when the change count differs from the one it has, so every
// change gets a value that was never used before.
u32 NextChangeCount()
{
  static std::atomic<u32> s_change_count = 0x10000;
  return ++s_change_count;
}
}  // namespace

GraphicsModList::GraphicsModList() = default;
GraphicsModList::~GraphicsModList() = default;

void GraphicsModList::Load(std::string game_id)
{
  m_game_id = std::move(game_id);
  m_group = std::make_unique<GraphicsModGroupConfig>(m_game_id);
  m_group->Load();
}

void GraphicsModList::Clear()
{
  m_group.reset();
  m_game_id.clear();
}

std::vector<GraphicsModList::Mod> GraphicsModList::GetMods() const
{
  std::vector<Mod> mods;
  if (!m_group)
    return mods;

  std::set<std::string, std::less<>> groups;
  for (const GraphicsModConfig& mod : m_group->GetMods())
  {
    for (const GraphicsTargetGroupConfig& group : mod.m_groups)
      groups.insert(group.m_name);
  }

  for (const GraphicsModConfig& mod : m_group->GetMods())
  {
    if (std::ranges::none_of(mod.m_features, [&groups](const GraphicsModFeatureConfig& feature) {
          return groups.contains(feature.m_group);
        }))
    {
      continue;
    }
    mods.push_back(
        {mod.GetAbsolutePath(), mod.m_title, mod.m_author, mod.m_description, mod.m_enabled});
  }
  return mods;
}

void GraphicsModList::SetEnabled(const std::string& path, bool enabled)
{
  if (!m_group)
    return;
  GraphicsModConfig* const mod = m_group->GetMod(path);
  if (!mod || mod->m_enabled == enabled)
    return;

  mod->m_enabled = enabled;
  m_group->SetChangeCount(NextChangeCount());
  m_group->Save();

  auto& system = Core::System::GetInstance();
  if (!Core::IsRunning(system) || SConfig::GetInstance().GetGameID() != m_game_id)
    return;
  system.GetFifo().PauseAndLock();
  g_Config.graphics_mod_config = *m_group;
  system.GetFifo().RestoreState(true);
}
}  // namespace DolphinUWP

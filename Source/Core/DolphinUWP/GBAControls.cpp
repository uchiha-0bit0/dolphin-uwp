// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GBAControls.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "Core/Config/MainSettings.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI_Device.h"
#include "DolphinUWP/GBAScreens.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/InputConfig.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace DolphinUWP::GBAControls
{
namespace
{
constexpr size_t PLAYERS = 4;
using Targets = std::array<std::optional<size_t>, PLAYERS>;

std::mutex s_mutex;
Targets s_targets;

bool IsGBAPort(size_t port)
{
  return Config::Get(Config::GetInfoForSIDevice(static_cast<int>(port))) ==
         SerialInterface::SIDEVICE_GC_GBA_EMULATED;
}

// The state of a control in a player's Game Boy Advance mapping.
ControlState GetGBAControl(size_t player, std::string_view group_name,
                           std::string_view control_name)
{
  for (const GBAPadGroup group_id : {GBAPadGroup::Buttons, GBAPadGroup::DPad})
  {
    const ControllerEmu::ControlGroup* const group =
        Pad::GetGBAGroup(static_cast<int>(player), group_id);
    if (!group || group->name != group_name)
      continue;
    for (const auto& control : group->controls)
    {
      if (control->name == control_name)
        return control->GetState<ControlState>();
    }
  }
  return 0;
}

void ApplyTargets(const Targets& targets)
{
  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  for (size_t player = 0; player < PLAYERS; ++player)
  {
    ControllerEmu::EmulatedController* const controller =
        Pad::GetConfig()->GetController(static_cast<int>(player));
    if (targets[player])
    {
      controller->SetInputOverrideFunction(
          [](std::string_view, std::string_view, ControlState) -> std::optional<ControlState> {
            return 0;
          });
    }
    else
    {
      controller->ClearInputOverrideFunction();
    }
  }

  for (size_t gba = 0; gba < GBAScreens::MAX_SCREENS; ++gba)
  {
    std::vector<size_t> players;
    for (size_t player = 0; player < PLAYERS; ++player)
    {
      if (targets[player] == gba)
        players.push_back(player);
    }
    ControllerEmu::EmulatedController* const controller =
        Pad::GetGBAConfig()->GetController(static_cast<int>(gba));
    if (players.empty())
    {
      controller->ClearInputOverrideFunction();
      continue;
    }
    controller->SetInputOverrideFunction(
        [players](std::string_view group, std::string_view control,
                  ControlState state) -> std::optional<ControlState> {
          for (const size_t player : players)
            state = std::max(state, GetGBAControl(player, group, control));
          return state;
        });
  }
}
}  // namespace

void SwitchPlayer(size_t player)
{
  if (player >= PLAYERS)
    return;
  if (IsGBAPort(player))
  {
    OSD::AddMessage(fmt::format("Player {} plays the GBA in port {}", player + 1, player + 1));
    return;
  }

  Targets targets;
  bool any_running = false;
  {
    std::lock_guard lock(s_mutex);
    const size_t first = s_targets[player] ? *s_targets[player] + 1 : 0;
    std::optional<size_t> next;
    for (size_t gba = 0; gba < GBAScreens::MAX_SCREENS; ++gba)
    {
      if (!GBAScreens::IsRunning(gba))
        continue;
      any_running = true;
      if (!next && gba >= first)
        next = gba;
    }
    s_targets[player] = next;
    targets = s_targets;
  }
  ApplyTargets(targets);

  if (targets[player])
    OSD::AddMessage(fmt::format("Player {} controls GBA {}", player + 1, *targets[player] + 1));
  else if (any_running)
    OSD::AddMessage(fmt::format("Player {} controls the game", player + 1));
  else
    OSD::AddMessage("There is no Game Boy Advance to control");
}

std::optional<size_t> GetControlledGBA(size_t player)
{
  if (player >= PLAYERS)
    return std::nullopt;
  std::lock_guard lock(s_mutex);
  return s_targets[player];
}

u32 GetControllingPlayers(size_t gba)
{
  u32 players = 0;
  std::lock_guard lock(s_mutex);
  for (size_t player = 0; player < PLAYERS; ++player)
  {
    if (s_targets[player] == gba)
      players |= 1u << player;
  }
  return players;
}

void Reapply()
{
  Targets targets;
  {
    std::lock_guard lock(s_mutex);
    targets = s_targets;
  }
  ApplyTargets(targets);
}

void Reset()
{
  {
    std::lock_guard lock(s_mutex);
    if (std::ranges::none_of(s_targets, [](const auto& target) { return target.has_value(); }))
      return;
    s_targets = {};
  }
  ApplyTargets({});
}
}  // namespace DolphinUWP::GBAControls

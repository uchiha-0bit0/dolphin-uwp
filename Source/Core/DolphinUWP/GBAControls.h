// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>

#include "Common/CommonTypes.h"

// Lets one controller play both the game and a GBA. A hotkey moves a player's controls between
// the screens, and while a GBA has them, the player's GameCube controller is idle.
namespace DolphinUWP::GBAControls
{
// Moves the player's controls to the next running GBA, and from the last one back to the game.
void SwitchPlayer(size_t player);
std::optional<size_t> GetControlledGBA(size_t player);
// One bit per player whose controls the GBA has.
u32 GetControllingPlayers(size_t gba);
// Gives every player's controls back to the game.
void Reset();
// Sets the controls again after something else, such as TAS input, replaced them.
void Reapply();
}  // namespace DolphinUWP::GBAControls

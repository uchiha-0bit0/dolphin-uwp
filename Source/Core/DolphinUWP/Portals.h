// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Functions return an error message, or an empty string on success.
namespace DolphinUWP::Portals
{
constexpr u8 SKYLANDER_SLOTS = 16;
constexpr u8 INFINITY_SLOTS = 9;

struct FigureFile
{
  std::string name;
  std::string path;
};

struct FigureType
{
  std::string name;
  u32 id = 0;
};

std::string GetSkylanderCollection();
std::vector<FigureFile> ListSkylanderFiles();
std::vector<FigureType> ListSkylanderTypes();
std::string GetSkylanderName(u8 slot);
std::string LoadSkylander(u8 slot, const std::string& path);
std::string CreateSkylander(u8 slot, u32 type);
std::string ClearSkylander(u8 slot);

std::string GetInfinityCollection();
std::vector<FigureFile> ListInfinityFiles();
std::vector<FigureType> ListInfinityTypes(u8 slot);
const char* GetInfinitySlotLabel(u8 slot);
std::string GetInfinityName(u8 slot);
std::string LoadInfinity(u8 slot, const std::string& path);
std::string CreateInfinity(u8 slot, u32 number);
void ClearInfinity(u8 slot);
}  // namespace DolphinUWP::Portals

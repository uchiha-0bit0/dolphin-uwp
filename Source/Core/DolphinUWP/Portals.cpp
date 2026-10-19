// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/Portals.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/IOS/USB/Emulated/Infinity.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"
#include "Core/System.h"

namespace DolphinUWP::Portals
{
namespace
{
using IOS::HLE::USB::FigureUIPosition;

constexpr size_t SKYLANDER_FILE_SIZE = 0x40 * 0x10;
constexpr size_t INFINITY_FILE_SIZE =
    IOS::HLE::USB::INFINITY_NUM_BLOCKS * IOS::HLE::USB::INFINITY_BLOCK_SIZE;

struct SkylanderSlot
{
  u8 portal_slot = 0;
  std::string name;
};

std::array<std::optional<SkylanderSlot>, SKYLANDER_SLOTS> s_skylanders;
std::array<std::string, INFINITY_SLOTS> s_infinity;

std::string SkylanderTypeName(u16 id, u16 var)
{
  const auto found = IOS::HLE::USB::list_skylanders.find({id, var});
  if (found != IOS::HLE::USB::list_skylanders.end())
    return found->second.name;
  return fmt::format("Unknown ({} {})", id, var);
}

std::string UniquePath(const std::string& directory, std::string name, std::string_view extension)
{
  std::ranges::replace_if(
      name, [](char c) { return std::string_view("\\/:*?\"<>|").find(c) != std::string::npos; },
      '_');
  std::string path = fmt::format("{}{}{}", directory, name, extension);
  for (int index = 2; File::Exists(path); ++index)
    path = fmt::format("{}{} ({}){}", directory, name, index, extension);
  return path;
}

std::vector<FigureFile> ListFiles(const std::string& directory,
                                  std::span<const std::string_view> extensions)
{
  std::vector<FigureFile> files;
  for (const std::string& path : Common::DoFileSearch(directory, extensions))
  {
    FigureFile file;
    file.path = path;
    SplitPath(path, nullptr, &file.name, nullptr);
    files.push_back(std::move(file));
  }
  std::ranges::sort(files, {}, &FigureFile::name);
  return files;
}

bool IsInfinityFigureValid(u8 slot, u32 figure)
{
  switch (static_cast<FigureUIPosition>(slot))
  {
  case FigureUIPosition::HexagonDiscOne:
    return (figure > 0x1E8480 && figure < 0x2DC6BF) || (figure > 0x3D0900 && figure < 0x4C4B3F);
  case FigureUIPosition::HexagonDiscTwo:
  case FigureUIPosition::HexagonDiscThree:
    return figure > 0x3D0900 && figure < 0x4C4B3F;
  case FigureUIPosition::PlayerOne:
  case FigureUIPosition::PlayerTwo:
    return figure < 0x1E847F;
  default:
    return figure > 0x2DC6C0 && figure < 0x3D08FF;
  }
}
}  // namespace

std::string GetSkylanderCollection()
{
  std::string path = Config::Get(Config::MAIN_SKYLANDERS_PATH);
  if (path.empty())
    path = File::GetUserPath(D_SKYLANDERS_IDX);
  if (!path.ends_with('/') && !path.ends_with('\\'))
    path += DIR_SEP;
  return path;
}

std::vector<FigureFile> ListSkylanderFiles()
{
  constexpr std::array<std::string_view, 4> extensions{".sky", ".bin", ".dmp", ".dump"};
  std::vector<FigureFile> files = ListFiles(GetSkylanderCollection(), extensions);
  auto& portal = Core::System::GetInstance().GetSkylanderPortal();
  for (FigureFile& file : files)
  {
    File::IOFile figure(file.path, "rb");
    std::array<u8, SKYLANDER_FILE_SIZE> data;
    if (!figure || !figure.ReadBytes(data.data(), data.size()))
      continue;
    const auto [id, var] = portal.CalculateIDs(data);
    file.name = fmt::format("{} - {}", file.name, SkylanderTypeName(id, var));
  }
  return files;
}

std::vector<FigureType> ListSkylanderTypes()
{
  std::vector<FigureType> types;
  for (const auto& [ids, data] : IOS::HLE::USB::list_skylanders)
    types.push_back({data.name, (u32(ids.first) << 16) | ids.second});
  std::ranges::stable_sort(types, {}, &FigureType::name);
  return types;
}

std::string GetSkylanderName(u8 slot)
{
  if (slot >= SKYLANDER_SLOTS || !s_skylanders[slot])
    return {};
  return s_skylanders[slot]->name;
}

std::string ClearSkylander(u8 slot)
{
  if (slot >= SKYLANDER_SLOTS || !s_skylanders[slot])
    return {};
  auto& portal = Core::System::GetInstance().GetSkylanderPortal();
  if (!portal.RemoveSkylander(s_skylanders[slot]->portal_slot))
    return "The figure could not be removed from the portal.";
  s_skylanders[slot].reset();
  return {};
}

std::string LoadSkylander(u8 slot, const std::string& path)
{
  if (slot >= SKYLANDER_SLOTS)
    return "Invalid portal slot.";

  File::IOFile file(path, "r+b");
  if (!file)
    return "The figure file could not be opened. It may already be on the portal.";
  std::array<u8, SKYLANDER_FILE_SIZE> data;
  if (!file.ReadBytes(data.data(), data.size()))
    return "The figure file is too small.";

  if (std::string error = ClearSkylander(slot); !error.empty())
    return error;

  auto& portal = Core::System::GetInstance().GetSkylanderPortal();
  const auto [id, var] = portal.CalculateIDs(data);
  const u8 portal_slot =
      portal.LoadSkylander(std::make_unique<IOS::HLE::USB::SkylanderFigure>(std::move(file)));
  if (portal_slot == 0xFF)
    return "The portal is full.";
  s_skylanders[slot] = SkylanderSlot{portal_slot, SkylanderTypeName(id, var)};
  return {};
}

std::string CreateSkylander(u8 slot, u32 type)
{
  const u16 id = static_cast<u16>(type >> 16);
  const u16 var = static_cast<u16>(type);
  const std::string directory = GetSkylanderCollection();
  File::CreateFullPath(directory);
  const std::string path = UniquePath(directory, SkylanderTypeName(id, var), ".sky");
  {
    IOS::HLE::USB::SkylanderFigure figure(path);
    if (!figure.Create(id, var))
      return "The figure file could not be created.";
    figure.Close();
  }
  return LoadSkylander(slot, path);
}

std::string GetInfinityCollection()
{
  return File::GetUserPath(D_USER_IDX) + "Infinity" DIR_SEP;
}

std::vector<FigureFile> ListInfinityFiles()
{
  constexpr std::array<std::string_view, 2> extensions{".bin", ".dmp"};
  return ListFiles(GetInfinityCollection(), extensions);
}

std::vector<FigureType> ListInfinityTypes(u8 slot)
{
  std::vector<FigureType> types;
  for (const auto& [name, number] : IOS::HLE::USB::InfinityBase::GetFigureList())
  {
    if (IsInfinityFigureValid(slot, number))
      types.push_back({name, number});
  }
  std::ranges::stable_sort(types, {}, &FigureType::name);
  return types;
}

const char* GetInfinitySlotLabel(u8 slot)
{
  static constexpr std::array<const char*, INFINITY_SLOTS> labels{
      "Power Disc 1", "Power Disc 2",         "Power Disc 3",
      "Player One",   "Player One Ability 1", "Player One Ability 2",
      "Player Two",   "Player Two Ability 1", "Player Two Ability 2"};
  return slot < labels.size() ? labels[slot] : "";
}

std::string GetInfinityName(u8 slot)
{
  return slot < INFINITY_SLOTS ? s_infinity[slot] : std::string();
}

std::string LoadInfinity(u8 slot, const std::string& path)
{
  if (slot >= INFINITY_SLOTS)
    return "Invalid base position.";

  File::IOFile file(path, "r+b");
  if (!file)
    return "The figure file could not be opened. It may already be on the base.";
  std::array<u8, INFINITY_FILE_SIZE> data;
  if (!file.ReadBytes(data.data(), data.size()))
    return "The figure file is too small.";

  auto& base = Core::System::GetInstance().GetInfinityBase();
  const auto position = static_cast<FigureUIPosition>(slot);
  base.RemoveFigure(position);
  s_infinity[slot] = base.LoadFigure(data, std::move(file), position);
  return {};
}

std::string CreateInfinity(u8 slot, u32 number)
{
  auto& base = Core::System::GetInstance().GetInfinityBase();
  const std::string directory = GetInfinityCollection();
  File::CreateFullPath(directory);
  const std::string path = UniquePath(directory, base.FindFigure(number), ".bin");
  if (!base.CreateFigure(path, number))
    return "The figure file could not be created.";
  return LoadInfinity(slot, path);
}

void ClearInfinity(u8 slot)
{
  if (slot >= INFINITY_SLOTS)
    return;
  Core::System::GetInstance().GetInfinityBase().RemoveFigure(static_cast<FigureUIPosition>(slot));
  s_infinity[slot].clear();
}
}  // namespace DolphinUWP::Portals

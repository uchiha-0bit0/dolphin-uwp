// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <UwpImGuiFrontend/GraphicsView.h>
#include <UwpImGuiFrontend/Types.h>

#include "Common/CommonTypes.h"
#include "DiscIO/RiivolutionParser.h"
#include "DolphinUWP/GameCheats.h"
#include "DolphinUWP/GraphicsMods.h"

namespace UICommon
{
class GameFile;
}

namespace DolphinUWP
{
struct ShellServices;

class CheatsPage
{
public:
  explicit CheatsPage(const ShellServices& services) : m_services(services) {}

  void Open(const UICommon::GameFile& game);
  void Close();
  // Returns true when the page asks to close.
  bool Draw(float controller_scroll, bool context_pressed);

private:
  enum class Kind
  {
    Gecko,
    ActionReplay,
    Patch,
  };

  struct Editor
  {
    Kind kind = Kind::Gecko;
    std::optional<size_t> index;
    std::array<char, 128> name{};
    std::array<char, 128> creator{};
    std::array<char, 8192> code{};
    std::string error;
    bool focus = true;
  };

  void DrawCodes(Kind kind);
  void DrawEditor();
  void DrawActions();
  void OpenEditor(Kind kind, std::optional<size_t> index);
  bool SaveEditor();
  void Delete(Kind kind, size_t index);
  void DownloadGeckoCodes();
  bool IsUserDefined(Kind kind, size_t index) const;
  std::string GetName(Kind kind, size_t index) const;

  const ShellServices& m_services;
  GameCheats m_cheats;
  std::string m_gametdb_id;
  UwpImGuiFrontend::GraphicsView m_view;
  bool m_context_pressed = false;
  std::optional<Editor> m_editor;
  std::optional<std::pair<Kind, size_t>> m_actions;
};

class GraphicsModsPage
{
public:
  using StorageDrawer = std::function<void(std::string_view id)>;

  void SetStorageDrawer(StorageDrawer drawer) { m_draw_storage = std::move(drawer); }
  void Open(const UICommon::GameFile& game);
  void Close();
  bool Draw(float controller_scroll);

private:
  StorageDrawer m_draw_storage;
  GraphicsModList m_mods;
  UwpImGuiFrontend::GraphicsView m_view;
};

class RiivolutionPage
{
public:
  explicit RiivolutionPage(const ShellServices& services) : m_services(services) {}

  void Open(const UICommon::GameFile& game, UwpImGuiFrontend::ItemId item, std::string title);
  void Close();
  bool Draw(float controller_scroll);

private:
  struct PatchDisc
  {
    DiscIO::Riivolution::Disc disc;
    std::string root;
    std::string name;
  };

  void SaveConfig() const;
  void Boot();

  const ShellServices& m_services;
  std::vector<PatchDisc> m_discs;
  std::string m_path;
  std::string m_game_id;
  std::optional<u16> m_revision;
  std::optional<u8> m_disc_number;
  UwpImGuiFrontend::ItemId m_item = 0;
  std::string m_title;
  UwpImGuiFrontend::GraphicsView m_view;
};
}  // namespace DolphinUWP

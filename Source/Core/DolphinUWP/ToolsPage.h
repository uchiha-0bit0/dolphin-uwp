// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <UwpImGuiFrontend/GraphicsView.h>

#include "DolphinUWP/LogBuffer.h"
#include "DolphinUWP/MemoryCardPanel.h"
#include "DolphinUWP/PortalPanel.h"
#include "DolphinUWP/Recordings.h"
#include "DolphinUWP/WiiSystemTools.h"

struct BootParameters;

namespace DolphinUWP
{
struct ShellServices;

class ToolsPage
{
public:
  explicit ToolsPage(const ShellServices& services);

  void Open(std::string_view tab = {});
  void Close();
  // Returns true when the page asks to close.
  bool Draw(float controller_scroll);

  void PickFileToStart();
  void PickMovie();

private:
  struct ResourcePackRow
  {
    std::string path;
    std::string title;
    std::string detail;
    UwpImGuiFrontend::TextureHandle logo;
    bool valid = false;
    bool installed = false;
  };

  void Refresh();
  void DrawStart();
  void DrawLog();
  void DrawLogTypes();
  void DrawResourcePacks();
  void DrawResourcePackActions();
  void RefreshResourcePacks();
  void ReleaseResourcePackLogos();
  void ImportResourcePack(const std::string& path);
  void ChangeResourcePack(const ResourcePackRow& row, std::string_view action);
  void DrawMovies();
  void DrawFifo();
  void DrawSystem();
  void DrawRegionDialog();
  void PlayMovie(const std::string& path);
  void PlayFifo(const std::string& path);
  void Boot(std::unique_ptr<BootParameters> parameters, std::string title);

  UwpImGuiFrontend::GraphicsViewModel BuildModel();

  const ShellServices& m_services;
  UwpImGuiFrontend::GraphicsView m_view;
  std::vector<Recordings::RecordingFile> m_movies;
  std::vector<Recordings::RecordingFile> m_fifo_logs;
  WiiSystemTools::SystemMenuInfo m_system_menu;
  std::vector<std::pair<DiscIO::Region, std::string>> m_ipl_regions;
  MemoryCardPanel m_card_a{ExpansionInterface::Slot::A};
  MemoryCardPanel m_card_b{ExpansionInterface::Slot::B};
  PortalPanel m_skylanders{PortalPanel::Kind::Skylanders};
  PortalPanel m_infinity{PortalPanel::Kind::Infinity};
  std::vector<ResourcePackRow> m_resource_packs;
  std::vector<LogBuffer::Entry> m_log_entries;
  u64 m_log_version = 0;
  std::optional<size_t> m_resource_pack_action;
  bool m_region_dialog = false;
};
}  // namespace DolphinUWP

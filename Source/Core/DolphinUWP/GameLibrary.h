// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <UwpImGuiFrontend/Types.h>

#include "Common/CommonTypes.h"

namespace UICommon
{
class GameFile;
}

namespace DolphinUWP
{
class GameLibrary
{
public:
  struct GameEntry
  {
    UwpImGuiFrontend::ItemId id = 0;
    std::string name;
    std::shared_ptr<const UICommon::GameFile> game;
  };

  static constexpr u32 SCREENSCRAPER_GAMECUBE_SYSTEM_ID = 13;
  static constexpr u32 SCREENSCRAPER_WII_SYSTEM_ID = 16;

  GameLibrary();
  ~GameLibrary();

  void Initialize(std::filesystem::path metadata_directory);
  void Shutdown();

  void Refresh();
  void Rebuild();
  bool ConsumeChanged();
  bool IsScanning() const;
  void SetMediaDirectory(std::filesystem::path directory);

  std::vector<UwpImGuiFrontend::LibraryItem> GetItems() const;
  std::optional<UwpImGuiFrontend::LibraryItem> FindItem(UwpImGuiFrontend::ItemId id) const;
  std::optional<UwpImGuiFrontend::LibraryItem> FindItemByPath(const std::string& path) const;
  std::shared_ptr<const UICommon::GameFile> FindGame(UwpImGuiFrontend::ItemId id) const;
  std::vector<GameEntry> GetGameEntries() const;
  std::optional<GameEntry> FindGameById(std::string_view game_id) const;

  // Game folders for each emulated system under a games root, as laid out on Xbox.
  static std::vector<std::string> GetSystemFolders(const std::filesystem::path& games_root);
  static constexpr std::wstring_view XBOX_GAMES_ROOT = L"D:\\DevelopmentFiles\\Games";
  static constexpr std::wstring_view USB_GAMES_ROOT = L"E:\\Games";

  static UwpImGuiFrontend::ItemId MakeItemId(std::string_view path);
  static std::string_view GetEsDeSystem(const UICommon::GameFile& game);
  static u32 GetScreenScraperSystem(const UICommon::GameFile& game);

private:
  struct Entry
  {
    UwpImGuiFrontend::LibraryItem item;
    std::shared_ptr<const UICommon::GameFile> game;
  };

  void WorkerThread();
  std::vector<Entry>
  BuildEntries(const std::vector<std::shared_ptr<const UICommon::GameFile>>& games) const;
  UwpImGuiFrontend::LibraryItem BuildItem(const UICommon::GameFile& game) const;

  mutable std::mutex m_mutex;
  std::condition_variable m_wakeup;
  std::vector<Entry> m_entries;
  std::filesystem::path m_metadata_directory;
  std::filesystem::path m_media_directory;
  std::thread m_thread;
  bool m_refresh_requested = false;
  bool m_rebuild_requested = false;
  bool m_shutdown = false;
  std::atomic_bool m_halt = false;
  std::atomic<bool> m_scanning = false;
  std::atomic<bool> m_changed = false;
};
}  // namespace DolphinUWP

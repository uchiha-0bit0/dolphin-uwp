// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GameLibrary.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <UwpImGuiFrontend/ScreenScraper.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/Boot/DolReader.h"
#include "Core/Boot/ElfReader.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/TitleDatabase.h"
#include "DiscIO/Enums.h"
#include "DolphinUWP/UWPSettings.h"
#include "UICommon/GameFile.h"
#include "UICommon/GameFileCache.h"

namespace DolphinUWP
{
namespace
{
using UwpImGuiFrontend::MediaKind;
using UwpImGuiFrontend::ScreenScraperArtwork;

enum class System
{
  GameCube,
  Triforce,
  Wii,
};

// Installed Wii titles are listed with Wii discs, and executables with the console they target.
System GetSystem(const UICommon::GameFile& game)
{
  switch (game.GetPlatform())
  {
  case DiscIO::Platform::Triforce:
    return System::Triforce;
  case DiscIO::Platform::WiiDisc:
  case DiscIO::Platform::WiiWAD:
    return System::Wii;
  case DiscIO::Platform::ELFOrDOL:
  {
    const std::string& path = game.GetFilePath();
    std::string extension;
    SplitPath(path, nullptr, nullptr, &extension);
    Common::ToLower(&extension);
    const bool is_wii = extension == ".elf" ? ElfReader(path).IsWii() : DolReader(path).IsWii();
    return is_wii ? System::Wii : System::GameCube;
  }
  default:
    return System::GameCube;
  }
}

std::string_view GetPlatformName(System system)
{
  switch (system)
  {
  case System::Triforce:
    return "Triforce";
  case System::Wii:
    return "Wii";
  default:
    return "GameCube";
  }
}

std::string_view GetRegionName(DiscIO::Region region)
{
  switch (region)
  {
  case DiscIO::Region::NTSC_J:
    return "Japan";
  case DiscIO::Region::NTSC_U:
    return "USA";
  case DiscIO::Region::PAL:
    return "Europe";
  case DiscIO::Region::NTSC_K:
    return "Korea";
  default:
    return {};
  }
}

std::filesystem::path WriteBanner(const UICommon::GameFile& game)
{
  const UICommon::GameBanner& banner = game.GetBannerImage();
  if (banner.empty() || banner.width == 0 || banner.height == 0)
    return {};

  const std::string path = fmt::format(
      "{}UWP/Banners/{}_{}_{:016x}.png", File::GetUserPath(D_CACHE_IDX), game.GetGameID(),
      game.GetRevision(), GameLibrary::MakeItemId(game.GetFilePath()));
  if (File::Exists(path))
    return StringToPath(path);

  std::vector<u8> rgba(banner.buffer.size() * 4);
  for (size_t i = 0; i < banner.buffer.size(); ++i)
  {
    const u32 pixel = banner.buffer[i];
    rgba[i * 4 + 0] = static_cast<u8>(pixel >> 16);
    rgba[i * 4 + 1] = static_cast<u8>(pixel >> 8);
    rgba[i * 4 + 2] = static_cast<u8>(pixel);
    rgba[i * 4 + 3] = 0xFF;
  }

  File::CreateFullPath(path);
  if (!Common::SavePNG(path, rgba.data(), Common::ImageByteFormat::RGBA, banner.width,
                       banner.height, banner.width * 4))
  {
    return {};
  }
  return StringToPath(path);
}

std::filesystem::path FindContentRoot(const std::string& game_path)
{
  for (const std::string& directory : Config::GetIsoPaths())
  {
    if (!directory.empty() && game_path.starts_with(directory))
      return StringToPath(directory);
  }
  return {};
}
}  // namespace

GameLibrary::GameLibrary() = default;

GameLibrary::~GameLibrary()
{
  Shutdown();
}

std::vector<std::string> GameLibrary::GetSystemFolders(const std::filesystem::path& games_root)
{
  std::vector<std::string> folders;
  for (const char* system : {"gc", "wii", "triforce"})
    folders.push_back(PathToString(games_root / system));
  return folders;
}

void GameLibrary::Initialize(std::filesystem::path metadata_directory)
{
  if (m_thread.joinable())
    return;

  m_metadata_directory = std::move(metadata_directory);
  m_shutdown = false;
  m_halt = false;
  m_refresh_requested = true;
  m_thread = std::thread(&GameLibrary::WorkerThread, this);
}

void GameLibrary::Shutdown()
{
  {
    std::lock_guard lock(m_mutex);
    m_shutdown = true;
  }
  m_halt = true;
  m_wakeup.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

void GameLibrary::Refresh()
{
  {
    std::lock_guard lock(m_mutex);
    m_refresh_requested = true;
  }
  m_wakeup.notify_all();
}

void GameLibrary::Rebuild()
{
  {
    std::lock_guard lock(m_mutex);
    m_rebuild_requested = true;
  }
  m_wakeup.notify_all();
}

bool GameLibrary::ConsumeChanged()
{
  return m_changed.exchange(false);
}

bool GameLibrary::IsScanning() const
{
  return m_scanning;
}

void GameLibrary::SetMediaDirectory(std::filesystem::path directory)
{
  {
    std::lock_guard lock(m_mutex);
    if (m_media_directory == directory)
      return;
    m_media_directory = std::move(directory);
  }
  Refresh();
}

std::vector<UwpImGuiFrontend::LibraryItem> GameLibrary::GetItems() const
{
  std::lock_guard lock(m_mutex);
  std::vector<UwpImGuiFrontend::LibraryItem> items;
  items.reserve(m_entries.size());
  for (const Entry& entry : m_entries)
    items.push_back(entry.item);
  return items;
}

std::optional<UwpImGuiFrontend::LibraryItem>
GameLibrary::FindItem(UwpImGuiFrontend::ItemId id) const
{
  std::lock_guard lock(m_mutex);
  const auto found = std::ranges::find(m_entries, id, [](const Entry& e) { return e.item.id; });
  if (found == m_entries.end())
    return std::nullopt;
  return found->item;
}

std::optional<UwpImGuiFrontend::LibraryItem>
GameLibrary::FindItemByPath(const std::string& path) const
{
  const auto normalize = [](std::filesystem::path value) {
    std::wstring text = value.lexically_normal().wstring();
    std::ranges::transform(text, text.begin(), [](wchar_t c) { return std::towlower(c); });
    return text;
  };

  const std::wstring target = normalize(StringToPath(path));
  std::lock_guard lock(m_mutex);
  const auto found = std::ranges::find_if(
      m_entries, [&](const Entry& entry) { return normalize(entry.item.launchPath) == target; });
  if (found == m_entries.end())
    return std::nullopt;
  return found->item;
}

std::vector<GameLibrary::GameEntry> GameLibrary::GetGameEntries() const
{
  std::lock_guard lock(m_mutex);
  std::vector<GameEntry> entries;
  entries.reserve(m_entries.size());
  for (const Entry& entry : m_entries)
    entries.push_back({entry.item.id, entry.item.name, entry.game});
  return entries;
}

std::optional<GameLibrary::GameEntry> GameLibrary::FindGameById(std::string_view game_id) const
{
  std::lock_guard lock(m_mutex);
  const auto found = std::ranges::find_if(m_entries, [game_id](const Entry& entry) {
    return entry.game && entry.game->GetGameID() == game_id;
  });
  if (found == m_entries.end())
    return std::nullopt;
  return GameEntry{found->item.id, found->item.name, found->game};
}

std::shared_ptr<const UICommon::GameFile> GameLibrary::FindGame(UwpImGuiFrontend::ItemId id) const
{
  std::lock_guard lock(m_mutex);
  const auto found = std::ranges::find(m_entries, id, [](const Entry& e) { return e.item.id; });
  return found == m_entries.end() ? nullptr : found->game;
}

UwpImGuiFrontend::ItemId GameLibrary::MakeItemId(std::string_view path)
{
  u64 hash = 0xcbf29ce484222325ull;
  for (const char c : path)
  {
    hash ^= static_cast<u8>(c == '\\' ? '/' : c);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

std::string_view GameLibrary::GetEsDeSystem(const UICommon::GameFile& game)
{
  return GetSystem(game) == System::Wii ? "wii" : "gc";
}

u32 GameLibrary::GetScreenScraperSystem(const UICommon::GameFile& game)
{
  return GetSystem(game) == System::Wii ? SCREENSCRAPER_WII_SYSTEM_ID :
                                          SCREENSCRAPER_GAMECUBE_SYSTEM_ID;
}

void GameLibrary::WorkerThread()
{
  Common::SetCurrentThreadName("UWP Game List");

  UICommon::GameFileCache cache;
  cache.Load();

  const auto collect = [&cache] {
    std::vector<std::shared_ptr<const UICommon::GameFile>> games;
    cache.ForEach([&games](const std::shared_ptr<const UICommon::GameFile>& game) {
      if (game->IsValid())
        games.push_back(game);
    });
    return games;
  };

  const auto publish = [this](std::vector<Entry> entries) {
    {
      std::lock_guard lock(m_mutex);
      m_entries = std::move(entries);
    }
    m_changed = true;
  };

  publish(BuildEntries(collect()));

  while (true)
  {
    bool refresh = false;
    {
      std::unique_lock lock(m_mutex);
      m_wakeup.wait(lock,
                    [this] { return m_shutdown || m_refresh_requested || m_rebuild_requested; });
      if (m_shutdown)
        break;
      refresh = std::exchange(m_refresh_requested, false);
      m_rebuild_requested = false;
    }

    if (!refresh)
    {
      publish(BuildEntries(collect()));
      continue;
    }

    m_scanning = true;
    const std::vector<std::string> directories = Config::GetIsoPaths();
    const std::vector<std::string_view> directory_views(directories.begin(), directories.end());
    const std::vector<std::string> paths =
        UICommon::FindAllGamePaths(directory_views, Config::Get(Config::MAIN_RECURSIVE_ISO_PATHS));

    bool changed = cache.Update(paths, {}, {}, m_halt);
    changed |= cache.UpdateAdditionalMetadata({}, m_halt);
    if (changed)
      cache.Save();

    publish(BuildEntries(collect()));
    m_scanning = false;
  }
}

std::vector<GameLibrary::Entry>
GameLibrary::BuildEntries(const std::vector<std::shared_ptr<const UICommon::GameFile>>& games) const
{
  std::vector<Entry> entries;
  entries.reserve(games.size());
  for (const auto& game : games)
    entries.push_back({BuildItem(*game), game});

  std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
    return Common::CaseInsensitiveLess{}(a.item.name, b.item.name);
  });
  return entries;
}

UwpImGuiFrontend::LibraryItem GameLibrary::BuildItem(const UICommon::GameFile& game) const
{
  static const Core::TitleDatabase title_database;

  UwpImGuiFrontend::LibraryItem item;
  item.id = MakeItemId(game.GetFilePath());
  item.contentId = game.GetTitleID();
  item.version = game.GetRevision();
  item.name = game.GetName(title_database);
  item.platform = GetPlatformName(GetSystem(game));
  item.region = GetRegionName(game.GetRegion());
  item.format = game.GetFileFormatName();
  item.launchPath = StringToPath(game.GetFilePath());
  item.metadata.description =
      game.GetDescription(UICommon::GameFile::Variant::LongAndPossiblyCustom);
  item.metadata.publisher = game.GetMaker(UICommon::GameFile::Variant::LongAndPossiblyCustom);

  std::filesystem::path media_directory;
  {
    std::lock_guard lock(m_mutex);
    media_directory = m_media_directory;
  }

  static constexpr std::array artworks{
      ScreenScraperArtwork::Box2D,         ScreenScraperArtwork::Box3D,
      ScreenScraperArtwork::MixRecalboxV1, ScreenScraperArtwork::MixRecalboxV2,
      ScreenScraperArtwork::FanArt,        ScreenScraperArtwork::Screenshot,
      ScreenScraperArtwork::Logo,          ScreenScraperArtwork::BackCover,
      ScreenScraperArtwork::PhysicalMedia};
  if (!media_directory.empty())
  {
    const UwpImGuiFrontend::EsDeMediaLayout layout =
        UwpImGuiFrontend::MakeEsDeMediaLayout(media_directory, std::string(GetEsDeSystem(game)),
                                              item.launchPath, FindContentRoot(game.GetFilePath()));
    for (const ScreenScraperArtwork artwork : artworks)
    {
      if (const auto path = UwpImGuiFrontend::FindExistingEsDeArtworkPath(layout, artwork))
        item.media.push_back({UwpImGuiFrontend::ScreenScraperArtworkMediaKind(artwork), *path});
    }
    if (const auto video = UwpImGuiFrontend::FindExistingEsDeVideoPath(layout))
      item.media.push_back({MediaKind::Video, *video});
  }

  // Without an ES-DE folder, the scraper keeps the media of a title with its metadata.
  if (!m_metadata_directory.empty())
  {
    const std::filesystem::path directory = m_metadata_directory / fmt::format("{:016x}", item.id);
    std::error_code error;
    for (const ScreenScraperArtwork artwork : artworks)
    {
      const MediaKind kind = UwpImGuiFrontend::ScreenScraperArtworkMediaKind(artwork);
      const std::filesystem::path path =
          directory / UwpImGuiFrontend::ScreenScraperArtworkFileName(artwork);
      if (!UwpImGuiFrontend::FindMedia(item, kind) && std::filesystem::exists(path, error))
        item.media.push_back({kind, path});
    }
    const std::filesystem::path video = directory / "video.mp4";
    if (!UwpImGuiFrontend::FindMedia(item, MediaKind::Video) &&
        std::filesystem::exists(video, error))
    {
      item.media.push_back({MediaKind::Video, video});
    }
  }

  if (!UwpImGuiFrontend::FindMedia(item, MediaKind::Cover2D))
  {
    const std::string cover_path =
        File::GetUserPath(D_COVERCACHE_IDX) + game.GetGameTDBID() + ".png";
    if (!game.GetGameTDBID().empty() && File::Exists(cover_path))
      item.media.push_back({MediaKind::Cover2D, StringToPath(cover_path)});
    else if (const std::filesystem::path banner = WriteBanner(game); !banner.empty())
      item.media.push_back({MediaKind::Cover2D, banner});
  }

  if (!m_metadata_directory.empty())
  {
    item.metadataPath = m_metadata_directory / fmt::format("{:016x}", item.id) / "metadata.json";
    std::error_code error;
    if (std::filesystem::exists(item.metadataPath, error))
    {
      const auto region = static_cast<UwpImGuiFrontend::ScreenScraperRegion>(
          Config::Get(Config::UWP_SCREENSCRAPER_REGION));
      UwpImGuiFrontend::Metadata scraped = UwpImGuiFrontend::LoadScreenScraperMetadata(
          item.metadataPath, UwpImGuiFrontend::ResolveScreenScraperRegionCode(region, item.region));
      UwpImGuiFrontend::MergeMissingMetadata(scraped, item.metadata);
      item.metadata = std::move(scraped);
    }

    const auto record = UwpImGuiFrontend::LoadGameMetadataRecord(
        m_metadata_directory / fmt::format("{:016x}.json", item.id));
    if (record)
    {
      if (!record->title.empty())
        item.name = record->title;
      UwpImGuiFrontend::Metadata metadata = record->metadata;
      UwpImGuiFrontend::MergeMissingMetadata(metadata, item.metadata);
      item.metadata = std::move(metadata);
    }
  }

  return item;
}
}  // namespace DolphinUWP

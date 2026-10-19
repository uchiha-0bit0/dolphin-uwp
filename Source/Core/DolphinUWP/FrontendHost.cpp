// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/FrontendHost.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iterator>
#include <utility>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <imgui.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.System.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FatFsUtil.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/UWP/Device.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/FreeLookManager.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HotkeyManager.h"
#include "Core/TimePlayed.h"
#include "Core/WiiUtils.h"
#include "DiscIO/Enums.h"
#include "DolphinUWP/GBAScreens.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/InputMappingHost.h"
#include "DolphinUWP/MediaStreamer.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/Recordings.h"
#include "DolphinUWP/SettingsPages.h"
#include "DolphinUWP/ShellWidgets.h"
#include "DolphinUWP/UWPSettings.h"
#include "DolphinUWP/WiiSystemTools.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/UICommon.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

constexpr auto LAUNCH_TRANSITION = std::chrono::milliseconds(600);
constexpr auto OPEN_NETPLAY = static_cast<Frontend::HostCommand>(0x80);
constexpr auto OPEN_TOOLS = static_cast<Frontend::HostCommand>(0x81);
constexpr float PAGE_SCROLL_SPEED = 900.0f;
constexpr size_t LIBRARY_ARTWORK_WINDOW = 16;
constexpr size_t LIBRARY_ARTWORK_LOADS_PER_FRAME = 3;

// The storage presets select the GameCube folder; the Wii and Triforce folders beside it are
// added with it.
std::array<Frontend::StorageLocationPreset, 2> MakeGamesPresets()
{
  return Frontend::MakeXboxStorageLocationPresets(
      std::filesystem::path(GameLibrary::XBOX_GAMES_ROOT) / "gc",
      std::filesystem::path(GameLibrary::USB_GAMES_ROOT) / "gc");
}

// Dolphin's folders on Xbox storage keep the names they have in the user folder.
constexpr std::wstring_view XBOX_DATA_ROOT = L"D:\\DevelopmentFiles\\Dolphin";
constexpr std::wstring_view USB_DATA_ROOT = L"E:\\Dolphin";

struct StorageFolder
{
  std::string_view id;
  const char* label;
  unsigned int user_path;
  const Config::Info<std::string>* info;
  const char* folder;
  const wchar_t* token;
  bool file = false;
  // Dolphin has no setting for this location, so the app applies it to the user path.
  bool applied_by_app = false;
};

constexpr std::array<StorageFolder, 13> STORAGE_FOLDERS{{
    {"load", "Custom textures and mods", D_LOAD_IDX, &Config::MAIN_LOAD_PATH, LOAD_DIR,
     L"dolphin-path-load"},
    {"resource-packs", "Resource packs", D_RESOURCEPACK_IDX, &Config::MAIN_RESOURCEPACK_PATH,
     RESOURCEPACK_DIR, L"dolphin-path-resource-packs"},
    {"dump", "Dumps", D_DUMP_IDX, &Config::MAIN_DUMP_PATH, DUMP_DIR, L"dolphin-path-dump"},
    {"nand", "Wii NAND root", D_WIIROOT_IDX, &Config::MAIN_FS_PATH, WII_USER_DIR,
     L"dolphin-path-nand"},
    {"wfs", "WFS", D_WFSROOT_IDX, &Config::MAIN_WFS_PATH, WFSROOT_DIR, L"dolphin-path-wfs"},
    {"gba-saves", "GBA saves", D_GBASAVES_IDX, &Config::MAIN_GBA_SAVES_PATH,
     GBA_USER_DIR DIR_SEP GBASAVES_DIR, L"dolphin-path-gba-saves"},
    {"sd-image", "SD card image", F_WIISDCARDIMAGE_IDX, &Config::MAIN_WII_SD_CARD_IMAGE_PATH,
     LOAD_DIR DIR_SEP WII_SD_CARD_IMAGE, L"dolphin-path-sd-image", true},
    {"sd-folder", "SD sync folder", D_WIISDCARDSYNCFOLDER_IDX,
     &Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH, LOAD_DIR DIR_SEP WIISDSYNC_DIR,
     L"dolphin-path-sd-folder"},
    {"gc", "Memory cards and IPL", D_GCUSER_IDX, &Config::UWP_GC_PATH, GC_USER_DIR,
     L"dolphin-path-gc", false, true},
    {"controller-profiles", "Controller profiles", D_CONTROLLERPROFILES_IDX,
     &Config::UWP_CONTROLLER_PROFILES_PATH, CONFIG_DIR DIR_SEP CONTROLLER_PROFILES_DIR,
     L"dolphin-path-controller-profiles", false, true},
    {"game-settings", "Per-game settings", D_GAMESETTINGS_IDX, &Config::UWP_GAME_SETTINGS_PATH,
     GAMESETTINGS_DIR, L"dolphin-path-game-settings", false, true},
    {"cache", "Cache", D_CACHE_IDX, &Config::UWP_CACHE_PATH, CACHE_DIR, L"dolphin-path-cache",
     false, true},
    {"graphics-mod-settings", "Graphics mod settings", D_GRAPHICSMODCONFIG_IDX,
     &Config::UWP_GRAPHICS_MOD_SETTINGS_PATH, CONFIG_DIR DIR_SEP GRAPHICSMOD_CONFIG_DIR,
     L"dolphin-path-graphics-mod-settings", false, true},
}};

// A folder path as File::SetUserPath stores it.
std::string UserPathForm(std::string path)
{
  std::ranges::replace(path, '\\', '/');
  while (path.ends_with('/'))
    path.pop_back();
  return path.empty() ? path : path + '/';
}

std::filesystem::path GetStoragePath(std::wstring_view root, const StorageFolder& folder)
{
  return std::filesystem::path(root) / StringToPath(folder.folder).make_preferred();
}

// Creates the folder of a storage location, or the folder that holds it when it is a file.
bool CreateStorageLocation(const std::string& path, bool file)
{
  return File::CreateFullPath(file ? path : path + '/');
}

std::filesystem::path GetLocalFolder()
{
  return std::filesystem::path(
      winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str());
}

Frontend::ScreenScraperArtwork ToArtwork(int value)
{
  return static_cast<Frontend::ScreenScraperArtwork>(
      std::clamp(value, 0, static_cast<int>(Frontend::ScreenScraperArtwork::PhysicalMedia)));
}

std::optional<Frontend::MediaKind> GetBackgroundKind(Frontend::ScreenScraperBackground background)
{
  switch (background)
  {
  case Frontend::ScreenScraperBackground::FanArt:
    return Frontend::MediaKind::FanArt;
  case Frontend::ScreenScraperBackground::Screenshot:
    return Frontend::MediaKind::Screenshot;
  case Frontend::ScreenScraperBackground::MixRecalboxV1:
    return Frontend::MediaKind::MixRecalboxV1;
  case Frontend::ScreenScraperBackground::MixRecalboxV2:
    return Frontend::MediaKind::MixRecalboxV2;
  default:
    return std::nullopt;
  }
}

std::string SanitizeField(std::string_view value, std::string_view forbidden)
{
  std::string result;
  for (const char c : value)
    result.push_back(forbidden.find(c) == std::string_view::npos ? c : ' ');
  return result;
}

std::vector<Frontend::DsuServerModel> ParseDsuServers(std::string_view value)
{
  std::vector<Frontend::DsuServerModel> servers;
  for (const std::string& entry : SplitString(std::string(value), ';'))
  {
    const std::vector<std::string> fields = SplitString(entry, '|');
    if (fields.size() < 5)
      continue;
    Frontend::DsuServerModel server;
    server.id = fields[0];
    server.name = fields[1];
    server.host = fields[2];
    u16 port = 0;
    server.port = TryParse(fields[3], &port) ? port : 26760;
    server.enabled = fields[4] == "1";
    servers.push_back(std::move(server));
  }
  return servers;
}

std::string SerializeDsuServers(const std::vector<Frontend::DsuServerModel>& servers)
{
  std::string result;
  for (const Frontend::DsuServerModel& server : servers)
  {
    result += fmt::format("{}|{}|{}|{}|{};", SanitizeField(server.id, "|;"),
                          SanitizeField(server.name, "|;"), SanitizeField(server.host, "|;"),
                          server.port, server.enabled ? 1 : 0);
  }
  return result;
}

std::string ToDolphinDsuEntries(const std::vector<Frontend::DsuServerModel>& servers)
{
  std::string result;
  for (const Frontend::DsuServerModel& server : servers)
  {
    if (!server.enabled || server.host.empty())
      continue;
    result += fmt::format("{}:{}:{};", SanitizeField(server.name, ":;"),
                          SanitizeField(server.host, ":;"), server.port);
  }
  return result;
}

std::unordered_set<Frontend::ItemId> ParseFavorites(std::string_view value)
{
  std::unordered_set<Frontend::ItemId> favorites;
  for (const std::string& entry : SplitString(std::string(value), ','))
  {
    u64 id = 0;
    if (TryParse("0x" + entry, &id))
      favorites.insert(id);
  }
  return favorites;
}

std::string SerializeFavorites(const std::unordered_set<Frontend::ItemId>& favorites)
{
  std::vector<std::string> entries;
  for (const Frontend::ItemId id : favorites)
    entries.push_back(fmt::format("{:016x}", id));
  std::ranges::sort(entries);
  return fmt::format("{}", fmt::join(entries, ","));
}

const Frontend::ShellTheme& GetTheme(int index)
{
  const Frontend::ShellThemeCollection& themes = Frontend::ShellThemes();
  return themes[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(themes.size()) - 1))];
}

// Carries companion extension datagrams over the DSU client's socket.
class CompanionTransport final : public Frontend::IExtensionPacketTransport
{
public:
  void RequestControllerData(u8 slot) override { ciface::DualShockUDPClient::RequestPadData(slot); }

  void SendExtensionPacket(std::vector<u8> packet, bool) override
  {
    ciface::DualShockUDPClient::SendToServers(packet);
  }

  void SendExtensionPacketTo(u32 peer, std::vector<u8> packet, bool) override
  {
    ciface::DualShockUDPClient::SendToServer(peer, packet);
  }
};

bool IsIntegratedGBA(size_t port)
{
#ifdef HAS_LIBMGBA
  return Config::Get(Config::GetInfoForSIDevice(static_cast<int>(port))) ==
         SerialInterface::SIDEVICE_GC_GBA_EMULATED;
#else
  return false;
#endif
}

// A phone plays as the player of its first DSU slot, and its secondary screen is that player's
// GBA. A phone without a controller gets the screen it asked for.
size_t GetCompanionPlayer(u32 peer, u8 screen)
{
  const u8 pads = ciface::DualShockUDPClient::GetConnectedPads(peer);
  return pads != 0 ? static_cast<size_t>(std::countr_zero(pads)) : static_cast<size_t>(screen) - 1;
}

Frontend::WidgetInputState ToWidgetInput(const Frontend::FrameInput& input)
{
  return {input.accept, input.back,  input.up,           input.down,
          input.left,   input.right, input.leftShoulder, input.rightShoulder};
}

bool HasCommandInput(const Frontend::FrameInput& input)
{
  return input.up || input.down || input.left || input.right || input.accept || input.back ||
         input.context || input.alternate || input.menu || input.view || input.leftShoulder ||
         input.rightShoulder || input.pointerPressed || std::abs(input.leftStickX) > 0.2f ||
         std::abs(input.leftStickY) > 0.2f || std::abs(input.rightStickX) > 0.2f ||
         std::abs(input.rightStickY) > 0.2f;
}
}  // namespace

bool FrontendHost::DirectionRepeat::Update(bool held, float delta_seconds)
{
  if (!held)
  {
    Reset();
    return false;
  }
  if (!was_held)
  {
    was_held = true;
    return true;
  }
  elapsed += std::clamp(delta_seconds, 0.0f, 0.1f);
  if (elapsed < next_repeat)
    return false;
  elapsed = 0.0f;
  next_repeat = 0.08f;
  return true;
}

void FrontendHost::DirectionRepeat::Reset()
{
  was_held = false;
  elapsed = 0.0f;
  next_repeat = 0.30f;
}

FrontendHost::FrontendHost(GameLibrary& library, InputMappingHost& mapping, SettingsPages& settings)
    : m_library(library), m_mapping_host(mapping), m_settings_pages(settings)
{
}

FrontendHost::~FrontendHost()
{
  Shutdown();
}

void FrontendHost::Initialize(Callbacks callbacks, ImFont* text_font, ImFont* shell_font)
{
  if (m_initialized)
    return;

  m_callbacks = std::move(callbacks);
  m_text_font = text_font;
  m_shell_font = shell_font;
  LoadSettings();
  m_settings_pages.SetActionHandler([this](std::string_view action) { RunAction(action); });
  m_settings_pages.SetStorageDrawer([this](std::string_view id) { DrawStorageLocation(id); });
  m_mods_page.SetStorageDrawer([this](std::string_view id) { DrawStorageLocation(id); });
  m_game_settings_page.SetStorageDrawer([this](std::string_view id) { DrawStorageLocation(id); });
  m_mapping_host.SetProfileLocationHandlers(
      [this] { return GetStorageChoices("controller-profiles"); },
      [this](std::string_view choice) { return SelectStorage("controller-profiles", choice); });

  m_screen_scraper = Frontend::CreateScreenScraperService(
      [this](Frontend::LogLevel level, std::string_view message) { Log(level, message); });
  m_companion = Frontend::CreateCompanionService();
  m_credentials = Frontend::CreatePasswordVaultCredentialStore(L"Dolphin.ScreenScraper");
  m_picker = Frontend::CreatePickerService();
  m_metadata_editor = std::make_unique<Frontend::MetadataEditor>(*m_screen_scraper);

  m_services.library = &m_library;
  m_services.task = &m_task;
  m_services.picker = m_picker.get();
  m_services.text_font = m_text_font;
  m_services.notify = [this](std::string message) { Notify(std::move(message)); };
  m_services.boot = [this](BootRequest request) { QueueBoot(std::move(request)); };
  m_services.boot_file = [this](std::string path) { QueueBootFile(path); };
  m_services.create_texture = m_callbacks.create_texture;
  m_services.release_texture = m_callbacks.release_texture;

  m_mapping_host.SetDsuServerEnumerator([this] { return GetDsuServers(); });
  m_mapping_host.SetGameIdResolver([this](std::uint64_t id) -> std::string {
    const auto game = m_library.FindGame(id);
    return game ? game->GetGameID() : std::string();
  });

  Frontend::FrontendConfiguration configuration = Frontend::MakeDefaultFrontendConfiguration();
  configuration.applicationName = "Dolphin";
  configuration.platformName = "GameCube & Wii";
  configuration.emptyCatalogueLabel = "Add games folder";
  configuration.logo = "icons/dolphin.png";
  configuration.animateLaunches = m_interface.animateLaunches;
  const auto settings =
      std::ranges::find(configuration.leftRail, "settings", &Frontend::ShellAction::id);
  configuration.leftRail.insert(
      settings, Frontend::ShellAction{"netplay", "NetPlay", "icons/netplay.png", OPEN_NETPLAY});
  const auto power =
      std::ranges::find(configuration.rightRail, "power", &Frontend::ShellAction::id);
  configuration.rightRail.insert(
      power, Frontend::ShellAction{"tools", "Tools", "icons/tools.png", OPEN_TOOLS});
  for (Frontend::ShellAction& action : configuration.leftRail)
  {
    if (action.id == "library")
    {
      action.label = "Search";
      action.icon = "icons/search.png";
    }
    else if (action.id == "settings")
    {
      action.icon = "icons/gear.png";
    }
  }
  for (Frontend::ShellAction& action : configuration.rightRail)
  {
    if (action.id == "controllers")
      action.icon = "icons/gamepad.png";
    else if (action.id == "graphics")
      action.icon = "icons/display.png";
  }
  for (Frontend::ActionHint& hint : configuration.actionHints)
  {
    if (hint.button == "View")
      hint.label = "Search";
  }
  configuration.backgroundShapes = {"icons/bg-gamecube.png", "icons/bg-wii-remote.png",
                                    "icons/bg-nunchuk.png", "icons/bg-classic.png"};
  m_frontend = std::make_unique<Frontend::Frontend>(*this, *m_screen_scraper, *m_companion,
                                                    std::move(configuration));
  m_shell_renderer = std::make_unique<Frontend::ShellRenderer>(*this);

  m_catalogue = m_library.GetItems();
  m_frontend->Initialize();

  ciface::DualShockUDPClient::SetExtensionHandler(
      [companion = m_companion.get()](u32 server, std::span<const u8> datagram,
                                      std::vector<std::vector<u8>>& responses) {
        return companion->HandlePeerExtensionPacket(server, datagram, responses);
      });
  m_companion_transport = std::make_unique<CompanionTransport>();
  m_companion->AttachTransport(m_companion_transport.get());
  m_initialized = true;
}

void FrontendHost::Shutdown()
{
  if (!m_initialized)
    return;

  CloseContextMenu();
  ReleaseLibraryArtwork();
  m_input_mapper.Close();
  m_tools.Close();
  m_netplay_page.Close();
  m_netplay.Leave();
  if (m_picker)
    m_picker->CancelCallbacks();
  m_settings_pages.Flush();
  m_shell_renderer.reset();
  ciface::DualShockUDPClient::SetExtensionHandler({});
  m_companion->DetachTransport(m_companion_transport.get());
  m_frontend->Shutdown();
  m_frontend.reset();
  m_metadata_editor.reset();
  m_companion.reset();
  m_screen_scraper.reset();
  MediaStreamer::StopAll();
  m_initialized = false;
}

void FrontendHost::LoadSettings()
{
  m_interface.themeIndex = Config::Get(Config::UWP_THEME);
  m_interface.showActionHints = Config::Get(Config::UWP_SHOW_ACTION_HINTS);
  m_interface.animateLaunches = Config::Get(Config::UWP_ANIMATE_LAUNCHES);
  m_favorites = ParseFavorites(Config::Get(Config::UWP_FAVORITES));

  Frontend::ScreenScraperPanelModel& scraper = m_screen_scraper_model;
  scraper.enabled = Config::Get(Config::UWP_SCREENSCRAPER_ENABLED);
  const int downloads = Config::Get(Config::UWP_SCREENSCRAPER_DOWNLOADS);
  scraper.options.downloadMetadata = (downloads & 0x01) != 0;
  scraper.options.downloadBoxArt = (downloads & 0x02) != 0;
  scraper.options.downloadFanArt = (downloads & 0x04) != 0;
  scraper.options.downloadVideos = (downloads & 0x08) != 0;
  scraper.options.downloadScreenshots = (downloads & 0x10) != 0;
  scraper.options.downloadLogos = (downloads & 0x20) != 0;
  scraper.options.overwriteExisting = Config::Get(Config::UWP_SCREENSCRAPER_OVERWRITE);
  scraper.options.preferredRegion = static_cast<Frontend::ScreenScraperRegion>(
      std::clamp(Config::Get(Config::UWP_SCREENSCRAPER_REGION), 0,
                 static_cast<int>(Frontend::ScreenScraperRegion::World)));
  scraper.listArtwork = ToArtwork(Config::Get(Config::UWP_SCREENSCRAPER_LIST_ARTWORK));
  scraper.detailArtwork = ToArtwork(Config::Get(Config::UWP_SCREENSCRAPER_DETAIL_ARTWORK));
  scraper.dynamicBackground = static_cast<Frontend::ScreenScraperBackground>(
      std::clamp(Config::Get(Config::UWP_SCREENSCRAPER_BACKGROUND), 0,
                 static_cast<int>(Frontend::ScreenScraperBackground::MixRecalboxV2)));
  scraper.developerCredentialsConfigured = !GetDeveloperIdentity().id.empty();
  scraper.progressFile = StateRoot() / "screenscraper-progress.json";
  scraper.mediaDirectory = StringToPath(Config::Get(Config::UWP_SCREENSCRAPER_MEDIA_DIRECTORY));
  const auto presets = Frontend::MakeXboxStorageLocationPresets(
      L"D:\\DevelopmentFiles\\ES-DE\\downloaded_media", L"E:\\ES-DE\\downloaded_media");
  scraper.mediaDirectoryPresets.assign(presets.begin(), presets.end());
  scraper.mediaDirectoryPicker.futureAccessToken = L"dolphin-screenscraper-media";
  m_library.SetMediaDirectory(scraper.mediaDirectory);

  m_companion_model.servers = ParseDsuServers(Config::Get(Config::UWP_DSU_SERVERS));
  m_companion_model.streamingEnabled = Config::Get(Config::UWP_STREAMING_ENABLED);
  m_companion_model.streamingUrl = Config::Get(Config::UWP_STREAMING_URL);

  m_link_groups = Frontend::MakeDefaultCommunityLinkGroups();
  m_link_groups.push_back({"dolphin",
                           "Dolphin",
                           {{"website", "Dolphin website", "https://dolphin-emu.org/"},
                            {"wiki", "Dolphin wiki", "https://wiki.dolphin-emu.org/"},
                            {"source", "Source code", "https://github.com/dolphin-emu/dolphin"}}});
}

void FrontendHost::SaveInterfaceSettings()
{
  Config::SetBase(Config::UWP_THEME, m_interface.themeIndex);
  Config::SetBase(Config::UWP_SHOW_ACTION_HINTS, m_interface.showActionHints);
  Config::SetBase(Config::UWP_ANIMATE_LAUNCHES, m_interface.animateLaunches);
  Config::Save();
  if (m_frontend)
    m_frontend->SetAnimateLaunches(m_interface.animateLaunches);
}

void FrontendHost::SaveScreenScraperSettings()
{
  const Frontend::ScreenScraperPanelModel& scraper = m_screen_scraper_model;
  const Frontend::ScreenScraperOptions& options = scraper.options;
  const int downloads =
      (options.downloadMetadata ? 0x01 : 0) | (options.downloadBoxArt ? 0x02 : 0) |
      (options.downloadFanArt ? 0x04 : 0) | (options.downloadVideos ? 0x08 : 0) |
      (options.downloadScreenshots ? 0x10 : 0) | (options.downloadLogos ? 0x20 : 0);
  Config::SetBase(Config::UWP_SCREENSCRAPER_ENABLED, scraper.enabled);
  Config::SetBase(Config::UWP_SCREENSCRAPER_DOWNLOADS, downloads);
  Config::SetBase(Config::UWP_SCREENSCRAPER_OVERWRITE, options.overwriteExisting);
  Config::SetBase(Config::UWP_SCREENSCRAPER_REGION, static_cast<int>(options.preferredRegion));
  Config::SetBase(Config::UWP_SCREENSCRAPER_LIST_ARTWORK, static_cast<int>(scraper.listArtwork));
  Config::SetBase(Config::UWP_SCREENSCRAPER_DETAIL_ARTWORK,
                  static_cast<int>(scraper.detailArtwork));
  Config::SetBase(Config::UWP_SCREENSCRAPER_BACKGROUND,
                  static_cast<int>(scraper.dynamicBackground));
  Config::SetBase(Config::UWP_SCREENSCRAPER_MEDIA_DIRECTORY, PathToString(scraper.mediaDirectory));
  Config::Save();
  m_library.SetMediaDirectory(scraper.mediaDirectory);
}

void FrontendHost::SaveCompanionSettings()
{
  Config::SetBase(Config::UWP_DSU_SERVERS, SerializeDsuServers(m_companion_model.servers));
  Config::SetBase(Config::UWP_STREAMING_ENABLED, m_companion_model.streamingEnabled);
  Config::SetBase(Config::UWP_STREAMING_URL, m_companion_model.streamingUrl);

  const std::string entries = ToDolphinDsuEntries(m_companion_model.servers);
  Config::SetBase(ciface::DualShockUDPClient::Settings::SERVERS, entries);
  Config::SetBase(ciface::DualShockUDPClient::Settings::SERVERS_ENABLED, !entries.empty());
  Config::Save();
}

void FrontendHost::RefreshCatalogue()
{
  ReleaseLibraryArtwork();
  m_catalogue = m_library.GetItems();
  m_library_items_dirty = true;
  if (m_frontend)
    m_frontend->RefreshCatalogue();
  if (m_context_item && !FindCatalogueItem(*m_context_item))
    CloseContextMenu();
}

void FrontendHost::DrawFrame(const Frontend::FrameInput& input, std::string controller_status,
                             std::optional<float> controller_battery)
{
  if (!m_initialized)
    return;
  m_settings_pages.Update();

  if (m_library.ConsumeChanged())
    RefreshCatalogue();
  if (m_screen_scraper->ConsumeFinished())
  {
    m_library.Rebuild();
    Notify("ScreenScraper download finished");
  }

  m_task.Tick();
  m_mapping_page.Update();

  const Frontend::FrameInput active_input = m_pending_launch ? Frontend::FrameInput{} : input;
  const Frontend::ShellPresentation presentation =
      BuildPresentation(std::move(controller_status), controller_battery);
  const Frontend::ShellTheme& theme = GetTheme(m_interface.themeIndex);
  const bool modal = PromptService::GetInstance().GetActivePrompt() || m_task.IsRunning();

  Frontend::BeginNativeTextInputFrame();
  Frontend::BeginWidgetFrame(theme, ToWidgetInput(active_input), ToWidgetInput(m_previous_input),
                             OwnsOverlayInput() || modal);

  const bool overlay_owned_at_start = OwnsOverlayInput();
  DrawModal();
  if (modal)
    m_shell_input_release_barrier = true;
  else if (!HandleOverlayInput(active_input, m_previous_input) && !m_shell_input_release_barrier)
    m_frontend->HandleInput(active_input);

  m_shell_renderer->Draw(*m_frontend, presentation, input.deltaSeconds);
  DrawOverlayPages();

  const bool overlay_owned_at_end = OwnsOverlayInput();
  if (overlay_owned_at_start && !overlay_owned_at_end)
    m_shell_input_release_barrier = true;
  if (!overlay_owned_at_end && m_shell_input_release_barrier && !HasCommandInput(active_input))
  {
    m_frontend->HandleInput(active_input);
    m_shell_input_release_barrier = false;
  }

  Frontend::EndWidgetFrame();
  Frontend::EndNativeTextInputFrame();
  m_previous_input = active_input;
}

void FrontendHost::Tick()
{
  m_netplay.Tick();
}

Frontend::ShellPresentation FrontendHost::BuildPresentation(std::string controller_status,
                                                            std::optional<float> controller_battery)
{
  Frontend::ShellPresentation presentation;
  presentation.themeIndex = m_interface.themeIndex;
  presentation.font = m_shell_font;
  presentation.showActionHints = m_interface.showActionHints;
  presentation.controllerStatus = std::move(controller_status);
  presentation.showControllerBattery = controller_battery.has_value();
  presentation.controllerBatteryLevel = controller_battery.value_or(0.0f);
  presentation.listArtwork =
      Frontend::ScreenScraperArtworkMediaKind(m_screen_scraper_model.listArtwork);

  if (const auto background = GetBackgroundKind(m_screen_scraper_model.dynamicBackground))
  {
    const auto& catalogue = m_frontend->Catalogue();
    const size_t selected = m_frontend->State().SelectedIndex();
    if (selected < catalogue.size())
      presentation.backgroundItem = catalogue[selected].id;
    presentation.backgroundArtwork = background;
    presentation.overrideThemeDynamicBackground = true;
  }
  return presentation;
}

void FrontendHost::ReleaseGraphicsResources()
{
  CloseContextMenu();
  ReleaseLibraryArtwork();
  Frontend::DismissNativeTextInput();
  if (m_shell_renderer)
    m_shell_renderer->ClearTextures();
  m_mapping_host.ReleaseTextures();
}

void FrontendHost::DrawModal()
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  if (!viewport)
    return;

  const Frontend::ShellLayoutContext context =
      Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  const Frontend::ShellTheme& theme = Frontend::CurrentWidgetTheme();
  auto prompt = PromptService::GetInstance().GetActivePrompt();
  if (prompt && prompt->system_dialog)
    prompt.reset();
  const auto task = m_task.GetSnapshot();

  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr ImGuiWindowFlags host_flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
  ImGui::Begin("DolphinModalHost", nullptr, host_flags);

  constexpr const char* popup_id = "dolphin-modal";
  if (!prompt && !task)
  {
    m_modal_focus_key = 0;
    if (ImGui::IsPopupOpen(popup_id) &&
        Frontend::BeginThemedModal(popup_id, context, theme, Frontend::ToPx(context, 620.0f, 0.0f)))
    {
      ImGui::CloseCurrentPopup();
      Frontend::EndThemedModal();
    }
    ImGui::End();
    return;
  }

  if (!ImGui::IsPopupOpen(popup_id))
    ImGui::OpenPopup(popup_id);

  // Each new prompt or task starts with focus on its non-destructive button.
  const u64 focus_key = prompt ? prompt->id : ~u64{0};
  const bool focus_default = std::exchange(m_modal_focus_key, focus_key) != focus_key;
  const auto focus_next = [focus_default] {
    if (!focus_default)
      return;
    ImGui::SetKeyboardFocusHere();
    ImGui::SetNavCursorVisible(true);
  };

  if (Frontend::BeginThemedModal(popup_id, context, theme, Frontend::ToPx(context, 620.0f, 0.0f)))
  {
    const ImVec2 button_size = Frontend::ToPx(context, 170.0f, 46.0f);
    if (prompt)
    {
      ImGui::PushID(static_cast<int>(prompt->id));
      Frontend::SectionTitle(prompt->caption.empty() ? "Dolphin" : prompt->caption.c_str());
      ImGui::TextWrapped("%s", prompt->text.c_str());
      ImGui::Spacing();
      std::optional<bool> answer;
      if (prompt->question)
      {
        if (Frontend::ShellButton("Yes", button_size))
          answer = true;
        ImGui::SameLine();
        focus_next();
        if (Frontend::ShellButton("No", button_size) || Frontend::ConsumeWidgetBackPressed())
          answer = false;
      }
      else
      {
        focus_next();
        if (Frontend::ShellButton("OK", button_size) || Frontend::ConsumeWidgetBackPressed())
          answer = true;
      }
      if (answer)
        PromptService::GetInstance().Answer(prompt->id, *answer);
      ImGui::PopID();
    }
    else
    {
      Frontend::SectionTitle(task->title.c_str());
      Frontend::DrawProgressDialogBody(task->fraction < 0.0f ? "Working..." : "",
                                       std::max(task->fraction, 0.0f), task->detail);
      if (task->cancellable)
      {
        ImGui::Spacing();
        focus_next();
        if (Frontend::ShellButton(task->cancelled ? "Cancelling..." : "Cancel", button_size) ||
            Frontend::ConsumeWidgetBackPressed())
        {
          m_task.Cancel();
        }
      }
      else
      {
        (void)Frontend::ConsumeWidgetBackPressed();
      }
    }
    Frontend::EndThemedModal();
  }
  ImGui::End();
}

void FrontendHost::Notify(std::string message)
{
  if (m_frontend)
    m_frontend->PushNotification({std::move(message), 4.0f});
}

std::optional<BootRequest> FrontendHost::TakePendingBoot()
{
  std::lock_guard lock(m_boot_mutex);
  return std::exchange(m_pending_boot, std::nullopt);
}

void FrontendHost::QueueBoot(BootRequest request)
{
  std::lock_guard lock(m_boot_mutex);
  m_pending_boot = std::move(request);
}

void FrontendHost::QueueBootFile(const std::string& path)
{
  std::optional<Frontend::ItemId> item;
  std::string title;
  if (const auto entry = m_library.FindItemByPath(path))
  {
    item = entry->id;
    title = entry->name;
  }
  else
  {
    title = PathToString(StringToPath(path).stem());
  }
  QueueBoot({BootParameters::GenerateFromFile(path), std::move(title), item});
}

void FrontendHost::RefreshLibrary()
{
  m_library.Refresh();
}

std::optional<Frontend::ItemId> FrontendHost::TakePendingLaunch()
{
  if (!m_pending_launch)
    return std::nullopt;

  const bool animated = m_active_page.empty() && m_interface.animateLaunches;
  if (animated && std::chrono::steady_clock::now() - m_pending_launch_time < LAUNCH_TRANSITION)
    return std::nullopt;
  return std::exchange(m_pending_launch, std::nullopt);
}

void FrontendHost::SetRunningContent(std::optional<Frontend::ItemId> item)
{
  m_running_item = item;
  if (m_companion)
    m_companion->UpdateRunningContent(GetRunningContent());
}

bool FrontendHost::OwnsOverlayInput() const
{
  return !m_active_page.empty() || m_context_item.has_value();
}

std::vector<Frontend::LibraryItem> FrontendHost::SnapshotCatalogue()
{
  std::vector<Frontend::LibraryItem> catalogue = m_library.GetItems();
  std::lock_guard lock(m_companion_mutex);
  (void)std::ranges::stable_partition(catalogue, [this](const Frontend::LibraryItem& item) {
    return m_favorites.contains(item.id);
  });
  return catalogue;
}

Frontend::RunningContent FrontendHost::GetRunningContent() const
{
  if (!m_running_item)
    return {};
  const Frontend::LibraryItem* const item = FindCatalogueItem(*m_running_item);
  return {*m_running_item, item ? item->name : std::string(), true};
}

std::vector<Frontend::DsuServerModel> FrontendHost::GetDsuServers() const
{
  std::lock_guard lock(m_companion_mutex);
  return m_companion_model.servers;
}

Frontend::DsuEndpoint FrontendHost::GetDsuEndpoint() const
{
  std::lock_guard lock(m_companion_mutex);
  Frontend::DsuEndpoint endpoint = Frontend::MakeCompanionEndpoint(m_companion_model);
  endpoint.mainStreamingSupported = true;
  // A phone shows one stream at a time, so a new request replaces its previous stream.
  bool gba = false;
  for (size_t port = 0; port < GBAScreens::MAX_SCREENS; ++port)
    gba |= IsIntegratedGBA(port);
  endpoint.secondaryStreamingSupported = gba;
  endpoint.auxiliaryStreamingScreenMask = gba ? 0x0F : 0;
  endpoint.independentScreenStreamsSupported = false;
  return endpoint;
}

bool FrontendHost::StartStream(const Frontend::StreamRequest& request)
{
  Frontend::StreamRequest screen_request = request;
  if (screen_request.screenId == 0 && screen_request.secondaryScreen)
    screen_request.screenId = 1;
  return StartScreenStream(screen_request);
}

void FrontendHost::StopStream(bool secondary_screen)
{
  StopPeerScreenStream(0, secondary_screen ? 1 : 0);
}

std::optional<Frontend::ActiveStream> FrontendHost::GetActiveStream() const
{
  return GetActivePeerScreenStream(0, 0);
}

std::optional<std::string> FrontendHost::TakeStreamFailure()
{
  return TakePeerScreenStreamFailure(0, 0);
}

bool FrontendHost::StartScreenStream(const Frontend::StreamRequest& request)
{
  const u8 screen = request.screenId;
  MediaStreamer::Source source = MediaStreamer::Source::Game;
  std::string error;
  if (screen != 0)
  {
    const size_t player = GetCompanionPlayer(request.peer, screen);
    if (player < GBAScreens::MAX_SCREENS && GBAScreens::IsRunning(player))
      source = MediaStreamer::GBASource(player);
    else
      error = fmt::format("Player {} has no Game Boy Advance", player + 1);
  }
  if (error.empty() && MediaStreamer::Start(request.peer, screen, source, request, &error))
    return true;

  std::lock_guard lock(m_stream_mutex);
  m_stream_failures[{request.peer, screen}] = std::move(error);
  return false;
}

void FrontendHost::StopScreenStream(u8 screen)
{
  StopPeerScreenStream(0, screen);
}

std::optional<Frontend::ActiveStream> FrontendHost::GetActiveScreenStream(u8 screen) const
{
  return GetActivePeerScreenStream(0, screen);
}

std::optional<std::string> FrontendHost::TakeScreenStreamFailure(u8 screen)
{
  return TakePeerScreenStreamFailure(0, screen);
}

void FrontendHost::StopPeerScreenStream(u32 peer, u8 screen)
{
  MediaStreamer::Stop(peer, screen);
}

std::optional<Frontend::ActiveStream> FrontendHost::GetActivePeerScreenStream(u32 peer,
                                                                              u8 screen) const
{
  return MediaStreamer::GetActiveStream(peer, screen);
}

std::optional<std::string> FrontendHost::TakePeerScreenStreamFailure(u32 peer, u8 screen)
{
  if (std::optional<std::string> failure = MediaStreamer::TakeFailure(peer, screen))
    return failure;
  std::lock_guard lock(m_stream_mutex);
  const auto it = m_stream_failures.find({peer, screen});
  if (it == m_stream_failures.end())
    return std::nullopt;
  std::string failure = std::move(it->second);
  m_stream_failures.erase(it);
  return failure;
}

void FrontendHost::Log(Frontend::LogLevel level, std::string_view message)
{
  switch (level)
  {
  case Frontend::LogLevel::Error:
    ERROR_LOG_FMT(COMMON, "Frontend: {}", message);
    break;
  case Frontend::LogLevel::Warning:
    WARN_LOG_FMT(COMMON, "Frontend: {}", message);
    break;
  case Frontend::LogLevel::Information:
    INFO_LOG_FMT(COMMON, "Frontend: {}", message);
    break;
  default:
    DEBUG_LOG_FMT(COMMON, "Frontend: {}", message);
    break;
  }
}

std::filesystem::path FrontendHost::ResourceRoot() const
{
  return std::filesystem::path(winrt::Windows::ApplicationModel::Package::Current()
                                   .InstalledLocation()
                                   .Path()
                                   .c_str()) /
         "resources";
}

std::filesystem::path FrontendHost::StateRoot() const
{
  return GetLocalFolder() / "Frontend";
}

Frontend::TextureHandle FrontendHost::LoadTexture(const std::filesystem::path& path)
{
  return m_callbacks.load_texture ? m_callbacks.load_texture(path) : Frontend::TextureHandle{};
}

void FrontendHost::ReleaseTexture(Frontend::TextureHandle texture)
{
  if (m_callbacks.release_texture)
    m_callbacks.release_texture(texture);
}

bool FrontendHost::RequestLaunch(Frontend::ItemId item)
{
  if (m_pending_launch || !FindCatalogueItem(item))
    return false;

  m_pending_launch = item;
  m_pending_launch_time = std::chrono::steady_clock::now();
  return true;
}

void FrontendHost::OpenContextMenu(Frontend::ItemId item)
{
  CloseContextMenu();
  const Frontend::LibraryItem* const found = FindCatalogueItem(item);
  if (!found)
    return;

  m_context_item = item;
  m_title_options_state = {};
  m_title_page = TitlePage::None;
  const auto game = m_library.FindGame(item);
  m_context_title_installed = game && game->GetPlatform() == DiscIO::Platform::WiiWAD &&
                              WiiUtils::IsTitleInstalled(game->GetTitleID());
  const Frontend::MediaAsset* artwork = Frontend::FindMedia(
      *found, Frontend::ScreenScraperArtworkMediaKind(m_screen_scraper_model.detailArtwork));
  if (!artwork)
    artwork = Frontend::FindMedia(*found, Frontend::MediaKind::Cover2D);
  if (!artwork)
    artwork = Frontend::FindMedia(*found, Frontend::MediaKind::Cover3D);
  if (artwork)
    m_context_artwork = LoadTexture(artwork->path);
}

void FrontendHost::Execute(Frontend::HostCommand command)
{
  if (command == OPEN_TOOLS)
  {
    OpenTools();
    return;
  }
  if (command == OPEN_NETPLAY)
  {
    m_settings_pages.Flush();
    m_active_page = "netplay";
    m_netplay_page.Open();
    return;
  }

  switch (command)
  {
  case Frontend::HostCommand::AddContent:
    m_active_page = "library";
    m_library_view.OpenAddGamesFolder();
    break;
  case Frontend::HostCommand::OpenHome:
    ClosePage();
    CloseContextMenu();
    break;
  case Frontend::HostCommand::OpenSettings:
    m_active_page = "settings";
    break;
  case Frontend::HostCommand::OpenControllers:
    m_active_page = "controllers";
    OpenInputMapper(std::nullopt);
    break;
  case Frontend::HostCommand::OpenGraphics:
    m_active_page = "graphics";
    break;
  case Frontend::HostCommand::OpenTitleManager:
    m_active_page = "library";
    break;
  case Frontend::HostCommand::CycleTheme:
    m_interface.themeIndex =
        (m_interface.themeIndex + 1) % static_cast<int>(Frontend::ShellThemes().size());
    SaveInterfaceSettings();
    Notify(fmt::format("Theme: {}", GetTheme(m_interface.themeIndex).name));
    break;
  case Frontend::HostCommand::ExitApplication:
    winrt::Windows::ApplicationModel::Core::CoreApplication::Exit();
    break;
  default:
    break;
  }
}

void FrontendHost::ClosePage()
{
  if (m_active_page == "tools")
    m_tools.Close();
  else if (m_active_page == "netplay")
    m_netplay_page.Close();
  m_mapping_page.Close();
  if (m_mapper_scope == MapperScope::Global)
  {
    m_input_mapper.Close();
    m_mapper_scope = MapperScope::None;
  }
  m_active_page.clear();
  ReleaseLibraryArtwork();
  m_settings_pages.Flush();
}

void FrontendHost::DrawSettingsPage(std::string_view page_id)
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  if (!viewport)
    return;

  const Frontend::ShellLayoutContext context =
      Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  constexpr float panel_inset = 32.0f;
  Frontend::UnitRect panel{panel_inset, panel_inset, context.layoutUnitsW - panel_inset * 2.0f,
                           context.layoutUnitsH - panel_inset * 2.0f};
  if (page_id == "context" && m_metadata_editor && m_metadata_editor->IsOpen())
  {
    const bool search = m_metadata_editor->View() == Frontend::MetadataEditorView::MatchBrowser;
    const float width = std::min(search ? context.layoutUnitsW - 36.0f : 1080.0f,
                                 std::max(420.0f, context.layoutUnitsW - (search ? 36.0f : 80.0f)));
    const float height =
        std::min(search ? context.layoutUnitsH - 32.0f : 650.0f,
                 std::max(360.0f, context.layoutUnitsH - (search ? 32.0f : 48.0f)));
    panel = Frontend::CenteredPanel(context, width, height);
  }

  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                     ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoBackground;
  if (!ImGui::Begin("DolphinFrontendPage", nullptr, flags))
  {
    ImGui::End();
    return;
  }

  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const auto draw_notifications = [&] {
    const auto notifications = m_frontend->Notifications();
    Frontend::DrawNotificationCards(draw, context, Frontend::CurrentWidgetTheme(), notifications,
                                    Frontend::ResolveFrontendTextFont(m_text_font));
  };

  if ((page_id == "controllers" && m_mapper_scope == MapperScope::Global) ||
      (page_id == "context" && m_mapper_scope == MapperScope::Title))
  {
    DrawInputMapper(context, draw);
    draw_notifications();
    ImGui::End();
    return;
  }

  Frontend::DrawDimmedBackground();
  if (page_id == "context" && m_title_page == TitlePage::None)
  {
    if (m_metadata_editor && m_metadata_editor->IsOpen())
      Frontend::DrawOverlayPanel(draw, context, panel, Frontend::CurrentWidgetTheme());
    DrawContextMenu(context, draw, panel);
    draw_notifications();
    ImGui::End();
    return;
  }

  Frontend::DrawOverlayPanel(draw, context, panel, Frontend::CurrentWidgetTheme(), 0.96f);
  const Frontend::UnitRect content = panel.Shrunk(30.0f);
  ImGui::SetCursorScreenPos(Frontend::RectMinPx(context, content));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.0f);
  ImGui::BeginChild("dolphin-page-content", Frontend::ToPx(context, content.w, content.h), false,
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NavFlattened |
                        ImGuiWindowFlags_NoBackground);
  if (page_id == "settings")
    DrawSettings();
  else if (page_id == "hotkeys" || page_id == "freelook-controls")
    DrawMappingPage();
  else if (page_id == "library")
    DrawLibrary();
  else if (page_id == "graphics")
    DrawGraphics();
  else if (page_id == "tools" && m_tools.Draw(std::exchange(m_page_scroll, 0.0f)))
    ClosePage();
  else if (page_id == "netplay" && m_netplay_page.Draw(std::exchange(m_page_scroll, 0.0f)))
    ClosePage();
  else if (page_id == "context")
    DrawTitlePage();
  ImGui::EndChild();
  ImGui::PopStyleVar(2);

  if (page_id != "context" && !m_active_page.empty() &&
      !Frontend::IsWidgetBackNavigationConsumed() && Frontend::ConsumeWidgetBackPressed())
  {
    Frontend::SplitViewState& settings = m_settings_view.State();
    if (page_id == "settings" && settings.focusedPane == Frontend::SplitPane::Content)
      settings.RequestFocus(Frontend::SplitPane::Navigation);
    else
      ClosePage();
  }
  draw_notifications();
  ImGui::End();
}

void FrontendHost::DrawSettings()
{
  Frontend::SettingsViewModel model;
  model.interfaceSettings = m_interface;
  model.screenScraper = m_screen_scraper_model;
  model.screenScraper.artworkTitleCount = m_catalogue.size();
  model.screenScraper.missingListArtworkCount = static_cast<size_t>(
      std::ranges::count_if(m_catalogue, [this](const Frontend::LibraryItem& item) {
        return !Frontend::FindMedia(
            item, Frontend::ScreenScraperArtworkMediaKind(m_screen_scraper_model.listArtwork));
      }));
  model.companion = m_companion_model;
  model.communityLinkGroups = m_link_groups;
  // The storage locations are on the Paths page.
  model.showStorage = false;

  const std::vector<Frontend::SettingsExtensionPage> extensions =
      m_settings_pages.BuildSettingsPages();

  Frontend::SplitViewOptions options;
  options.contentControllerScroll = std::exchange(m_page_scroll, 0.0f);

  m_settings_view.Draw(
      model,
      {.screenScraper = m_screen_scraper.get(),
       .credentials = m_credentials.get(),
       .picker = m_picker.get()},
      {
          .persistInterface =
              [this](const Frontend::InterfaceSettingsModel& value) {
                m_interface = value;
                SaveInterfaceSettings();
              },
          .screenScraper =
              {
                  .makeRequest =
                      [this](Frontend::ScreenScraperCredentials account,
                             Frontend::ScreenScraperOptions options) {
                        return MakeScreenScraperRequest(std::move(account), options);
                      },
                  .persist =
                      [this](const Frontend::ScreenScraperPanelModel& value) {
                        m_screen_scraper_model = value;
                        SaveScreenScraperSettings();
                      },
                  .notify = [this](std::string_view message) { Notify(std::string(message)); },
                  .mediaDirectorySelected =
                      [this](Frontend::StorageLocationSelection selection) {
                        m_screen_scraper_model.mediaDirectory = std::move(selection.path);
                        SaveScreenScraperSettings();
                        Notify("ScreenScraper media location updated");
                      },
              },
          .persistCompanion =
              [this](const Frontend::CompanionPanelModel& value) {
                {
                  std::lock_guard lock(m_companion_mutex);
                  m_companion_model = value;
                }
                SaveCompanionSettings();
              },
          .openUri = [this](std::string_view uri) { OpenUri(uri); },
          .notify = [this](std::string_view message) { Notify(std::string(message)); },
      },
      extensions, ImGui::GetContentRegionAvail(), options);

  m_interface = model.interfaceSettings;
  m_screen_scraper_model.enabled = model.screenScraper.enabled;
  m_screen_scraper_model.options = model.screenScraper.options;
  m_screen_scraper_model.listArtwork = model.screenScraper.listArtwork;
  m_screen_scraper_model.detailArtwork = model.screenScraper.detailArtwork;
  m_screen_scraper_model.dynamicBackground = model.screenScraper.dynamicBackground;
  {
    std::lock_guard lock(m_companion_mutex);
    m_companion_model = model.companion;
  }
  m_link_groups = model.communityLinkGroups;
}

void FrontendHost::DrawLibrary()
{
  const auto presets = MakeGamesPresets();
  const std::vector<std::string> iso_paths = Config::GetIsoPaths();

  Frontend::StorageLocationSelectorModel selector;
  selector.id = "dolphin-library";
  selector.label = "Games directory";
  selector.currentPath =
      iso_paths.empty() ? std::filesystem::path() : StringToPath(iso_paths.front());
  selector.presets = presets;
  selector.promptLabel = "Select storage location";
  selector.currentPathLabel = "Folder";
  selector.showCurrentPath = false;
  selector.customPicker.futureAccessToken = L"dolphin-games-folder";

  DrawLibraryFilters();
  if (m_library_items_dirty)
    UpdateLibraryItems();

  Frontend::LibraryViewModel model;
  model.items = m_library_items;
  model.showAddContent = false;
  model.contextRequested = std::exchange(m_library_context_requested, false);
  model.controllerScroll = std::exchange(m_page_scroll, 0.0f);
  model.emptyLabel = m_library.IsScanning() ? "Scanning for games..." :
                     !m_catalogue.empty()   ? "No games match the search and filter." :
                                              "No GameCube or Wii games found.";
  model.gamesFolderSelector = selector;
  model.picker = m_picker.get();

  const size_t selected =
      std::min(m_library_view.SelectedIndex(),
               m_library_items.empty() ? size_t{0} : m_library_items.size() - 1);
  const size_t first = selected > LIBRARY_ARTWORK_WINDOW ? selected - LIBRARY_ARTWORK_WINDOW : 0;
  const size_t last = std::min(m_library_items.size(), selected + LIBRARY_ARTWORK_WINDOW + 1);
  std::unordered_set<Frontend::ItemId> visible;
  for (size_t i = first; i < last; ++i)
    visible.insert(m_library_items[i].id);

  size_t loads = 0;
  const auto artwork = [&](const Frontend::LibraryItem& item) -> Frontend::TextureHandle {
    if (!visible.contains(item.id))
      return {};
    if (const auto found = m_library_artwork.find(item.id); found != m_library_artwork.end())
      return found->second;
    if (loads >= LIBRARY_ARTWORK_LOADS_PER_FRAME)
      return {};
    ++loads;

    const Frontend::MediaAsset* media = Frontend::FindMedia(
        item, Frontend::ScreenScraperArtworkMediaKind(m_screen_scraper_model.listArtwork));
    if (!media)
      media = Frontend::FindMedia(item, Frontend::MediaKind::Cover2D);
    const Frontend::TextureHandle texture =
        media ? LoadTexture(media->path) : Frontend::TextureHandle{};
    m_library_artwork.emplace(item.id, texture);
    return texture;
  };

  (void)m_library_view.Draw(
      model,
      {
          .artwork = artwork,
          .launch = [this](const Frontend::LibraryItem& item) { (void)RequestLaunch(item.id); },
          .openContextMenu =
              [this](const Frontend::LibraryItem& item) { OpenContextMenu(item.id); },
          .refresh =
              [this] {
                m_library.Refresh();
                Notify("Scanning for games");
              },
          .addGamesFolder =
              {
                  .selected =
                      [this](Frontend::StorageLocationSelection selection) {
                        AddGamesDirectory(selection.path);
                      },
                  .failed = [this](std::string_view message) { Notify(std::string(message)); },
              },
      },
      ImGui::GetContentRegionAvail());

  for (auto it = m_library_artwork.begin(); it != m_library_artwork.end();)
  {
    if (visible.contains(it->first))
    {
      ++it;
      continue;
    }
    if (it->second)
      ReleaseTexture(it->second);
    it = m_library_artwork.erase(it);
  }
}

void FrontendHost::DrawLibraryFilters()
{
  constexpr std::array<const char*, 4> platforms{"All systems", "GameCube", "Wii", "Triforce"};
  constexpr std::array<const char*, 4> sorts{"Name", "System", "Region", "File size"};

  if (!ImGui::BeginTable("library-filters", 3, ImGuiTableFlags_SizingStretchProp))
    return;
  ImGui::TableSetupColumn("search", ImGuiTableColumnFlags_WidthStretch, 2.0f);
  ImGui::TableSetupColumn("platform", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("sort", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableNextRow();

  ImGui::TableNextColumn();
  if (Frontend::NativeInputText("Search", m_library_search.data(), m_library_search.size(), 0,
                                Frontend::TextInputScope::Text))
  {
    m_library_items_dirty = true;
  }

  const auto choice = [this](const char* label, const Config::Info<int>& info,
                             std::span<const char* const> labels) {
    std::vector<Frontend::ChoiceItem> items;
    for (size_t i = 0; i < labels.size(); ++i)
      items.push_back({std::to_string(i), labels[i]});
    size_t selected =
        static_cast<size_t>(std::clamp(Config::Get(info), 0, static_cast<int>(labels.size()) - 1));
    if (!Frontend::ShellCombo(label, &selected, items))
      return;
    Config::SetBase(info, static_cast<int>(selected));
    Config::Save();
    m_library_items_dirty = true;
  };
  ImGui::TableNextColumn();
  choice("Show", Config::UWP_LIBRARY_PLATFORM, platforms);
  ImGui::TableNextColumn();
  choice("Sort by", Config::UWP_LIBRARY_SORT, sorts);
  ImGui::EndTable();
}

void FrontendHost::UpdateLibraryItems()
{
  constexpr std::array<std::string_view, 4> platforms{"", "GameCube", "Wii", "Triforce"};
  m_library_items_dirty = false;

  const auto lower = [](std::string text) {
    Common::ToLower(&text);
    return text;
  };
  const std::string query = lower(m_library_search.data());
  const int platform = std::clamp(Config::Get(Config::UWP_LIBRARY_PLATFORM), 0,
                                  static_cast<int>(platforms.size()) - 1);
  std::vector<std::pair<const Frontend::LibraryItem*, u64>> items;
  for (const Frontend::LibraryItem& item : m_catalogue)
  {
    if (platform != 0 && item.platform != platforms[platform])
      continue;
    const auto game = m_library.FindGame(item.id);
    if (!query.empty() && !lower(item.name).contains(query) &&
        !(game && lower(game->GetGameID()).contains(query)))
    {
      continue;
    }
    items.emplace_back(&item, game ? game->GetFileSize() : 0);
  }

  const auto by_name = [&lower](const auto& entry) { return lower(entry.first->name); };
  switch (Config::Get(Config::UWP_LIBRARY_SORT))
  {
  case 1:
    std::ranges::stable_sort(items, {}, [&by_name](const auto& entry) {
      return std::pair(entry.first->platform, by_name(entry));
    });
    break;
  case 2:
    std::ranges::stable_sort(items, {}, [&by_name](const auto& entry) {
      return std::pair(entry.first->region, by_name(entry));
    });
    break;
  case 3:
    std::ranges::stable_sort(items, std::greater<>{},
                             [](const auto& entry) { return entry.second; });
    break;
  default:
    std::ranges::stable_sort(items, {}, by_name);
    break;
  }

  m_library_items.clear();
  m_library_items.reserve(items.size());
  for (const auto& [item, size] : items)
    m_library_items.push_back(*item);
}

void FrontendHost::ReleaseLibraryArtwork()
{
  for (const auto& [id, texture] : m_library_artwork)
  {
    if (texture)
      ReleaseTexture(texture);
  }
  m_library_artwork.clear();
}

void FrontendHost::DrawGraphics()
{
  Frontend::GraphicsViewModel model = m_settings_pages.BuildGraphicsModel();
  model.contentControllerScroll = std::exchange(m_page_scroll, 0.0f);
  const Frontend::GraphicsViewResult result =
      m_graphics_view.Draw(model, ImGui::GetContentRegionAvail());
  if (result.closeRequested)
    ClosePage();
}

void FrontendHost::RunAction(std::string_view action)
{
  if (action == "hotkeys")
    OpenHotkeyMapping();
  else if (action == "freelook-controls")
    OpenFreeLookMapping();
  else if (action == "memory-cards")
    OpenTools("memory-card-a");
  else if (action == "wii-system")
    OpenTools("system");
  else if (action == "path-reset")
    ResetPaths();
  else if (action == "sd-pack" || action == "sd-unpack")
    SyncSDCard(action == "sd-pack");
  else if (action == "gba-bios")
    ImportGBAFile(std::nullopt);
  else if (action == "gba-roms-clear")
    ClearGBARoms();
  else if (action.starts_with("gba-rom-") && action.size() == 9 && action[8] >= '0' &&
           static_cast<size_t>(action[8] - '0') < Config::MAIN_GBA_ROM_PATHS.size())
    ImportGBAFile(static_cast<size_t>(action[8] - '0'));
  else if (action == "agp-rom-a" || action == "agp-rom-b")
    ImportAGPCartridge(action == "agp-rom-a");
  else if (action == "default-iso")
    ChooseDefaultDisc();
  else if (action == "library-rebuild")
    m_library.Rebuild();
  else if (action == "settings-reset")
    ResetSettings();
}

void FrontendHost::CopyIntoUserFolder(const std::string& source, const std::string& destination,
                                      std::string title, std::function<void()> copied)
{
  m_task.Start(std::move(title), false,
               [this, source, destination, copied = std::move(copied)](
                   BackgroundTask::Context&) -> BackgroundTask::Completion {
                 const bool success = File::CreateFullPath(destination) &&
                                      (source == destination || File::Copy(source, destination));
                 return [success, copied] {
                   if (success)
                     copied();
                   else
                     PromptService::GetInstance().Inform("Game Boy Advance",
                                                         "The file could not be copied.",
                                                         Common::MsgType::Warning);
                 };
               });
}

void FrontendHost::ImportGBAFile(std::optional<size_t> port)
{
  std::vector<std::wstring> types{L".bin"};
  if (port)
    types = {L".gba", L".agb", L".gb", L".gbc", L".mb", L".zip", L".7z"};
  m_services.PickFile(
      std::move(types), port ? L"dolphin-gba-rom" : L"dolphin-gba-bios",
      [this, port](std::string path) {
        if (path.empty())
          return;
        const std::string destination = port ? File::GetUserPath(D_GBAUSER_IDX) + "ROMs/" +
                                                   PathToString(StringToPath(path).filename()) :
                                               File::GetUserPath(F_GBABIOS_IDX);
        CopyIntoUserFolder(
            path, destination, port ? "Importing the GBA ROM" : "Importing the GBA BIOS",
            [this, destination, port] {
              if (port)
              {
                Config::SetBaseOrCurrent(Config::MAIN_GBA_ROM_PATHS[*port], destination);
                Config::Save();
              }
              Notify(port ? "GBA ROM imported" : "GBA BIOS imported");
            });
      });
}

void FrontendHost::ImportAGPCartridge(bool slot_a)
{
  m_services.PickFile(
      std::vector<std::wstring>{L".gba", L".agb"}, L"dolphin-agp-cartridge",
      [this, slot_a](std::string path) {
        if (path.empty())
          return;
        const std::string destination = File::GetUserPath(D_GBAUSER_IDX) + "ROMs/" +
                                        PathToString(StringToPath(path).filename());
        CopyIntoUserFolder(
            path, destination, "Importing the GBA cartridge", [this, destination, slot_a] {
              m_settings_pages.Flush();
              Config::SetBaseOrCurrent(
                  Config::GetInfoForAGPCartPath(slot_a ? ExpansionInterface::Slot::A :
                                                         ExpansionInterface::Slot::B),
                  destination);
              Config::Save();
              Notify("GBA cartridge inserted");
            });
      });
}

void FrontendHost::ClearGBARoms()
{
  PromptService::GetInstance().Confirm(
      "Game Boy Advance",
      "Remove the GBA ROMs? Their copies in the user folder are deleted. Saves are kept.",
      [this](bool confirmed) {
        if (!confirmed)
          return;
        const std::string roms = File::GetUserPath(D_GBAUSER_IDX) + "ROMs/";
        for (const auto& info : Config::MAIN_GBA_ROM_PATHS)
        {
          const std::string path = Config::Get(info);
          if (path.starts_with(roms))
            File::Delete(path);
          Config::SetBaseOrCurrent(info, std::string());
        }
        Config::Save();
        Notify("GBA ROMs removed");
      });
}

void FrontendHost::ChooseDefaultDisc()
{
  m_services.PickFile(GetBootableFileTypes(), L"dolphin-default-iso", [this](std::string path) {
    if (path.empty())
      return;
    m_settings_pages.Flush();
    Config::SetBase(Config::MAIN_DEFAULT_ISO, path);
    Config::Save();
    Notify("Default disc set");
  });
}

void FrontendHost::ResetSettings()
{
  PromptService::GetInstance().Confirm(
      "Reset all settings",
      "Restore every Dolphin setting to its default? This can't be undone. Games, saves and "
      "controller profiles are kept.",
      [this](bool confirmed) {
        if (!confirmed)
          return;
        m_settings_pages.Flush();
        SConfig::ResetAllSettings();
        UICommon::SetUserDirectory(File::GetUserPath(D_USER_IDX));
        if (Common::UWP::IsXboxConsole())
        {
          Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("D3D12"));
          Config::SetIsoPaths(GameLibrary::GetSystemFolders(GameLibrary::XBOX_GAMES_ROOT));
        }
        Config::Save();
        m_library.Refresh();
        Notify("Settings reset");
      });
}

void FrontendHost::ResetPaths()
{
  PromptService::GetInstance().Confirm(
      "Default locations",
      "Use the default locations for Dolphin's folders, such as custom textures, the Wii NAND, "
      "memory cards, controller profiles, per-game settings and the cache? Files are not moved.",
      [this](bool confirmed) {
        if (!confirmed)
          return;
        m_settings_pages.Flush();
        File::SetUserPath(D_USER_IDX, File::GetUserPath(D_USER_IDX));
        {
          Config::ConfigChangeCallbackGuard guard;
          Config::SetBase(Config::MAIN_LOAD_PATH, std::string());
          Config::SetBase(Config::MAIN_RESOURCEPACK_PATH, std::string());
          Config::SetBase(Config::MAIN_FS_PATH, std::string());
          Config::SetBase(Config::MAIN_WII_SD_CARD_IMAGE_PATH, std::string());
          Config::SetBase(Config::MAIN_WII_SD_CARD_SYNC_FOLDER_PATH, std::string());
          Config::SetBase(Config::MAIN_DUMP_PATH, std::string());
          Config::SetBase(Config::MAIN_WFS_PATH, std::string());
          Config::SetBase(Config::MAIN_GBA_SAVES_PATH, std::string());
          for (const StorageFolder& folder : STORAGE_FOLDERS)
          {
            if (folder.applied_by_app)
              Config::SetBase(*folder.info, std::string());
          }
        }
        ApplyDefaultStorage();
        Config::OnConfigChanged();
        Config::Save();
        ResourcePack::Init();
        Notify("Default locations restored");
      });
}

void FrontendHost::SyncSDCard(bool pack)
{
  const std::string folder = File::GetUserPath(D_WIISDCARDSYNCFOLDER_IDX);
  const std::string image = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  const std::string title = pack ? "Pack the SD card" : "Unpack the SD card";
  PromptService::GetInstance().Confirm(
      title,
      pack ? fmt::format("Pack the contents of {} into {}? Everything currently in the SD card "
                         "image is deleted.",
                         folder, image) :
             fmt::format("Unpack the contents of {} into {}? Everything currently in the folder is "
                         "deleted.",
                         image, folder),
      [this, pack, title](bool confirmed) {
        if (!confirmed)
          return;
        m_task.Start(
            pack ? "Packing the SD card" : "Unpacking the SD card", true,
            [this, pack, title](BackgroundTask::Context& context) -> BackgroundTask::Completion {
              const auto cancelled = [&context] { return context.IsCancelled(); };
              const bool success = pack ? Common::SyncSDFolderToSDImage(cancelled, false) :
                                          Common::SyncSDImageToSDFolder(cancelled);
              return [this, success, pack, title] {
                if (success)
                  Notify(pack ? "SD card packed" : "SD card unpacked");
                else
                  PromptService::GetInstance().Inform(title, "The conversion failed.",
                                                      Common::MsgType::Warning);
              };
            });
      });
}

void FrontendHost::OpenTools(std::string_view tab)
{
  m_settings_pages.Flush();
  m_active_page = "tools";
  m_tools.Open(tab);
}

void FrontendHost::OpenHotkeyMapping()
{
  const auto groups = [](std::initializer_list<HotkeyGroup> ids) {
    std::vector<ControllerEmu::ControlGroup*> result;
    for (const HotkeyGroup id : ids)
      result.push_back(HotkeyManagerEmu::GetHotkeyGroup(id));
    return result;
  };

  // Bluetooth passthrough is not available to apps. Xbox apps always fill the screen and never
  // lock the cursor.
  std::vector<std::string> hidden{"Press Sync Button"};
  if (Common::UWP::IsXboxConsole())
  {
    hidden.emplace_back("Toggle Fullscreen");
    hidden.emplace_back("Unlock Cursor");
  }

  HotkeyManagerEmu::LoadConfig();
  m_mapping_page.Open(
      HotkeyManagerEmu::GetConfig(),
      {
          {"general", "General", groups({HKGP_GENERAL, HKGP_VOLUME, HKGP_SPEED})},
          {"tas", "TAS Tools", groups({HKGP_FRAME_ADVANCE, HKGP_MOVIE})},
          {"debugging", "Debugging", groups({HKGP_STEPPING, HKGP_PC, HKGP_BREAKPOINT})},
          {"wii", "Wii and Wii Remote", groups({HKGP_WII})},
          {"profiles", "Controller Profile",
           groups({HKGP_CONTROLLER_PROFILE_1, HKGP_CONTROLLER_PROFILE_2, HKGP_CONTROLLER_PROFILE_3,
                   HKGP_CONTROLLER_PROFILE_4})},
          {"graphics", "Graphics", groups({HKGP_GRAPHICS_TOGGLES, HKGP_FREELOOK, HKGP_IR})},
          {"usb", "USB Emulation", groups({HKGP_USB_EMU})},
          {"3d", "3D", groups({HKGP_3D_TOGGLE, HKGP_3D_DEPTH})},
#ifdef HAS_LIBMGBA
          {"gba", "Game Boy Advance",
           groups({HKGP_GBA_CONTROLS, HKGP_GBA_CORE, HKGP_GBA_VOLUME, HKGP_GBA_SIZE})},
#endif
          {"states", "Save and Load State", groups({HKGP_SAVE_STATE, HKGP_LOAD_STATE})},
          {"state-management", "Other State Management",
           groups({HKGP_SELECT_STATE, HKGP_LOAD_LAST_STATE, HKGP_STATE_MISC})},
      },
      std::move(hidden));
  m_active_page = "hotkeys";
}

void FrontendHost::OpenFreeLookMapping()
{
  const auto group = [](FreeLookGroup id) { return FreeLook::GetInputGroup(0, id); };

  FreeLook::LoadInputConfig();
  m_mapping_page.Open(FreeLook::GetInputConfig(),
                      {
                          {"general",
                           "General",
                           {group(FreeLookGroup::Move), group(FreeLookGroup::Speed),
                            group(FreeLookGroup::FieldOfView), group(FreeLookGroup::Other)}},
                          {"rotation", "Rotation", {group(FreeLookGroup::Rotation)}},
                      });
  m_active_page = "freelook-controls";
}

void FrontendHost::DrawMappingPage()
{
  if (m_mapping_page.Draw(m_page_scroll, std::exchange(m_context_pressed, false)))
  {
    m_mapping_page.Close();
    m_active_page = "settings";
    m_settings_view.State().RequestFocus(Frontend::SplitPane::Navigation);
  }
}

void FrontendHost::AddGamesDirectory(const std::filesystem::path& path)
{
  const std::vector<std::string> directories =
      Frontend::FindStorageLocationPreset(path, MakeGamesPresets()) ?
          GameLibrary::GetSystemFolders(path.parent_path()) :
          std::vector<std::string>{PathToString(path)};
  if (directories.front().empty())
    return;

  std::vector<std::string> paths = Config::GetIsoPaths();
  bool changed = false;
  for (auto directory = directories.rbegin(); directory != directories.rend(); ++directory)
  {
    if (std::ranges::find(paths, *directory) != paths.end())
      continue;
    paths.insert(paths.begin(), *directory);
    changed = true;
  }
  if (changed)
  {
    Config::SetIsoPaths(paths);
    Config::Save();
  }
  m_library.Refresh();
  Notify(directories.size() > 1 ? "Games directories added" : "Games directory added");
}

void FrontendHost::DrawStorageLocation(std::string_view id)
{
  if (!m_picker)
    return;

  if (id == "games")
  {
    const auto presets = MakeGamesPresets();
    const std::vector<std::string> iso_paths = Config::GetIsoPaths();
    Frontend::StorageLocationSelectorModel games;
    games.id = "dolphin-games";
    games.label = "Games directory";
    games.currentPath =
        iso_paths.empty() ? std::filesystem::path() : StringToPath(iso_paths.front());
    games.presets = presets;
    games.promptLabel = "Select storage location";
    games.showCurrentPath = false;
    games.customPicker.futureAccessToken = L"dolphin-games-folder";
    Frontend::SectionTitle("Games directory");
    (void)Frontend::DrawStorageLocationSelector(
        games, *m_picker,
        {.selected =
             [this](Frontend::StorageLocationSelection selection) {
               AddGamesDirectory(selection.path);
             },
         .failed = [this](std::string_view message) { Notify(std::string(message)); }});
    DrawGameFolders();
    return;
  }

  const auto folder = std::ranges::find(STORAGE_FOLDERS, id, &StorageFolder::id);
  if (folder == STORAGE_FOLDERS.end())
    return;
  const auto presets = Frontend::MakeXboxStorageLocationPresets(
      GetStoragePath(XBOX_DATA_ROOT, *folder), GetStoragePath(USB_DATA_ROOT, *folder));
  Frontend::StorageLocationSelectorModel model;
  model.id = folder->id;
  model.label = folder->label;
  model.currentPath = StringToPath(File::GetUserPath(folder->user_path));
  model.presets = presets;
  model.customPicker.futureAccessToken = folder->token;
  (void)Frontend::DrawStorageLocationSelector(
      model, *m_picker,
      {.selected =
           [this, entry = *folder](Frontend::StorageLocationSelection selection) {
             // A file in a custom folder keeps the name it has in the app's storage.
             if (entry.file && selection.custom)
               selection.path /= StringToPath(entry.folder).filename();
             SetStorageFolder(*entry.info, PathToString(selection.path), entry.file);
           },
       .failed = [this](std::string_view message) { Notify(std::string(message)); }});
}

void FrontendHost::ApplyStoragePaths()
{
  for (const StorageFolder& folder : STORAGE_FOLDERS)
  {
    if (!folder.applied_by_app)
      continue;
    const std::string path = UserPathForm(Config::Get(*folder.info));
    if (path.empty() || path == File::GetUserPath(folder.user_path))
      continue;
    File::SetUserPath(folder.user_path, path);
    File::CreateFullPath(path);
  }
}

bool FrontendHost::ApplyDefaultStorage()
{
  // On a PC, the folders stay in the app's storage.
  if (!Common::UWP::IsXboxConsole())
    return false;

  bool changed = false;
  for (const StorageFolder& folder : STORAGE_FOLDERS)
  {
    if (!Config::Get(*folder.info).empty())
      continue;
    const std::string path = PathToString(GetStoragePath(XBOX_DATA_ROOT, folder));
    if (!CreateStorageLocation(path, folder.file))
      continue;
    Config::SetBase(*folder.info, path);
    changed = true;
  }
  if (changed)
    Config::Save();
  return changed;
}

UwpImGuiFrontend::InputProfileLocation FrontendHost::GetStorageChoices(std::string_view id) const
{
  Frontend::InputProfileLocation location;
  const auto folder = std::ranges::find(STORAGE_FOLDERS, id, &StorageFolder::id);
  if (folder == STORAGE_FOLDERS.end())
    return location;
  const auto presets = Frontend::MakeXboxStorageLocationPresets(
      GetStoragePath(XBOX_DATA_ROOT, *folder), GetStoragePath(USB_DATA_ROOT, *folder));
  for (const Frontend::StorageLocationPreset& preset : presets)
    location.choices.push_back({preset.id, preset.label});
  location.choices.push_back({"custom-folder", "Custom folder"});
  const std::string& path = File::GetUserPath(folder->user_path);
  const auto preset = Frontend::FindStorageLocationPreset(StringToPath(path), presets);
  location.current = preset ? presets[*preset].id : "custom-folder";
  location.revision = std::hash<std::string>{}(path);
  return location;
}

UwpImGuiFrontend::MappingSaveResult FrontendHost::SelectStorage(std::string_view id,
                                                                std::string_view choice)
{
  const auto folder = std::ranges::find(STORAGE_FOLDERS, id, &StorageFolder::id);
  if (folder == STORAGE_FOLDERS.end() || !m_picker)
    return {false, "This location can't be changed"};

  if (choice == "custom-folder")
  {
    Frontend::PickerRequest request;
    request.futureAccessToken = folder->token;
    Frontend::PickerCallbacks callbacks;
    callbacks.completed = [this, entry = *folder](std::unique_ptr<Frontend::PickedItem> item) {
      if (item)
        SetStorageFolder(*entry.info, PathToString(item->path), entry.file);
    };
    callbacks.failed = [this](std::string_view message) { Notify(std::string(message)); };
    if (!m_picker->PickFolder(std::move(request), std::move(callbacks)))
      return {false, "A file or folder picker is already open"};
    return {true, {}};
  }

  const auto presets = Frontend::MakeXboxStorageLocationPresets(
      GetStoragePath(XBOX_DATA_ROOT, *folder), GetStoragePath(USB_DATA_ROOT, *folder));
  const auto preset = std::ranges::find(presets, choice, &Frontend::StorageLocationPreset::id);
  if (preset == presets.end())
    return {false, "Unknown location"};
  if (!SetStorageFolder(*folder->info, PathToString(preset->path), folder->file))
    return {false, "The folder can't be created. Check that the drive is connected."};
  return {true, {}};
}

bool FrontendHost::SetStorageFolder(const Config::Info<std::string>& info, const std::string& path,
                                    bool file)
{
  if (!CreateStorageLocation(path, file))
  {
    Notify(fmt::format("{} can't be created. Check that the drive is connected.", path));
    return false;
  }

  m_settings_pages.Flush();
  Config::SetBase(info, path);
  Config::Save();
  if (&info == &Config::MAIN_RESOURCEPACK_PATH)
    ResourcePack::Init();
  Notify("Location updated");
  return true;
}

void FrontendHost::DrawGameFolders()
{
  Frontend::SectionTitle("Game folders");
  const std::vector<std::string> paths = Config::GetIsoPaths();
  if (paths.empty())
  {
    ShellWidgets::HintText("No game folders have been added.");
    return;
  }

  for (size_t index = 0; index < paths.size(); ++index)
  {
    ImGui::PushID(static_cast<int>(index));
    if (ShellWidgets::ValueRow("game-folder", paths[index], "Remove"))
    {
      PromptService::GetInstance().Confirm(
          "Remove game folder",
          fmt::format("Stop looking for games in {}? The files are not deleted.", paths[index]),
          [this, path = paths[index]](bool confirmed) {
            if (!confirmed)
              return;
            std::vector<std::string> remaining = Config::GetIsoPaths();
            std::erase(remaining, path);
            Config::SetIsoPaths(remaining);
            Config::Save();
            m_library.Refresh();
          });
    }
    ImGui::PopID();
  }
}

bool FrontendHost::HandleOverlayInput(const Frontend::FrameInput& input,
                                      const Frontend::FrameInput& previous)
{
  if (!OwnsOverlayInput())
  {
    m_page_scroll = 0.0f;
    return false;
  }
  m_context_pressed = input.context && !previous.context;

  const bool editor_open = m_metadata_editor && m_metadata_editor->IsOpen();
  m_context_detail_scroll =
      m_context_item && !editor_open ? -input.rightStickY * 520.0f * input.deltaSeconds : 0.0f;
  m_page_scroll = -input.rightStickY * PAGE_SCROLL_SPEED * input.deltaSeconds;

  if (m_mapper_scope != MapperScope::None)
  {
    const bool up = input.up || input.leftStickY >= 0.55f;
    const bool down = input.down || input.leftStickY <= -0.55f;
    const bool left = input.left || input.leftStickX <= -0.55f;
    const bool right = input.right || input.leftStickX >= 0.55f;
    m_mapper_frame_input = {
        .deltaSeconds = input.deltaSeconds,
        .up = m_mapper_repeat[0].Update(up, input.deltaSeconds),
        .down = m_mapper_repeat[1].Update(down, input.deltaSeconds),
        .left = m_mapper_repeat[2].Update(left, input.deltaSeconds),
        .right = m_mapper_repeat[3].Update(right, input.deltaSeconds),
        .accept = input.accept && !previous.accept,
        .back = input.back && !previous.back,
        .context = input.context && !previous.context,
        .alternate = input.alternate && !previous.alternate,
        .menu = input.menu && !previous.menu,
        .view = input.view && !previous.view,
        .leftShoulder = input.leftShoulder && !previous.leftShoulder,
        .rightShoulder = input.rightShoulder && !previous.rightShoulder,
        .controllerInputHeld = input.controllerInputHeld,
        .leftStickX = input.leftStickX,
        .leftStickY = input.leftStickY,
        .rightStickX = input.rightStickX,
        .rightStickY = input.rightStickY,
        .pointer = input.pointer,
        .pointerPressed = input.pointerPressed,
    };
    return true;
  }
  for (DirectionRepeat& repeat : m_mapper_repeat)
    repeat.Reset();

  if (m_context_item && !editor_open && m_title_page == TitlePage::None && input.back &&
      !previous.back)
  {
    (void)Frontend::ConsumeWidgetBackPressed();
    CloseContextMenu();
    return true;
  }

  if (m_active_page == "library" && !m_context_item && input.context && !previous.context)
  {
    m_library_context_requested = true;
    return true;
  }

  if (editor_open)
  {
    const Frontend::MetadataEditorEvent event = m_metadata_editor->HandleInput({
        .accept = input.accept && !previous.accept,
        .back = input.back && !previous.back,
        .up = input.up && !previous.up,
        .down = input.down && !previous.down,
        .left = input.left && !previous.left,
        .right = input.right && !previous.right,
        .leftShoulder = input.leftShoulder && !previous.leftShoulder,
        .rightShoulder = input.rightShoulder && !previous.rightShoulder,
        .rightStickY = input.rightStickY,
        .deltaSeconds = input.deltaSeconds,
    });
    if (event != Frontend::MetadataEditorEvent::None)
      (void)Frontend::ConsumeWidgetBackPressed();
  }
  return true;
}

void FrontendHost::DrawOverlayPages()
{
  if (m_active_page.empty() && !m_context_item)
    return;
  DrawSettingsPage(m_context_item ? "context" : m_active_page);
}

void FrontendHost::DrawContextMenu(const Frontend::ShellLayoutContext& context, ImDrawList* draw,
                                   const Frontend::UnitRect& panel)
{
  if (!m_context_item)
    return;

  const Frontend::LibraryItem* const item = FindCatalogueItem(*m_context_item);
  if (!item)
  {
    CloseContextMenu();
    return;
  }

  const auto game = m_library.FindGame(item->id);
  const std::string title_id = game ? game->GetGameID() : fmt::format("{:016X}", item->contentId);
  ImFont* const font = Frontend::ResolveFrontendTextFont(m_text_font);

  if (m_metadata_editor && m_metadata_editor->IsOpen())
  {
    Frontend::DrawOverlayHeader(draw, context, panel, Frontend::CurrentWidgetTheme(),
                                m_metadata_editor->Heading(),
                                Frontend::NormalizeDisplayText(item->name), title_id, font);
    (void)m_metadata_editor->Draw(context, draw, panel, Frontend::CurrentWidgetTheme(), font);
    return;
  }

  Frontend::TitleOptionsModel model;
  model.artwork = m_context_artwork;
  model.title = Frontend::NormalizeDisplayText(item->name);
  model.titleId = title_id;
  model.subtitle = item->platform;
  model.metadata = item->metadata;
  model.information = {
      {"Platform", item->platform},
      {"Region", item->region},
      {"Format", item->format},
      {"Revision", std::to_string(item->version)},
  };
  if (game)
  {
    model.information.push_back(
        {"Size", fmt::format("{:.2f} GiB", static_cast<double>(game->GetFileSize()) /
                                               (1024.0 * 1024.0 * 1024.0))});
    model.information.push_back({"Path", game->GetFilePath()});
    if (Config::Get(Config::MAIN_TIME_TRACKING) && !game->GetGameID().empty())
    {
      const auto played = std::chrono::duration_cast<std::chrono::minutes>(
          TimePlayed().GetTimePlayed(game->GetGameID()));
      model.information.push_back(
          {"Time played", fmt::format("{}h {:02}m", played.count() / 60, played.count() % 60)});
    }
  }
  model.detailControllerScroll = std::exchange(m_context_detail_scroll, 0.0f);
  model.actions = Frontend::MakeDefaultTitleOptionActions(m_favorites.contains(item->id));
  if (game)
  {
    const DiscIO::Platform platform = game->GetPlatform();
    const bool wii = platform == DiscIO::Platform::WiiDisc || platform == DiscIO::Platform::WiiWAD;
    model.actions.push_back(
        {"start-recording", "Start with input recording", {}, true, false, false, true});
    if (platform == DiscIO::Platform::WiiDisc)
      model.actions.push_back({"riivolution", "Start with Riivolution patches"});
    if (!game->GetGameID().empty())
      model.actions.push_back({"game-settings", "Game settings", {}, true, false, false, true});
    model.actions.push_back({"cheats", "Cheats and patches"});
    model.actions.push_back({"graphics-mods", "Graphics mods"});
    if (DiscIO::IsDisc(platform))
      model.actions.push_back({"disc-tools", "Verify and convert"});
    if (wii)
      model.actions.push_back({"export-wii-save", "Export Wii save", {}, true, false, false, true});
    if (platform == DiscIO::Platform::WiiDisc)
      model.actions.push_back({"disc-update", "Update the Wii system from this disc"});
    if (platform == DiscIO::Platform::WiiWAD)
    {
      model.actions.push_back({"install-wad", "Install to the NAND"});
      model.actions.push_back({"uninstall-wad", "Uninstall from the NAND",
                               m_context_title_installed ? "" : "Not installed",
                               m_context_title_installed});
    }
  }

  const Frontend::TitleOptionsResult result =
      Frontend::DrawTitleOptionsView(context, draw, panel, m_title_options_state, model, font);
  if (result.activatedAction)
    ActivateTitleOption(*result.activatedAction);
  if (result.closeRequested)
    CloseContextMenu();
}

void FrontendHost::DrawInputMapper(const Frontend::ShellLayoutContext& context, ImDrawList* draw)
{
  if (!m_input_mapper.IsOpen())
    OpenInputMapper(m_mapper_scope == MapperScope::Title ? m_context_item : std::nullopt);

  const Frontend::InputMapperResult result = m_input_mapper.Draw(
      context, draw, m_mapper_frame_input, Frontend::ResolveFrontendTextFont(m_text_font));
  m_mapper_frame_input = {};
  if (!result.error.empty())
    Notify(result.error);
  if (result.saved)
  {
    Notify(m_mapper_scope == MapperScope::Title ? "Game controller profile saved" :
                                                  "Controller settings saved");
  }
  if (result.saved || result.cancelled || !m_input_mapper.IsOpen())
  {
    const MapperScope scope = std::exchange(m_mapper_scope, MapperScope::None);
    m_input_mapper.Close();
    for (DirectionRepeat& repeat : m_mapper_repeat)
      repeat.Reset();
    if (scope == MapperScope::Global)
      m_active_page.clear();
  }
}

void FrontendHost::ActivateTitleOption(std::string_view action)
{
  if (!m_context_item)
    return;

  if (action == "start")
  {
    const Frontend::ItemId item = *m_context_item;
    CloseContextMenu();
    (void)RequestLaunch(item);
  }
  else if (action == "favorite")
  {
    {
      std::lock_guard lock(m_companion_mutex);
      if (!m_favorites.erase(*m_context_item))
        m_favorites.insert(*m_context_item);
    }
    Config::SetBase(Config::UWP_FAVORITES, SerializeFavorites(m_favorites));
    Config::Save();
    m_frontend->RefreshCatalogue();
  }
  else if (action == "metadata")
  {
    OpenMetadataEditor();
  }
  else if (action == "controller-mapping")
  {
    OpenInputMapper(m_context_item);
  }
  else
  {
    const Frontend::ItemId item = *m_context_item;
    const auto game = m_library.FindGame(item);
    const Frontend::LibraryItem* const entry = FindCatalogueItem(item);
    if (!game || !entry)
      return;

    const auto refresh = [this, item] {
      const auto refreshed = m_library.FindGame(item);
      m_context_title_installed = refreshed && WiiUtils::IsTitleInstalled(refreshed->GetTitleID());
    };
    if (action == "start-recording")
    {
      if (!Recordings::BeginMovieRecording())
      {
        Notify("Input recording could not be started");
        return;
      }
      QueueBoot({BootParameters::GenerateFromFile(game->GetFilePath()), entry->name, item});
      CloseContextMenu();
    }
    else if (action == "riivolution")
    {
      m_riivolution_page.Open(*game, item, entry->name);
      m_title_page = TitlePage::Riivolution;
    }
    else if (action == "cheats")
    {
      m_cheats_page.Open(*game);
      m_title_page = TitlePage::Cheats;
    }
    else if (action == "graphics-mods")
    {
      m_mods_page.Open(*game);
      m_title_page = TitlePage::GraphicsMods;
    }
    else if (action == "game-settings")
    {
      m_game_settings_page.Open(*game);
      m_title_page = TitlePage::GameSettings;
    }
    else if (action == "disc-tools")
    {
      m_disc_tools_page.Open(game, entry->name);
      m_title_page = TitlePage::DiscTools;
    }
    else if (action == "export-wii-save")
    {
      m_services.PickFolder(L"dolphin-wii-save-export",
                            [this, title_id = game->GetTitleID()](std::string directory) {
                              if (!directory.empty())
                                WiiSystemTools::ExportSave(m_services, title_id, directory);
                            });
    }
    else if (action == "disc-update")
    {
      WiiSystemTools::UpdateFromDisc(m_services, game->GetFilePath(), {});
    }
    else if (action == "install-wad")
    {
      WiiSystemTools::InstallWad(m_services, game->GetFilePath(), refresh);
    }
    else if (action == "uninstall-wad")
    {
      WiiSystemTools::UninstallTitle(m_services, game->GetTitleID(), entry->name, refresh);
    }
  }
}

void FrontendHost::DrawTitlePage()
{
  const float scroll = std::exchange(m_page_scroll, 0.0f);
  bool close = false;
  switch (m_title_page)
  {
  case TitlePage::Cheats:
    close = m_cheats_page.Draw(scroll, std::exchange(m_context_pressed, false));
    break;
  case TitlePage::GraphicsMods:
    close = m_mods_page.Draw(scroll);
    break;
  case TitlePage::Riivolution:
    close = m_riivolution_page.Draw(scroll);
    break;
  case TitlePage::GameSettings:
    close = m_game_settings_page.Draw(scroll);
    break;
  case TitlePage::DiscTools:
    close = m_disc_tools_page.Draw(scroll);
    break;
  default:
    break;
  }
  if (close)
    CloseTitlePage();
}

void FrontendHost::CloseTitlePage()
{
  switch (m_title_page)
  {
  case TitlePage::Cheats:
    m_cheats_page.Close();
    break;
  case TitlePage::GraphicsMods:
    m_mods_page.Close();
    break;
  case TitlePage::Riivolution:
    m_riivolution_page.Close();
    break;
  case TitlePage::GameSettings:
    m_game_settings_page.Close();
    break;
  case TitlePage::DiscTools:
    m_disc_tools_page.Close();
    break;
  default:
    break;
  }
  m_title_page = TitlePage::None;
  m_title_options_state.actions.requestFocus = true;
}

void FrontendHost::CloseContextMenu()
{
  CloseTitlePage();
  if (m_metadata_editor && m_metadata_editor->IsOpen())
    m_metadata_editor->Close();
  if (m_context_artwork)
  {
    ReleaseTexture(m_context_artwork);
    m_context_artwork = {};
  }
  if (m_mapper_scope == MapperScope::Title)
  {
    m_input_mapper.Close();
    m_mapper_scope = MapperScope::None;
  }
  m_context_item.reset();
  m_title_options_state = {};
}

void FrontendHost::OpenInputMapper(std::optional<Frontend::ItemId> item)
{
  m_input_mapper.Close();
  m_mapper_scope = item ? MapperScope::Title : MapperScope::Global;
  m_mapper_frame_input = {};
  for (DirectionRepeat& repeat : m_mapper_repeat)
    repeat.Reset();
  m_input_mapper.Open(m_mapping_host, item ? std::optional<std::uint64_t>(*item) : std::nullopt, 0);
}

void FrontendHost::OpenMetadataEditor()
{
  if (!m_context_item || !m_metadata_editor)
    return;

  const Frontend::LibraryItem* const item = FindCatalogueItem(*m_context_item);
  if (!item)
    return;

  const auto game = m_library.FindGame(item->id);
  const std::filesystem::path record_path =
      StateRoot() / "metadata" / fmt::format("{:016x}.json", item->id);
  Frontend::GameMetadataRecord record{
      .itemId = item->id, .title = item->name, .metadata = item->metadata};
  if (const auto saved = Frontend::LoadGameMetadataRecord(record_path))
    record = *saved;

  Frontend::ScreenScraperSearchRequest search;
  search.developer = GetDeveloperIdentity();
  if (m_credentials)
    search.account = m_credentials->Load();
  search.preferredRegion = m_screen_scraper_model.options.preferredRegion;
  search.systemId = game ? GameLibrary::GetScreenScraperSystem(*game) :
                           GameLibrary::SCREENSCRAPER_GAMECUBE_SYSTEM_ID;
  search.platformName = item->platform;
  search.regionHint = item->region;
  search.query = item->name;
  search.previewDirectory = StateRoot() / "metadata" / ".search";

  const Frontend::MediaAsset* const list_art =
      Frontend::FindMedia(*item, Frontend::MediaKind::Cover2D);
  const Frontend::MediaAsset* const detail_art = Frontend::FindMedia(
      *item, Frontend::ScreenScraperArtworkMediaKind(m_screen_scraper_model.detailArtwork));

  m_metadata_editor->Open(
      {
          .record = std::move(record),
          .defaultTitle = item->name,
          .listArtwork = list_art ? list_art->path : std::filesystem::path(),
          .detailArtwork = detail_art ? detail_art->path : std::filesystem::path(),
      },
      {
          .listArtwork = m_screen_scraper_model.listArtwork,
          .detailArtwork = m_screen_scraper_model.detailArtwork,
          .searchRequest = std::move(search),
      },
      {
          .loadTexture = [this](const std::filesystem::path& path) { return LoadTexture(path); },
          .save =
              [this, record_path](const Frontend::GameMetadataRecord& saved, bool) {
                const bool ok = Frontend::SaveGameMetadataRecordAtomic(record_path, saved);
                Notify(ok ? "Metadata saved" : "Could not save metadata");
                if (ok)
                  m_library.Rebuild();
                return ok;
              },
          .notify = [this](std::string_view message) { Notify(std::string(message)); },
      });
}

void FrontendHost::OpenUri(std::string_view uri)
{
  if (uri.empty())
    return;
  try
  {
    (void)winrt::Windows::System::Launcher::LaunchUriAsync(
        winrt::Windows::Foundation::Uri(winrt::to_hstring(uri)));
  }
  catch (const winrt::hresult_error& error)
  {
    Notify(fmt::format("Unable to open link: {}", winrt::to_string(error.message())));
  }
}

void FrontendHost::PersistFrontendState(std::string_view data)
{
  std::error_code error;
  std::filesystem::create_directories(StateRoot(), error);
  std::ofstream output(StateRoot() / "frontend-state.txt", std::ios::binary | std::ios::trunc);
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string FrontendHost::LoadFrontendState() const
{
  std::ifstream input(StateRoot() / "frontend-state.txt", std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Frontend::ScreenScraperRequest
FrontendHost::MakeScreenScraperRequest(Frontend::ScreenScraperCredentials account,
                                       Frontend::ScreenScraperOptions options) const
{
  Frontend::ScreenScraperRequest request;
  request.developer = GetDeveloperIdentity();
  request.account = std::move(account);
  request.options = options;
  request.platformSystemId = GameLibrary::SCREENSCRAPER_GAMECUBE_SYSTEM_ID;
  request.platformMediaDirectory = StateRoot() / "platform-media";
  request.progressFile = m_screen_scraper_model.progressFile;

  for (const Frontend::LibraryItem& item : m_catalogue)
  {
    const auto game = m_library.FindGame(item.id);
    if (!game)
      continue;

    Frontend::ScreenScraperTarget target;
    target.id = item.id;
    target.systemId = GameLibrary::GetScreenScraperSystem(*game);
    target.name = item.name;
    target.region = item.region;
    target.contentPath = item.launchPath;
    target.mediaDirectory = StateRoot() / "metadata" / fmt::format("{:016x}", item.id);
    if (!m_screen_scraper_model.mediaDirectory.empty())
    {
      target.esDeMedia = Frontend::MakeEsDeMediaLayout(
          m_screen_scraper_model.mediaDirectory, std::string(GameLibrary::GetEsDeSystem(*game)),
          item.launchPath);
    }
    request.targets.push_back(std::move(target));
  }
  return request;
}

Frontend::ScreenScraperDeveloperIdentity FrontendHost::GetDeveloperIdentity() const
{
  Frontend::ScreenScraperDeveloperIdentity identity;
#if defined(DOLPHIN_UWP_SCREENSCRAPER_DEV_ID) && defined(DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD)
  identity.id = DOLPHIN_UWP_SCREENSCRAPER_DEV_ID;
  identity.password = DOLPHIN_UWP_SCREENSCRAPER_DEV_PASSWORD;
#endif
  identity.softwareName = "Dolphin UWP";
  return identity;
}

const Frontend::LibraryItem* FrontendHost::FindCatalogueItem(Frontend::ItemId id) const
{
  const auto found = std::ranges::find(m_catalogue, id, &Frontend::LibraryItem::id);
  return found == m_catalogue.end() ? nullptr : &*found;
}
}  // namespace DolphinUWP

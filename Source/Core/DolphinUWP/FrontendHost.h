// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <UwpImGuiFrontend/UwpImGuiFrontend.h>

#include "Common/Config/ConfigInfo.h"
#include "DolphinUWP/BackgroundTask.h"
#include "DolphinUWP/DiscToolsPage.h"
#include "DolphinUWP/GameSettingsPage.h"
#include "DolphinUWP/MappingPage.h"
#include "DolphinUWP/NetPlayController.h"
#include "DolphinUWP/NetPlayPage.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/TitlePages.h"
#include "DolphinUWP/ToolsPage.h"

namespace DolphinUWP
{
class GameLibrary;
class InputMappingHost;
class SettingsPages;

class FrontendHost final : public UwpImGuiFrontend::IFrontendHost
{
public:
  struct Callbacks
  {
    std::function<UwpImGuiFrontend::TextureHandle(const std::filesystem::path&)> load_texture;
    std::function<UwpImGuiFrontend::TextureHandle(const u8*, u32, u32)> create_texture;
    std::function<void(UwpImGuiFrontend::TextureHandle)> release_texture;
  };

  FrontendHost(GameLibrary& library, InputMappingHost& mapping, SettingsPages& settings);
  ~FrontendHost() override;

  FrontendHost(const FrontendHost&) = delete;
  FrontendHost& operator=(const FrontendHost&) = delete;

  void Initialize(Callbacks callbacks, ImFont* text_font, ImFont* shell_font);
  // Puts the storage folders that have no location in Development files on Xbox. Returns whether
  // any did.
  static bool ApplyDefaultStorage();
  // Points Dolphin at the locations of the folders it has no setting for. Runs whenever the
  // configuration changes.
  static void ApplyStoragePaths();
  void Shutdown();

  void DrawFrame(const UwpImGuiFrontend::FrameInput& input, std::string controller_status,
                 std::optional<float> controller_battery);
  // Runs work that must continue while a game is running.
  void Tick();
  NetPlayController& GetNetPlay() { return m_netplay; }
  void ReleaseGraphicsResources();
  void Notify(std::string message);

  std::optional<UwpImGuiFrontend::ItemId> TakePendingLaunch();
  std::optional<BootRequest> TakePendingBoot();
  void QueueBoot(BootRequest request);
  void QueueBootFile(const std::string& path);
  const ShellServices& GetServices() const { return m_services; }

  void PickFileToStart() { m_tools.PickFileToStart(); }
  void PickMovieToPlay() { m_tools.PickMovie(); }
  void RefreshLibrary();
  void SetRunningContent(std::optional<UwpImGuiFrontend::ItemId> item);
  bool OwnsOverlayInput() const;
  bool IsCapturingInput() const { return m_mapping_page.IsCapturing(); }
  int GetThemeIndex() const { return m_interface.themeIndex; }
  BackgroundTask& GetBackgroundTask() { return m_task; }

  std::vector<UwpImGuiFrontend::LibraryItem> SnapshotCatalogue() override;
  UwpImGuiFrontend::RunningContent GetRunningContent() const override;
  UwpImGuiFrontend::DsuEndpoint GetDsuEndpoint() const override;
  std::vector<UwpImGuiFrontend::DsuServerModel> GetDsuServers() const;
  // The choices of a storage location and the current one, as the controller mapper shows them.
  UwpImGuiFrontend::InputProfileLocation GetStorageChoices(std::string_view id) const;
  bool StartStream(const UwpImGuiFrontend::StreamRequest& request) override;
  void StopStream(bool secondary_screen) override;
  std::optional<UwpImGuiFrontend::ActiveStream> GetActiveStream() const override;
  std::optional<std::string> TakeStreamFailure() override;
  bool StartScreenStream(const UwpImGuiFrontend::StreamRequest& request) override;
  void StopScreenStream(u8 screen) override;
  std::optional<UwpImGuiFrontend::ActiveStream> GetActiveScreenStream(u8 screen) const override;
  std::optional<std::string> TakeScreenStreamFailure(u8 screen) override;
  void StopPeerScreenStream(u32 peer, u8 screen) override;
  std::optional<UwpImGuiFrontend::ActiveStream> GetActivePeerScreenStream(u32 peer,
                                                                          u8 screen) const override;
  std::optional<std::string> TakePeerScreenStreamFailure(u32 peer, u8 screen) override;
  void Log(UwpImGuiFrontend::LogLevel level, std::string_view message) override;

  std::filesystem::path ResourceRoot() const override;
  std::filesystem::path StateRoot() const override;
  UwpImGuiFrontend::TextureHandle LoadTexture(const std::filesystem::path& path) override;
  void ReleaseTexture(UwpImGuiFrontend::TextureHandle texture) override;
  bool RequestLaunch(UwpImGuiFrontend::ItemId item) override;
  void OpenContextMenu(UwpImGuiFrontend::ItemId item) override;
  void Execute(UwpImGuiFrontend::HostCommand command) override;
  void DrawSettingsPage(std::string_view page_id) override;
  void OpenUri(std::string_view uri) override;
  void PersistFrontendState(std::string_view data) override;
  std::string LoadFrontendState() const override;

private:
  enum class MapperScope
  {
    None,
    Global,
    Title,
  };

  enum class TitlePage
  {
    None,
    Cheats,
    GraphicsMods,
    Riivolution,
    GameSettings,
    DiscTools,
  };

  struct DirectionRepeat
  {
    bool Update(bool held, float delta_seconds);
    void Reset();

    bool was_held = false;
    float elapsed = 0.0f;
    float next_repeat = 0.30f;
  };

  void LoadSettings();
  void SaveInterfaceSettings();
  void SaveScreenScraperSettings();
  void SaveCompanionSettings();
  void RefreshCatalogue();

  bool HandleOverlayInput(const UwpImGuiFrontend::FrameInput& input,
                          const UwpImGuiFrontend::FrameInput& previous);
  void DrawOverlayPages();
  void DrawModal();
  void DrawSettings();
  void DrawLibrary();
  void DrawLibraryFilters();
  void UpdateLibraryItems();
  void DrawGraphics();
  void DrawContextMenu(const UwpImGuiFrontend::ShellLayoutContext& context, ImDrawList* draw,
                       const UwpImGuiFrontend::UnitRect& panel);
  void DrawInputMapper(const UwpImGuiFrontend::ShellLayoutContext& context, ImDrawList* draw);
  void ActivateTitleOption(std::string_view action);
  void DrawTitlePage();
  void CloseTitlePage();
  void ClosePage();
  void CloseContextMenu();
  void OpenMetadataEditor();
  void OpenInputMapper(std::optional<UwpImGuiFrontend::ItemId> item);
  void AddGamesDirectory(const std::filesystem::path& path);
  void DrawGameFolders();
  // Draws the games directory ("games") or the storage folder with this id.
  void DrawStorageLocation(std::string_view id);
  bool SetStorageFolder(const Config::Info<std::string>& info, const std::string& path, bool file);
  UwpImGuiFrontend::MappingSaveResult SelectStorage(std::string_view id, std::string_view choice);
  void RunAction(std::string_view action);
  void ResetPaths();
  void SyncSDCard(bool pack);
  // Copies the GBA BIOS, or a ROM for a port, into the user folder, which the GBA core can open.
  void ImportGBAFile(std::optional<size_t> port);
  // Copies a cartridge for an Advance Game Port into the user folder, where its save is kept.
  void ImportAGPCartridge(bool slot_a);
  void CopyIntoUserFolder(const std::string& source, const std::string& destination,
                          std::string title, std::function<void()> copied);
  void ClearGBARoms();
  void ChooseDefaultDisc();
  void ResetSettings();
  void OpenTools(std::string_view tab = {});
  void OpenHotkeyMapping();
  void OpenFreeLookMapping();
  void DrawMappingPage();
  void ReleaseLibraryArtwork();

  UwpImGuiFrontend::ShellPresentation BuildPresentation(std::string controller_status,
                                                        std::optional<float> controller_battery);
  UwpImGuiFrontend::ScreenScraperRequest
  MakeScreenScraperRequest(UwpImGuiFrontend::ScreenScraperCredentials account,
                           UwpImGuiFrontend::ScreenScraperOptions options) const;
  UwpImGuiFrontend::ScreenScraperDeveloperIdentity GetDeveloperIdentity() const;
  const UwpImGuiFrontend::LibraryItem* FindCatalogueItem(UwpImGuiFrontend::ItemId id) const;

  GameLibrary& m_library;
  InputMappingHost& m_mapping_host;
  SettingsPages& m_settings_pages;
  Callbacks m_callbacks;
  ImFont* m_text_font = nullptr;
  ImFont* m_shell_font = nullptr;
  bool m_initialized = false;

  std::unique_ptr<UwpImGuiFrontend::IScreenScraperService> m_screen_scraper;
  std::unique_ptr<UwpImGuiFrontend::ICompanionService> m_companion;
  std::unique_ptr<UwpImGuiFrontend::IExtensionPacketTransport> m_companion_transport;
  std::unique_ptr<UwpImGuiFrontend::ICredentialStore> m_credentials;
  std::unique_ptr<UwpImGuiFrontend::IPickerService> m_picker;
  std::unique_ptr<UwpImGuiFrontend::Frontend> m_frontend;
  std::unique_ptr<UwpImGuiFrontend::ShellRenderer> m_shell_renderer;
  std::unique_ptr<UwpImGuiFrontend::MetadataEditor> m_metadata_editor;

  UwpImGuiFrontend::SettingsView m_settings_view;
  UwpImGuiFrontend::LibraryView m_library_view;
  UwpImGuiFrontend::GraphicsView m_graphics_view;
  UwpImGuiFrontend::InputMapperView m_input_mapper;
  UwpImGuiFrontend::TitleOptionsState m_title_options_state;

  UwpImGuiFrontend::InterfaceSettingsModel m_interface;
  UwpImGuiFrontend::ScreenScraperPanelModel m_screen_scraper_model;
  // Guards the companion model and favorites, which companion threads read.
  mutable std::mutex m_companion_mutex;
  UwpImGuiFrontend::CompanionPanelModel m_companion_model;
  std::vector<UwpImGuiFrontend::SettingsLinkGroup> m_link_groups;

  std::vector<UwpImGuiFrontend::LibraryItem> m_catalogue;
  std::vector<UwpImGuiFrontend::LibraryItem> m_library_items;
  std::array<char, 128> m_library_search{};
  bool m_library_items_dirty = true;
  std::unordered_map<UwpImGuiFrontend::ItemId, UwpImGuiFrontend::TextureHandle> m_library_artwork;
  std::unordered_set<UwpImGuiFrontend::ItemId> m_favorites;
  std::string m_active_page;
  std::optional<UwpImGuiFrontend::ItemId> m_context_item;
  UwpImGuiFrontend::TextureHandle m_context_artwork;
  float m_context_detail_scroll = 0.0f;
  float m_page_scroll = 0.0f;
  bool m_context_pressed = false;
  bool m_library_context_requested = false;

  MapperScope m_mapper_scope = MapperScope::None;
  UwpImGuiFrontend::FrameInput m_mapper_frame_input;
  std::array<DirectionRepeat, 4> m_mapper_repeat;

  BackgroundTask m_task;
  ShellServices m_services;
  ToolsPage m_tools{m_services};
  NetPlayController m_netplay{m_library,
                              [this](BootRequest request) { QueueBoot(std::move(request)); }};
  NetPlayPage m_netplay_page{m_services, m_netplay};
  CheatsPage m_cheats_page{m_services};
  GraphicsModsPage m_mods_page;
  RiivolutionPage m_riivolution_page{m_services};
  GameSettingsPage m_game_settings_page;
  DiscToolsPage m_disc_tools_page{m_services};
  TitlePage m_title_page = TitlePage::None;
  bool m_context_title_installed = false;
  MappingPage m_mapping_page;
  UwpImGuiFrontend::FrameInput m_previous_input;
  bool m_shell_input_release_barrier = false;
  u64 m_modal_focus_key = 0;

  std::optional<UwpImGuiFrontend::ItemId> m_pending_launch;
  std::chrono::steady_clock::time_point m_pending_launch_time;
  std::mutex m_boot_mutex;
  std::optional<BootRequest> m_pending_boot;
  std::optional<UwpImGuiFrontend::ItemId> m_running_item;
  std::mutex m_stream_mutex;
  std::map<std::pair<u32, u8>, std::string> m_stream_failures;
};
}  // namespace DolphinUWP

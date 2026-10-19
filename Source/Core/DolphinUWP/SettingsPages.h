// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <UwpImGuiFrontend/GraphicsView.h>
#include <UwpImGuiFrontend/NativeTextInput.h>
#include <UwpImGuiFrontend/Settings.h>

#include "Common/Config/Config.h"
#include "Common/HookableEvent.h"

namespace VideoCommon
{
class PostProcessingConfiguration;
}

namespace DolphinUWP
{
class SettingsPages
{
public:
  using ActionHandler = std::function<void(std::string_view action)>;
  using StorageDrawer = std::function<void(std::string_view id)>;

  SettingsPages();
  ~SettingsPages();

  SettingsPages(const SettingsPages&) = delete;
  SettingsPages& operator=(const SettingsPages&) = delete;

  void SetActionHandler(ActionHandler handler) { m_action = std::move(handler); }
  // Draws the games directory ("games") or a storage folder with the storage presets.
  void SetStorageDrawer(StorageDrawer drawer) { m_draw_storage = std::move(drawer); }

  std::vector<UwpImGuiFrontend::SettingsExtensionPage> BuildSettingsPages();
  UwpImGuiFrontend::GraphicsViewModel BuildGraphicsModel();

  void MarkDirty() { m_dirty = true; }
  void Flush();
  // Saves changes a moment after the last one, so that they survive the app being closed while
  // a page is still open.
  void Update();

private:
  void Save();
  bool TextSetting(const char* label, const Config::Info<std::string>& info,
                   UwpImGuiFrontend::TextInputScope scope = UwpImGuiFrontend::TextInputScope::Text);
  bool ActionButton(const char* label, std::string_view action);
  void DrawAdapterSettings();
  void DrawTriforce();
  void DrawCustomRTC();
  void DrawAdapter();
  bool CustomAspectRatio();
  void DrawAudioRouting();
  void DrawLanguage();
  bool OutputDevice(const char* label, const Config::Info<std::string>& info);

  void DrawGeneral();
  void DrawOnScreenDisplay();
  void DrawGameCube();
  void DrawWii();
  void DrawAudio();
  void DrawAchievements();
  void DrawPaths();
  void DrawAdvanced();
  void DrawAbout();

  void DrawGraphicsGeneral();
  void DrawEnhancements();
  void DrawPostProcessing();
  void DrawColorCorrection();
  void DrawStereoscopy();
  void DrawHacks();
  void DrawGraphicsAdvanced();

  ActionHandler m_action;
  StorageDrawer m_draw_storage;
  std::unordered_map<std::string, std::array<char, 256>> m_text_buffers;
  std::unique_ptr<VideoCommon::PostProcessingConfiguration> m_post_processing;
  std::string m_triforce_error;
  std::vector<std::string> m_adapters;
  bool m_adapters_loaded = false;
  std::future<std::vector<std::pair<std::string, std::string>>> m_output_device_list;
  std::vector<std::pair<std::string, std::string>> m_output_devices;
  bool m_output_devices_requested = false;
  Common::EventHook m_login_hook;
  std::atomic<int> m_login_result = 0;
  bool m_login_pending = false;
  bool m_dirty = false;
  std::optional<std::chrono::steady_clock::time_point> m_dirty_since;
};
}  // namespace DolphinUWP

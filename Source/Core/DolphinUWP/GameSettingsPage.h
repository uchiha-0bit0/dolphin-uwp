// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <UwpImGuiFrontend/GraphicsView.h>

#include "Common/Config/Config.h"
#include "DolphinUWP/SettingChoices.h"

namespace Config
{
class Layer;
}

namespace UICommon
{
class GameFile;
}

namespace DolphinUWP
{
// Per-game settings, stored in the game's user settings file and applied when the game starts.
class GameSettingsPage
{
public:
  using StorageDrawer = std::function<void(std::string_view id)>;

  GameSettingsPage();
  ~GameSettingsPage();

  void SetStorageDrawer(StorageDrawer drawer) { m_draw_storage = std::move(drawer); }
  void Open(const UICommon::GameFile& game);
  void Close();
  // Returns true when the page asks to close.
  bool Draw(float controller_scroll);

private:
  void DrawCore();
  void DrawGraphics();

  template <typename T>
  T GetInherited(const Config::Info<T>& info) const;
  void BoolSetting(const char* label, const Config::Info<bool>& info, bool invert = false);
  template <typename T, size_t N>
  void ChoiceSetting(const char* label, const Config::Info<T>& info,
                     const std::array<Choice<T>, N>& choices);
  void Reset();

  StorageDrawer m_draw_storage;
  std::unique_ptr<Config::Layer> m_layer;
  std::unique_ptr<Config::Layer> m_global_layer;
  std::string m_game_id;
  UwpImGuiFrontend::GraphicsView m_view;
};
}  // namespace DolphinUWP

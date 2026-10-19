// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <UwpImGuiFrontend/GraphicsView.h>
#include <UwpImGuiFrontend/NativeTextInput.h>

#include "Common/Config/Config.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/NetPlayController.h"
#include "UICommon/NetPlayIndex.h"

namespace DolphinUWP
{
struct ShellServices;

class NetPlayPage
{
public:
  NetPlayPage(const ShellServices& services, NetPlayController& controller);

  void Open();
  void Close();
  bool Draw(float controller_scroll);

private:
  enum class GameDialog
  {
    None,
    Host,
    Change,
  };

  void DrawJoin();
  void DrawHost();
  void DrawBrowse();
  void DrawSettings();
  void DrawLobby();
  void DrawChat();
  void DrawGameDialog();
  void DrawPasswordDialog();
  void DrawConnectionSettings(bool hosting);
  void DrawControllerAssignment(const std::vector<NetPlayController::PlayerInfo>& players);
  void DrawGameCheck(const std::vector<NetPlayController::PlayerInfo>& players);

  bool TextSetting(const char* label, const Config::Info<std::string>& info,
                   UwpImGuiFrontend::TextInputScope scope = UwpImGuiFrontend::TextInputScope::Text,
                   bool password = false);
  void Join();
  void Host();
  void RefreshSessions();
  void JoinSession(const NetPlaySession& session, const std::string& server_id);

  const ShellServices& m_services;
  NetPlayController& m_controller;
  UwpImGuiFrontend::GraphicsView m_setup_view;
  UwpImGuiFrontend::GraphicsView m_lobby_view;
  std::unordered_map<std::string, std::array<char, 256>> m_text_buffers;

  GameDialog m_game_dialog = GameDialog::None;
  std::vector<GameLibrary::GameEntry> m_games;
  std::optional<GameLibrary::GameEntry> m_host_game;

  std::vector<NetPlaySession> m_sessions;
  std::string m_browse_status;
  bool m_hide_incompatible = true;
  std::optional<size_t> m_password_session;
  std::array<char, 128> m_password{};
  std::array<char, 256> m_chat{};
  bool m_session_shown = false;
};
}  // namespace DolphinUWP

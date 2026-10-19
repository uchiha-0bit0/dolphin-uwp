// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/NetPlayPage.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <map>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/ImGuiShell.h>
#include <UwpImGuiFrontend/Views.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/Version.h"
#include "Core/Config/NetplaySettings.h"
#include "DolphinUWP/BackgroundTask.h"
#include "DolphinUWP/NetPlayController.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/ShellWidgets.h"
#include "UICommon/GameFile.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

constexpr std::array<std::pair<const char*, const char*>, 3> NETWORK_MODES{{
    {"fixeddelay", "Fair input delay"},
    {"hostinputauthority", "Host input authority"},
    {"golf", "Golf mode"},
}};

constexpr std::array<const char*, 6> QUICK_MESSAGES{
    "Ready", "Pause please", "One moment", "Good game", "Restarting", "Is it lagging for you?"};

Frontend::ShellLayoutContext ViewportLayout()
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  return Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
}

bool Toggle(const char* label, const Config::Info<bool>& info)
{
  bool value = Config::Get(info);
  if (!Frontend::ShellCheckbox(label, &value))
    return false;
  Config::SetBaseOrCurrent(info, value);
  Config::Save();
  return true;
}

bool PortStepper(const char* label, const Config::Info<u16>& info)
{
  int value = Config::Get(info);
  if (!Frontend::ShellIntegerStepper(label, &value, 1, 65535))
    return false;
  Config::SetBaseOrCurrent(info, static_cast<u16>(value));
  Config::Save();
  return true;
}
}  // namespace

NetPlayPage::NetPlayPage(const ShellServices& services, NetPlayController& controller)
    : m_services(services), m_controller(controller)
{
}

void NetPlayPage::Open()
{
  m_text_buffers.clear();
  m_game_dialog = GameDialog::None;
  m_password_session.reset();
  m_setup_view = {};
  m_lobby_view = {};
  m_setup_view.State().RequestFocus(Frontend::SplitPane::Navigation);
  m_lobby_view.State().RequestFocus(Frontend::SplitPane::Navigation);
  m_session_shown = m_controller.IsActive();
}

void NetPlayPage::Close()
{
  Frontend::DismissNativeTextInput();
  m_text_buffers.clear();
  m_game_dialog = GameDialog::None;
  m_password_session.reset();
  Config::Save();
}

bool NetPlayPage::TextSetting(const char* label, const Config::Info<std::string>& info,
                              Frontend::TextInputScope scope, bool password)
{
  auto [iter, inserted] = m_text_buffers.try_emplace(label);
  auto& buffer = iter->second;
  if (inserted)
  {
    const std::string value = Config::Get(info);
    buffer.fill('\0');
    value.copy(buffer.data(), buffer.size() - 1);
  }
  if (!Frontend::NativeInputText(label, buffer.data(), buffer.size(),
                                 password ? ImGuiInputTextFlags_Password : 0, scope))
  {
    return false;
  }
  Config::SetBaseOrCurrent(info, std::string(buffer.data()));
  return true;
}

bool NetPlayPage::Draw(float controller_scroll)
{
  const bool active = m_controller.IsActive();
  if (active != m_session_shown)
  {
    m_session_shown = active;
    (active ? m_lobby_view : m_setup_view).State().RequestFocus(Frontend::SplitPane::Navigation);
  }

  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = controller_scroll;
  bool close = false;
  if (active)
  {
    model.pages = {
        {"lobby", "Lobby", true, [this] { DrawLobby(); }},
        {"chat", "Chat", true, [this] { DrawChat(); }},
    };
    close = m_lobby_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
  }
  else
  {
    model.pages = {
        {"join", "Join", true, [this] { DrawJoin(); }},
        {"host", "Host", true, [this] { DrawHost(); }},
        {"browse", "Session Browser", true, [this] { DrawBrowse(); }},
        {"settings", "Settings", true, [this] { DrawSettings(); }},
    };
    close = m_setup_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
  }
  DrawGameDialog();
  DrawPasswordDialog();
  return close;
}

void NetPlayPage::DrawConnectionSettings(bool hosting)
{
  const bool traversal = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE) == "traversal";
  const std::array<Frontend::ChoiceItem, 2> connections{{
      {"traversal", "Room code"},
      {"direct", "Direct connection"},
  }};
  size_t selected = traversal ? 0 : 1;
  if (Frontend::ShellCombo("Connection", &selected, connections))
  {
    Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, connections[selected].id);
    Config::Save();
  }
  if (hosting)
  {
    if (!traversal)
    {
      PortStepper("Port", Config::NETPLAY_HOST_PORT);
      Toggle("Forward the port with UPnP", Config::NETPLAY_USE_UPNP);
    }
    return;
  }
  if (traversal)
  {
    TextSetting("Room code", Config::NETPLAY_HOST_CODE);
  }
  else
  {
    TextSetting("Host address", Config::NETPLAY_ADDRESS, Frontend::TextInputScope::Url);
    PortStepper("Port", Config::NETPLAY_CONNECT_PORT);
  }
}

void NetPlayPage::DrawJoin()
{
  Frontend::SectionTitle("Join a session");
  DrawConnectionSettings(false);
  if (Frontend::ShellButton("Join"))
    Join();
  ShellWidgets::HintText("Ask the host for the room code, or for their address and port.");
}

void NetPlayPage::DrawHost()
{
  Frontend::SectionTitle("Host a session");
  if (ShellWidgets::ValueRow("game", "Game", m_host_game ? m_host_game->name : "Choose a game",
                             m_host_game.has_value()))
  {
    m_games = m_services.library->GetGameEntries();
    std::ranges::sort(m_games, {}, &GameLibrary::GameEntry::name);
    m_game_dialog = GameDialog::Host;
  }
  DrawConnectionSettings(true);

  const std::string mode = Config::Get(Config::NETPLAY_NETWORK_MODE);
  std::vector<Frontend::ChoiceItem> modes;
  size_t selected = 0;
  for (size_t i = 0; i < NETWORK_MODES.size(); ++i)
  {
    modes.push_back({NETWORK_MODES[i].first, NETWORK_MODES[i].second});
    if (mode == NETWORK_MODES[i].first)
      selected = i;
  }
  if (Frontend::ShellCombo("Network mode", &selected, modes))
  {
    Config::SetBaseOrCurrent(Config::NETPLAY_NETWORK_MODE,
                             std::string(NETWORK_MODES[selected].first));
    Config::Save();
  }
  int buffer = static_cast<int>(Config::Get(Config::NETPLAY_BUFFER_SIZE));
  if (Frontend::ShellIntegerStepper("Pad buffer", &buffer, 0, 64))
  {
    Config::SetBaseOrCurrent(Config::NETPLAY_BUFFER_SIZE, static_cast<u32>(buffer));
    Config::Save();
  }

  Frontend::SectionTitle("Session browser");
  Toggle("List the session in the session browser", Config::NETPLAY_USE_INDEX);
  if (Config::Get(Config::NETPLAY_USE_INDEX))
  {
    TextSetting("Session name", Config::NETPLAY_INDEX_NAME);
    const std::string region = Config::Get(Config::NETPLAY_INDEX_REGION);
    std::vector<Frontend::ChoiceItem> regions;
    size_t region_index = 0;
    for (const auto& [code, name] : NetPlayIndex::GetRegions())
    {
      if (code == region)
        region_index = regions.size();
      regions.push_back({code, name});
    }
    if (Frontend::ShellCombo("Region", &region_index, regions) && region_index < regions.size())
    {
      Config::SetBaseOrCurrent(Config::NETPLAY_INDEX_REGION, regions[region_index].id);
      Config::Save();
    }
    TextSetting("Password (optional)", Config::NETPLAY_INDEX_PASSWORD,
                Frontend::TextInputScope::Password, true);
  }

  if (Frontend::ShellButton("Host"))
    Host();
}

void NetPlayPage::DrawBrowse()
{
  Frontend::SectionTitle("Session browser");
  if (Frontend::ShellCheckbox("Only show sessions for this version", &m_hide_incompatible))
    RefreshSessions();
  if (Frontend::ShellButton("Refresh"))
    RefreshSessions();
  if (!m_browse_status.empty())
    ShellWidgets::HintText(m_browse_status);

  for (size_t i = 0; i < m_sessions.size(); ++i)
  {
    const NetPlaySession& session = m_sessions[i];
    ImGui::PushID(static_cast<int>(i));
    const std::string detail =
        fmt::format("{}  ·  {} player{}  ·  {}{}", session.game_id, session.player_count,
                    session.player_count == 1 ? "" : "s", session.region,
                    session.in_game ? "  ·  In game" : "");
    if (ShellWidgets::DetailRow("session", {}, {}, session.name, detail,
                                session.has_password ? "Password" : ""))
    {
      if (session.has_password)
      {
        m_password.fill('\0');
        m_password_session = i;
      }
      else
      {
        JoinSession(session, session.server_id);
      }
    }
    ImGui::PopID();
  }
}

void NetPlayPage::DrawSettings()
{
  Frontend::SectionTitle("Player");
  TextSetting("Nickname", Config::NETPLAY_NICKNAME, Frontend::TextInputScope::UserName);

  Frontend::SectionTitle("Data");
  Toggle("Load save data", Config::NETPLAY_SAVEDATA_LOAD);
  Toggle("Write save data", Config::NETPLAY_SAVEDATA_WRITE);
  Toggle("Sync all Wii saves", Config::NETPLAY_SAVEDATA_SYNC_ALL_WII);
  Toggle("Sync AR and Gecko codes", Config::NETPLAY_SYNC_CODES);
  Toggle("Strict settings sync", Config::NETPLAY_STRICT_SETTINGS_SYNC);
  Toggle("Record inputs", Config::NETPLAY_RECORD_INPUTS);
}

void NetPlayPage::DrawLobby()
{
  const bool hosting = m_controller.IsHosting();
  const std::string game = m_controller.GetGameName();
  Frontend::SectionTitle(game.empty() ? "No game selected" : game.c_str());
  if (hosting)
    ShellWidgets::ValueRow("room", "Room", m_controller.GetHostCode(), true);

  if (hosting)
  {
    if (!m_controller.IsGameRunning())
    {
      if (Frontend::ShellButton("Start the game"))
      {
        const auto start = [this] {
          std::string error;
          if (!m_controller.StartGame(&error) && !error.empty())
            PromptService::GetInstance().Inform("NetPlay", error, Common::MsgType::Warning);
        };
        if (m_controller.DoAllPlayersHaveGame())
        {
          start();
        }
        else
        {
          PromptService::GetInstance().Confirm(
              "NetPlay", "Not all players have the game. Do you really want to start?",
              [start](bool yes) {
                if (yes)
                  start();
              });
        }
      }
      if (Frontend::ShellButton("Change the game"))
      {
        m_games = m_services.library->GetGameEntries();
        std::ranges::sort(m_games, {}, &GameLibrary::GameEntry::name);
        m_game_dialog = GameDialog::Change;
      }
    }
    else if (Frontend::ShellButton("Stop the game"))
    {
      m_controller.RequestStopGame();
    }
    int buffer = static_cast<int>(m_controller.GetPadBuffer());
    if (Frontend::ShellIntegerStepper("Pad buffer", &buffer, 0, 64))
      m_controller.SetPadBuffer(static_cast<u32>(buffer));
    if (Frontend::ShellButton("Check that everyone has the same game"))
    {
      if (!m_controller.ComputeGameDigest())
        m_services.Notify("The game check could not be started");
    }
  }

  const auto players = m_controller.GetPlayers();
  if (hosting && !m_controller.IsGameRunning())
    DrawControllerAssignment(players);

  Frontend::SectionTitle("Players");
  for (size_t i = 0; i < players.size(); ++i)
  {
    const NetPlayController::PlayerInfo& player = players[i];
    ImGui::PushID(static_cast<int>(i));
    const std::string name = player.host  ? fmt::format("{} (host)", player.name) :
                             player.local ? fmt::format("{} (you)", player.name) :
                                            player.name;
    if (ShellWidgets::DetailRow(
            "player", {}, {}, name,
            fmt::format("{}  ·  Controllers: {}", player.status, player.mapping),
            fmt::format("{} ms", player.ping)) &&
        hosting && !player.local)
    {
      PromptService::GetInstance().Confirm("NetPlay",
                                           fmt::format("Remove {} from the session?", player.name),
                                           [this, pid = player.pid](bool confirmed) {
                                             if (confirmed)
                                               m_controller.KickPlayer(pid);
                                           });
    }
    ImGui::PopID();
  }
  if (hosting && players.size() > 1)
    ShellWidgets::HintText("Select a player to remove them from the session.");

  DrawGameCheck(players);

  Frontend::SectionTitle("Session");
  if (Frontend::ShellButton("Leave the session"))
  {
    m_controller.Leave();
    m_services.Notify("Left the NetPlay session");
  }
}

void NetPlayPage::DrawControllerAssignment(
    const std::vector<NetPlayController::PlayerInfo>& players)
{
  std::vector<Frontend::ChoiceItem> items{{"0", "Nobody"}};
  for (const NetPlayController::PlayerInfo& player : players)
    items.push_back({std::to_string(player.pid), player.name});

  const auto assignment = [&](const char* label, NetPlay::PlayerId current, const auto& apply) {
    size_t selected = 0;
    for (size_t i = 1; i < items.size(); ++i)
    {
      if (players[i - 1].pid == current)
        selected = i;
    }
    if (Frontend::ShellCombo(label, &selected, items))
      apply(selected == 0 ? NetPlay::PlayerId{0} : players[selected - 1].pid);
  };

  Frontend::SectionTitle("Controllers");
  const NetPlay::PadMappingArray pads = m_controller.GetPadMapping();
  for (size_t port = 0; port < pads.size(); ++port)
  {
    ImGui::PushID(static_cast<int>(port));
    assignment(fmt::format("GameCube port {}", port + 1).c_str(), pads[port],
               [this, port](NetPlay::PlayerId pid) { m_controller.SetPadMapping(port, pid); });
    ImGui::PopID();
  }
  const NetPlay::PadMappingArray wiimotes = m_controller.GetWiimoteMapping();
  for (size_t slot = 0; slot < wiimotes.size(); ++slot)
  {
    ImGui::PushID(static_cast<int>(slot + pads.size()));
    assignment(fmt::format("Wii Remote {}", slot + 1).c_str(), wiimotes[slot],
               [this, slot](NetPlay::PlayerId pid) { m_controller.SetWiimoteMapping(slot, pid); });
    ImGui::PopID();
  }
}

void NetPlayPage::DrawGameCheck(const std::vector<NetPlayController::PlayerInfo>& players)
{
  const std::vector<NetPlayController::DigestState> digests = m_controller.GetDigests();
  const std::string title = m_controller.GetDigestTitle();
  if (title.empty() && digests.empty())
    return;

  Frontend::SectionTitle(fmt::format("Game check: {}", title).c_str());
  bool complete = !digests.empty();
  bool same = true;
  for (size_t i = 0; i < digests.size(); ++i)
  {
    const NetPlayController::DigestState& digest = digests[i];
    const auto player = std::ranges::find(players, digest.pid, &NetPlayController::PlayerInfo::pid);
    const std::string name =
        player != players.end() ? player->name : fmt::format("Player {}", digest.pid);
    ImGui::PushID(static_cast<int>(i));
    ShellWidgets::ValueRow("digest", name,
                           digest.result.empty() ? fmt::format("{}%", digest.progress) :
                                                   digest.result);
    ImGui::PopID();
    complete &= !digest.result.empty();
    same &= digest.result == digests.front().result;
  }
  if (complete)
  {
    ShellWidgets::HintText(same ? "Every player has the same game." :
                                  "The players' game files are different.");
  }
}

void NetPlayPage::DrawChat()
{
  Frontend::SectionTitle("Send a message");
  (void)Frontend::NativeInputText("Message", m_chat.data(), m_chat.size());
  if (Frontend::ShellButton("Send") && m_chat[0] != '\0')
  {
    m_controller.SendChat(m_chat.data());
    m_chat.fill('\0');
  }
  for (size_t i = 0; i < QUICK_MESSAGES.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    if (Frontend::ShellButton(QUICK_MESSAGES[i]))
      m_controller.SendChat(QUICK_MESSAGES[i]);
    ImGui::PopID();
  }

  Frontend::SectionTitle("Messages");
  const std::vector<std::string> messages = m_controller.GetMessages();
  if (messages.empty())
    ShellWidgets::HintText("There are no messages yet.");
  for (auto it = messages.rbegin(); it != messages.rend(); ++it)
    ShellWidgets::HintText(*it);
}

void NetPlayPage::DrawGameDialog()
{
  constexpr const char* popup = "netplay-game";
  if (m_game_dialog == GameDialog::None)
    return;
  if (!ImGui::IsPopupOpen(popup))
    ImGui::OpenPopup(popup);

  Frontend::SelectionDialogModel model;
  model.popupId = popup;
  model.title = "Choose a game";
  for (const GameLibrary::GameEntry& entry : m_games)
    model.entries.push_back({std::to_string(entry.id), entry.name});
  model.closeLabel = "Cancel";
  const Frontend::SelectionDialogResult result = Frontend::DrawSelectionDialog(
      model, ViewportLayout(), Frontend::CurrentWidgetTheme(), {640.0f, 0.0f});
  if (!result.selected && !result.closed)
    return;

  const GameDialog purpose = std::exchange(m_game_dialog, GameDialog::None);
  if (!result.selected || *result.selected >= m_games.size())
    return;
  const GameLibrary::GameEntry& entry = m_games[*result.selected];
  if (purpose == GameDialog::Host)
    m_host_game = entry;
  else if (entry.game)
    m_controller.ChangeGame(*entry.game);
}

void NetPlayPage::DrawPasswordDialog()
{
  constexpr const char* popup = "netplay-password";
  if (!m_password_session)
    return;
  if (!ImGui::IsPopupOpen(popup))
    ImGui::OpenPopup(popup);

  const Frontend::ShellLayoutContext context = ViewportLayout();
  if (!Frontend::BeginThemedModal(popup, context, Frontend::CurrentWidgetTheme(),
                                  Frontend::ToPx(context, 600.0f, 0.0f)))
  {
    return;
  }
  Frontend::SectionTitle("This session requires a password");
  ImGui::SetNextItemWidth(-FLT_MIN);
  (void)Frontend::NativeInputText("##password", m_password.data(), m_password.size(),
                                  ImGuiInputTextFlags_Password, Frontend::TextInputScope::Password);
  const ImVec2 button = Frontend::ToPx(context, 170.0f, 46.0f);
  bool close = false;
  if (Frontend::ShellButton("Join", button))
  {
    close = true;
    if (*m_password_session < m_sessions.size())
    {
      const NetPlaySession& session = m_sessions[*m_password_session];
      if (const auto server_id = session.DecryptID(m_password.data()))
        JoinSession(session, *server_id);
      else
        PromptService::GetInstance().Inform("NetPlay", "The password is not correct.",
                                            Common::MsgType::Warning);
    }
  }
  ImGui::SameLine();
  if (Frontend::ShellButton("Cancel", button) ||
      (!Frontend::IsNativeTextInputActive() && Frontend::ConsumeWidgetBackPressed()))
  {
    close = true;
  }
  if (close)
  {
    Frontend::DismissNativeTextInput();
    ImGui::CloseCurrentPopup();
    m_password_session.reset();
  }
  Frontend::EndThemedModal();
}

void NetPlayPage::Join()
{
  Config::Save();
  std::string error;
  if (!m_controller.Join(&error))
    PromptService::GetInstance().Inform("NetPlay", error, Common::MsgType::Warning);
}

void NetPlayPage::Host()
{
  if (!m_host_game || !m_host_game->game)
  {
    m_services.Notify("Choose a game to host");
    return;
  }
  Config::Save();
  std::string error;
  if (!m_controller.Host(*m_host_game->game, &error))
    PromptService::GetInstance().Inform("NetPlay", error, Common::MsgType::Warning);
}

void NetPlayPage::RefreshSessions()
{
  m_browse_status = "Refreshing...";
  std::map<std::string, std::string> filters;
  if (m_hide_incompatible)
    filters["version"] = Common::GetScmDescStr();
  m_services.task->Start(
      "Refreshing sessions", false,
      [this, filters = std::move(filters)](BackgroundTask::Context&) -> BackgroundTask::Completion {
        NetPlayIndex index;
        auto sessions = index.List(filters);
        std::string error = sessions ? std::string() : index.GetLastError();
        return [this, sessions = std::move(sessions), error = std::move(error)]() mutable {
          if (!sessions)
          {
            m_browse_status = fmt::format("The session list could not be loaded: {}", error);
            return;
          }
          m_sessions = std::move(*sessions);
          m_browse_status = m_sessions.empty() ? "There are no compatible sessions right now." :
                                                 fmt::format("{} session(s)", m_sessions.size());
        };
      });
}

void NetPlayPage::JoinSession(const NetPlaySession& session, const std::string& server_id)
{
  Config::SetBaseOrCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, session.method);
  Config::SetBaseOrCurrent(Config::NETPLAY_CONNECT_PORT, static_cast<u16>(session.port));
  if (session.method == "traversal")
    Config::SetBaseOrCurrent(Config::NETPLAY_HOST_CODE, server_id);
  else
    Config::SetBaseOrCurrent(Config::NETPLAY_ADDRESS, server_id);
  m_text_buffers.clear();
  Join();
}
}  // namespace DolphinUWP

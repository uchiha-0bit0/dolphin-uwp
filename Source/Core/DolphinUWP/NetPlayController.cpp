// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/NetPlayController.h"

#include <algorithm>
#include <utility>

#include <fmt/format.h>

#include "Common/TraversalClient.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/NetPlayServer.h"
#include "Core/System.h"
#include "Core/TitleDatabase.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/HostState.h"
#include "DolphinUWP/ShellServices.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace DolphinUWP
{
namespace
{
constexpr size_t MESSAGE_HISTORY = 100;

const char* DescribeStatus(NetPlay::SyncIdentifierComparison status)
{
  switch (status)
  {
  case NetPlay::SyncIdentifierComparison::SameGame:
    return "Has the game";
  case NetPlay::SyncIdentifierComparison::DifferentHash:
    return "Different game data";
  case NetPlay::SyncIdentifierComparison::DifferentDiscNumber:
    return "Different disc";
  case NetPlay::SyncIdentifierComparison::DifferentRevision:
    return "Different revision";
  case NetPlay::SyncIdentifierComparison::DifferentRegion:
    return "Different region";
  case NetPlay::SyncIdentifierComparison::DifferentGame:
    return "Missing the game";
  default:
    return "Unknown";
  }
}

std::string DescribeMapping(const NetPlay::PadMappingArray& pads,
                            const NetPlay::PadMappingArray& wiimotes, NetPlay::PlayerId pid)
{
  std::string mapping;
  for (size_t i = 0; i < pads.size(); ++i)
  {
    if (pads[i] == pid)
      mapping += fmt::format("{}GC{}", mapping.empty() ? "" : " ", i + 1);
  }
  for (size_t i = 0; i < wiimotes.size(); ++i)
  {
    if (wiimotes[i] == pid)
      mapping += fmt::format("{}Wii{}", mapping.empty() ? "" : " ", i + 1);
  }
  return mapping.empty() ? std::string("None") : mapping;
}
}  // namespace

NetPlayController::NetPlayController(GameLibrary& library, BootCallback boot)
    : m_library(library), m_boot(std::move(boot))
{
}

NetPlayController::~NetPlayController()
{
  Leave();
}

bool NetPlayController::Host(const UICommon::GameFile& game, std::string* error)
{
  if (m_client)
  {
    *error = "A NetPlay session is already in progress.";
    return false;
  }
  if (!Core::IsUninitialized(Core::System::GetInstance()))
  {
    *error = "A NetPlay session cannot start while a game is running.";
    return false;
  }

  const bool traversal = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE) == "traversal";
  const u16 port =
      traversal ? Config::Get(Config::NETPLAY_LISTEN_PORT) : Config::Get(Config::NETPLAY_HOST_PORT);
  m_server = std::make_unique<NetPlay::NetPlayServer>(
      port, Config::Get(Config::NETPLAY_USE_UPNP), this,
      NetPlay::NetTraversalConfig{traversal, Config::Get(Config::NETPLAY_TRAVERSAL_SERVER),
                                  Config::Get(Config::NETPLAY_TRAVERSAL_PORT),
                                  Config::Get(Config::NETPLAY_TRAVERSAL_PORT_ALT)});
  if (!m_server->is_connected)
  {
    m_server.reset();
    *error =
        fmt::format("Failed to listen on port {}. Another NetPlay server may be running.", port);
    return false;
  }

  const Core::TitleDatabase titles;
  m_server->ChangeGame(game.GetSyncIdentifier(), game.GetNetPlayName(titles));
  return Join(error);
}

bool NetPlayController::Join(std::string* error)
{
  if (m_client)
  {
    *error = "A NetPlay session is already in progress.";
    return false;
  }

  const bool traversal = Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE) == "traversal";
  std::string address;
  u16 port = 0;
  if (m_server)
  {
    address = "127.0.0.1";
    port = m_server->GetPort();
    const std::string mode = Config::Get(Config::NETPLAY_NETWORK_MODE);
    m_server->SetHostInputAuthority(mode == "hostinputauthority" || mode == "golf");
    m_server->AdjustPadBufferSize(Config::Get(Config::NETPLAY_BUFFER_SIZE));
  }
  else
  {
    address =
        traversal ? Config::Get(Config::NETPLAY_HOST_CODE) : Config::Get(Config::NETPLAY_ADDRESS);
    port = Config::Get(Config::NETPLAY_CONNECT_PORT);
  }

  {
    std::lock_guard lock(m_mutex);
    m_messages.clear();
  }
  m_pad_buffer = Config::Get(Config::NETPLAY_BUFFER_SIZE);
  m_client = std::make_unique<NetPlay::NetPlayClient>(
      address, port, this, Config::Get(Config::NETPLAY_NICKNAME),
      NetPlay::NetTraversalConfig{m_server ? false : traversal,
                                  Config::Get(Config::NETPLAY_TRAVERSAL_SERVER),
                                  Config::Get(Config::NETPLAY_TRAVERSAL_PORT)});
  if (!m_client->IsConnected())
  {
    Leave();
    std::lock_guard lock(m_mutex);
    *error = m_messages.empty() ? std::string("Could not connect to the NetPlay session.") :
                                  m_messages.back();
    return false;
  }
  return true;
}

void NetPlayController::Leave()
{
  m_client.reset();
  m_server.reset();
  m_game_running = false;
  std::lock_guard lock(m_mutex);
  m_posted.clear();
}

void NetPlayController::Post(std::function<void()> work)
{
  std::lock_guard lock(m_mutex);
  m_posted.push_back(std::move(work));
}

void NetPlayController::Tick()
{
  std::vector<std::function<void()>> posted;
  {
    std::lock_guard lock(m_mutex);
    posted.swap(m_posted);
  }
  for (const auto& work : posted)
    work();
}

void NetPlayController::AddMessage(std::string message)
{
  if (m_game_running)
    OSD::AddMessage(message, OSD::Duration::NORMAL, OSD::Color::CYAN);
  std::lock_guard lock(m_mutex);
  m_messages.push_back(std::move(message));
  while (m_messages.size() > MESSAGE_HISTORY)
    m_messages.pop_front();
}

std::vector<std::string> NetPlayController::GetMessages() const
{
  std::lock_guard lock(m_mutex);
  return {m_messages.begin(), m_messages.end()};
}

bool NetPlayController::IsHosting() const
{
  return m_server != nullptr;
}

std::vector<NetPlayController::PlayerInfo> NetPlayController::GetPlayers()
{
  std::vector<PlayerInfo> players;
  if (!m_client)
    return players;
  const auto& pads = m_client->GetPadMapping();
  const auto& wiimotes = m_client->GetWiimoteMapping();
  for (const NetPlay::Player* player : m_client->GetPlayers())
  {
    players.push_back({player->pid, player->name, DescribeStatus(player->game_status),
                       DescribeMapping(pads, wiimotes, player->pid), player->ping, player->IsHost(),
                       m_client->IsLocalPlayer(player->pid)});
  }
  return players;
}

std::string NetPlayController::GetGameName() const
{
  std::lock_guard lock(m_mutex);
  return m_game_name;
}

std::string NetPlayController::GetHostCode() const
{
  if (!m_server)
    return {};
  if (Config::Get(Config::NETPLAY_TRAVERSAL_CHOICE) != "traversal" || !Common::g_TraversalClient)
    return fmt::format("Port {}", m_server->GetPort());

  switch (Common::g_TraversalClient->GetState())
  {
  case Common::TraversalClient::State::Connecting:
    return "Connecting...";
  case Common::TraversalClient::State::Failure:
    return "Error";
  default:
  {
    const auto host_id = Common::g_TraversalClient->GetHostID();
    return std::string(host_id.begin(), host_id.end());
  }
  }
}

bool NetPlayController::DoAllPlayersHaveGame()
{
  return m_client && m_client->DoAllPlayersHaveGame();
}

void NetPlayController::SendChat(const std::string& message)
{
  if (!m_client || message.empty())
    return;
  m_client->SendChatMessage(message);
  AddMessage(fmt::format("{}: {}", Config::Get(Config::NETPLAY_NICKNAME), message));
}

void NetPlayController::ChangeGame(const UICommon::GameFile& game)
{
  if (!m_server)
    return;
  const Core::TitleDatabase titles;
  m_server->ChangeGame(game.GetSyncIdentifier(), game.GetNetPlayName(titles));
}

bool NetPlayController::StartGame(std::string* error)
{
  if (!m_server)
    return false;
  if (Config::Get(Config::NETPLAY_STRICT_SETTINGS_SYNC) && Config::Get(Config::GFX_EFB_SCALE) == 0)
  {
    *error = "Automatic internal resolution is not allowed in strict sync mode, because it depends "
             "on the window size. Choose a specific internal resolution.";
    return false;
  }
  NetPlay::SyncIdentifier identifier;
  {
    std::lock_guard lock(m_mutex);
    identifier = m_game_identifier;
  }
  if (!FindGameFile(identifier))
  {
    *error = "The selected game is not in the game library.";
    return false;
  }
  m_stop_requested = false;
  return m_server->RequestStartGame();
}

void NetPlayController::RequestStopGame()
{
  if (m_client)
    m_client->RequestStopGame();
}

void NetPlayController::RequestGolfControl()
{
  if (m_client)
    m_client->RequestGolfControl();
}

void NetPlayController::SetPadBuffer(u32 buffer)
{
  m_pad_buffer = buffer;
  Config::SetBaseOrCurrent(Config::NETPLAY_BUFFER_SIZE, buffer);
  if (m_server)
    m_server->AdjustPadBufferSize(buffer);
}

void NetPlayController::BootGame(const std::string& filename,
                                 std::unique_ptr<BootSessionData> boot_session_data)
{
  m_stop_requested = false;
  std::unique_ptr<BootParameters> parameters =
      BootParameters::GenerateFromFile(filename, std::move(*boot_session_data));
  std::string title;
  std::optional<UwpImGuiFrontend::ItemId> item;
  if (const auto entry = m_library.FindItemByPath(filename))
  {
    title = entry->name;
    item = entry->id;
  }
  else
  {
    title = GetGameName();
  }
  m_boot({std::move(parameters), std::move(title), item});
}

void NetPlayController::StopGame()
{
  if (m_stop_requested.exchange(true))
    return;
  HostState::RequestStop();
}

void NetPlayController::AppendChat(const std::string& message)
{
  AddMessage(message);
}

void NetPlayController::OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                                        const std::string& netplay_name)
{
  {
    std::lock_guard lock(m_mutex);
    m_game_identifier = sync_identifier;
    m_game_name = netplay_name;
  }
  AddMessage(fmt::format("The game changed to {}", netplay_name));
}

void NetPlayController::OnMsgStartGame()
{
  m_game_running = true;
  AddMessage("The game started");
  Post([this] {
    if (!m_client)
      return;
    NetPlay::SyncIdentifier identifier;
    {
      std::lock_guard lock(m_mutex);
      identifier = m_game_identifier;
    }
    if (const auto game = FindGameFile(identifier))
      m_client->StartGame(game->GetFilePath());
    else
      AddMessage("The selected game is not in the game library");
  });
}

void NetPlayController::OnMsgStopGame()
{
  m_game_running = false;
}

void NetPlayController::OnMsgPowerButton()
{
  if (Core::IsRunning(Core::System::GetInstance()))
    Post([] { UICommon::TriggerSTMPowerEvent(); });
}

void NetPlayController::OnPlayerConnect(const std::string& player)
{
  AddMessage(fmt::format("{} joined", player));
}

void NetPlayController::OnPlayerDisconnect(const std::string& player)
{
  AddMessage(fmt::format("{} left", player));
}

void NetPlayController::OnPadBufferChanged(u32 buffer)
{
  m_pad_buffer = buffer;
  AddMessage(fmt::format("The pad buffer changed to {}", buffer));
}

void NetPlayController::OnHostInputAuthorityChanged(bool enabled)
{
  AddMessage(enabled ? "Host input authority is on" : "Host input authority is off");
}

void NetPlayController::OnDesync(u32 frame, const std::string& player)
{
  AddMessage(fmt::format("Possible desync: {} might have desynced at frame {}", player, frame));
}

void NetPlayController::OnConnectionLost()
{
  AddMessage("Lost the connection to the NetPlay server");
}

void NetPlayController::OnConnectionError(const std::string& message)
{
  AddMessage(fmt::format("Failed to connect to the server: {}", message));
}

void NetPlayController::OnTraversalError(Common::TraversalClient::FailureReason error)
{
  switch (error)
  {
  case Common::TraversalClient::FailureReason::BadHost:
    AddMessage("Couldn't look up the traversal server");
    break;
  case Common::TraversalClient::FailureReason::VersionTooOld:
    AddMessage("This version of Dolphin is too old for the traversal server");
    break;
  default:
    AddMessage("The connection to the traversal server failed");
    break;
  }
}

void NetPlayController::OnTraversalStateChanged(Common::TraversalClient::State)
{
}

void NetPlayController::OnGameStartAborted()
{
  AddMessage("The game start was cancelled");
}

void NetPlayController::OnGolferChanged(bool is_golfer, const std::string& golfer_name)
{
  if (!golfer_name.empty())
    AddMessage(is_golfer ? "You have control" : fmt::format("{} has control", golfer_name));
}

bool NetPlayController::IsRecording()
{
  return Config::Get(Config::NETPLAY_RECORD_INPUTS);
}

std::shared_ptr<const UICommon::GameFile>
NetPlayController::FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
                                NetPlay::SyncIdentifierComparison* found)
{
  NetPlay::SyncIdentifierComparison best = NetPlay::SyncIdentifierComparison::DifferentGame;
  std::shared_ptr<const UICommon::GameFile> match;
  for (const GameLibrary::GameEntry& entry : m_library.GetGameEntries())
  {
    if (!entry.game)
      continue;
    const NetPlay::SyncIdentifierComparison comparison =
        entry.game->CompareSyncIdentifier(sync_identifier);
    best = std::min(best, comparison);
    if (comparison == NetPlay::SyncIdentifierComparison::SameGame)
    {
      match = entry.game;
      break;
    }
  }
  if (found)
    *found = best;
  return match;
}

NetPlay::PadMappingArray NetPlayController::GetPadMapping() const
{
  return m_server ? m_server->GetPadMapping() : NetPlay::PadMappingArray{};
}

NetPlay::PadMappingArray NetPlayController::GetWiimoteMapping() const
{
  return m_server ? m_server->GetWiimoteMapping() : NetPlay::PadMappingArray{};
}

void NetPlayController::SetPadMapping(size_t port, NetPlay::PlayerId player)
{
  if (!m_server || port >= NetPlay::PadMappingArray{}.size())
    return;
  NetPlay::PadMappingArray mapping = m_server->GetPadMapping();
  mapping[port] = player;
  m_server->SetPadMapping(mapping);
}

void NetPlayController::SetWiimoteMapping(size_t slot, NetPlay::PlayerId player)
{
  if (!m_server || slot >= NetPlay::PadMappingArray{}.size())
    return;
  NetPlay::PadMappingArray mapping = m_server->GetWiimoteMapping();
  mapping[slot] = player;
  m_server->SetWiimoteMapping(mapping);
}

void NetPlayController::KickPlayer(NetPlay::PlayerId player)
{
  if (m_server)
    m_server->KickPlayer(player);
}

bool NetPlayController::ComputeGameDigest()
{
  if (!m_server)
    return false;
  NetPlay::SyncIdentifier identifier;
  {
    std::lock_guard lock(m_mutex);
    identifier = m_game_identifier;
  }
  return m_server->ComputeGameDigest(identifier);
}

std::string NetPlayController::GetDigestTitle() const
{
  std::lock_guard lock(m_mutex);
  return m_digest_title;
}

std::vector<NetPlayController::DigestState> NetPlayController::GetDigests() const
{
  std::lock_guard lock(m_mutex);
  return m_digests;
}

void NetPlayController::ShowGameDigestDialog(const std::string& title)
{
  std::lock_guard lock(m_mutex);
  m_digest_title = title;
  m_digests.clear();
}

void NetPlayController::SetGameDigestProgress(int pid, int progress)
{
  std::lock_guard lock(m_mutex);
  const auto found =
      std::ranges::find(m_digests, static_cast<NetPlay::PlayerId>(pid), &DigestState::pid);
  if (found == m_digests.end())
    m_digests.push_back({static_cast<NetPlay::PlayerId>(pid), progress, {}});
  else
    found->progress = progress;
}

void NetPlayController::SetGameDigestResult(int pid, const std::string& result)
{
  std::lock_guard lock(m_mutex);
  const auto found =
      std::ranges::find(m_digests, static_cast<NetPlay::PlayerId>(pid), &DigestState::pid);
  if (found == m_digests.end())
  {
    m_digests.push_back({static_cast<NetPlay::PlayerId>(pid), 100, result});
  }
  else
  {
    found->progress = 100;
    found->result = result;
  }
}

void NetPlayController::AbortGameDigest()
{
  std::lock_guard lock(m_mutex);
  m_digest_title.clear();
  m_digests.clear();
}

void NetPlayController::OnIndexAdded(bool success, std::string error)
{
  AddMessage(success ? std::string("The session is listed in the session browser") :
                       fmt::format("The session could not be listed: {}", error));
}

void NetPlayController::OnIndexRefreshFailed(std::string error)
{
  AddMessage(fmt::format("The session browser listing failed: {}", error));
}

void NetPlayController::ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                                  std::span<const int>)
{
  AddMessage(fmt::format("Sending {} ({} KiB)", title, data_size / 1024));
}

void NetPlayController::SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder)
{
  if (m_client)
    m_client->SetWiiSyncData(nullptr, std::move(titles), std::move(redirect_folder));
}
}  // namespace DolphinUWP

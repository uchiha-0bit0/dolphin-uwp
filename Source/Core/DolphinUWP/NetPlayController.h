// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/NetPlayClient.h"
#include "Core/SyncIdentifier.h"

namespace NetPlay
{
class NetPlayServer;
}

namespace DolphinUWP
{
class GameLibrary;
struct BootRequest;

// Owns the NetPlay client and server and answers their callbacks. The client calls back on its
// own thread, so work that touches the frontend is queued for Tick on the UI thread.
class NetPlayController final : public NetPlay::NetPlayUI
{
public:
  struct PlayerInfo
  {
    NetPlay::PlayerId pid = 0;
    std::string name;
    std::string status;
    std::string mapping;
    u32 ping = 0;
    bool host = false;
    bool local = false;
  };

  struct DigestState
  {
    NetPlay::PlayerId pid = 0;
    int progress = 0;
    std::string result;
  };

  using BootCallback = std::function<void(BootRequest)>;

  NetPlayController(GameLibrary& library, BootCallback boot);
  ~NetPlayController() override;

  NetPlayController(const NetPlayController&) = delete;
  NetPlayController& operator=(const NetPlayController&) = delete;

  // Host and Join read the connection settings from the NetPlay configuration.
  bool Host(const UICommon::GameFile& game, std::string* error);
  bool Join(std::string* error);
  void Leave();
  void Tick();

  bool IsActive() const { return m_client != nullptr; }
  bool IsHosting() const override;
  bool IsGameRunning() const { return m_game_running; }

  std::vector<PlayerInfo> GetPlayers();
  std::string GetGameName() const;
  std::string GetHostCode() const;
  std::vector<std::string> GetMessages() const;
  bool DoAllPlayersHaveGame();

  void SendChat(const std::string& message);
  void ChangeGame(const UICommon::GameFile& game);
  bool StartGame(std::string* error);
  void StopGame() override;
  void RequestStopGame();
  void RequestGolfControl();
  void SetPadBuffer(u32 buffer);
  u32 GetPadBuffer() const { return m_pad_buffer; }

  // Host only.
  NetPlay::PadMappingArray GetPadMapping() const;
  NetPlay::PadMappingArray GetWiimoteMapping() const;
  void SetPadMapping(size_t port, NetPlay::PlayerId player);
  void SetWiimoteMapping(size_t slot, NetPlay::PlayerId player);
  void KickPlayer(NetPlay::PlayerId player);
  bool ComputeGameDigest();

  std::string GetDigestTitle() const;
  std::vector<DigestState> GetDigests() const;

  // NetPlayUI
  void BootGame(const std::string& filename,
                std::unique_ptr<BootSessionData> boot_session_data) override;
  void Update() override {}
  void AppendChat(const std::string& message) override;
  void OnMsgChangeGame(const NetPlay::SyncIdentifier& sync_identifier,
                       const std::string& netplay_name) override;
  void OnMsgChangeGBARom(int, const NetPlay::GBAConfig&) override {}
  void OnMsgStartGame() override;
  void OnMsgStopGame() override;
  void OnMsgPowerButton() override;
  void OnPlayerConnect(const std::string& player) override;
  void OnPlayerDisconnect(const std::string& player) override;
  void OnPadBufferChanged(u32 buffer) override;
  void OnHostInputAuthorityChanged(bool enabled) override;
  void OnDesync(u32 frame, const std::string& player) override;
  void OnConnectionLost() override;
  void OnConnectionError(const std::string& message) override;
  void OnTraversalError(Common::TraversalClient::FailureReason error) override;
  void OnTraversalStateChanged(Common::TraversalClient::State state) override;
  void OnGameStartAborted() override;
  void OnGolferChanged(bool is_golfer, const std::string& golfer_name) override;
  void OnTtlDetermined(u8) override {}
  bool IsRecording() override;
  std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const NetPlay::SyncIdentifier& sync_identifier,
               NetPlay::SyncIdentifierComparison* found = nullptr) override;
  std::string FindGBARomPath(const std::array<u8, 20>&, std::string_view, int) override
  {
    return {};
  }
  void ShowGameDigestDialog(const std::string& title) override;
  void SetGameDigestProgress(int pid, int progress) override;
  void SetGameDigestResult(int pid, const std::string& result) override;
  void AbortGameDigest() override;
  void OnIndexAdded(bool success, std::string error) override;
  void OnIndexRefreshFailed(std::string error) override;
  void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                 std::span<const int> players) override;
  void HideChunkedProgressDialog() override {}
  void SetChunkedProgress(int, u64) override {}
  void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) override;

private:
  void Post(std::function<void()> work);
  void AddMessage(std::string message);

  GameLibrary& m_library;
  BootCallback m_boot;
  std::unique_ptr<NetPlay::NetPlayServer> m_server;
  std::unique_ptr<NetPlay::NetPlayClient> m_client;

  mutable std::mutex m_mutex;
  std::vector<std::function<void()>> m_posted;
  std::deque<std::string> m_messages;
  NetPlay::SyncIdentifier m_game_identifier;
  std::string m_game_name;
  std::string m_digest_title;
  std::vector<DigestState> m_digests;
  std::atomic<u32> m_pad_buffer = 0;
  std::atomic<bool> m_game_running = false;
  std::atomic<bool> m_stop_requested = false;
};
}  // namespace DolphinUWP

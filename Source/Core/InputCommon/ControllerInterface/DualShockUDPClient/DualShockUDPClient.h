// Copyright 2019 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/ConfigInfo.h"
#include "InputCommon/ControllerInterface/InputBackend.h"

namespace ciface::DualShockUDPClient
{
constexpr char DEFAULT_SERVER_ADDRESS[] = "127.0.0.1";
constexpr u16 DEFAULT_SERVER_PORT = 26760;

namespace Settings
{
// These two kept for backwards compatibility
extern const Config::Info<std::string> SERVER_ADDRESS;
extern const Config::Info<int> SERVER_PORT;

extern const Config::Info<std::string> SERVERS;
extern const Config::Info<bool> SERVERS_ENABLED;
}  // namespace Settings

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);

// Lets a frontend exchange protocol extension datagrams with the servers, over the sockets the
// client already uses for them. A server is identified by a nonzero number derived from its
// address. The handler runs on the client thread and returns true when it consumed a datagram; its
// responses are sent back to the server that sent it.
using ExtensionHandler = std::function<bool(u32 server, std::span<const u8> datagram,
                                            std::vector<std::vector<u8>>& responses)>;
void SetExtensionHandler(ExtensionHandler handler);
bool SendToServer(u32 server, std::span<const u8> datagram);
void SendToServers(std::span<const u8> datagram);
void RequestPadData(u8 pad_id);
// The pad slots that the server reports as connected, one bit per slot. A server sends each
// player's controller in that player's slot.
u8 GetConnectedPads(u32 server);

}  // namespace ciface::DualShockUDPClient

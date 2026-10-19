// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/DebuggerNetwork.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Network/SSL.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/System.h"

namespace DolphinUWP::DebuggerNetwork
{
namespace
{
std::string Domain(s32 host_fd)
{
  sockaddr address{};
  socklen_t length = sizeof(address);
  if (getsockname(host_fd, &address, &length) != 0)
    return "Unknown";
  switch (address.sa_family)
  {
  case AF_INET:
    return "AF_INET";
  case AF_INET6:
    return "AF_INET6";
  default:
    return std::to_string(address.sa_family);
  }
}

std::string Type(s32 host_fd)
{
  int type = 0;
  socklen_t length = sizeof(type);
  if (getsockopt(host_fd, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) != 0)
    return "Unknown";
  switch (type)
  {
  case SOCK_STREAM:
    return "SOCK_STREAM";
  case SOCK_DGRAM:
    return "SOCK_DGRAM";
  default:
    return std::to_string(type);
  }
}

std::string State(s32 host_fd)
{
  sockaddr_in peer{};
  socklen_t peer_length = sizeof(peer);
  if (getpeername(host_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) == 0)
    return "Connected";
  int accepting = 0;
  socklen_t length = sizeof(accepting);
  if (getsockopt(host_fd, SOL_SOCKET, SO_ACCEPTCONN, reinterpret_cast<char*>(&accepting),
                 &length) == 0 &&
      accepting > 0)
  {
    return "Listening";
  }
  return "Unbound";
}

std::string AddressAndPort(const sockaddr_in& address)
{
  char buffer[INET_ADDRSTRLEN];
  if (!inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer)))
    return {};
  return fmt::format("{}:{}", buffer, ntohs(address.sin_port));
}

std::string Name(s32 host_fd)
{
  sockaddr_in local{};
  socklen_t local_length = sizeof(local);
  if (getsockname(host_fd, reinterpret_cast<sockaddr*>(&local), &local_length) != 0)
    return "Unknown";
  const std::string local_name = AddressAndPort(local);
  sockaddr_in peer{};
  socklen_t peer_length = sizeof(peer);
  if (local_name.empty() ||
      getpeername(host_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0)
  {
    return local_name.empty() ? std::string("Unknown") : local_name;
  }
  const std::string peer_name = AddressAndPort(peer);
  return peer_name.empty() ? local_name : fmt::format("{} -> {}", local_name, peer_name);
}

Socket Describe(std::string label, s32 host_fd, std::string extra = {})
{
  label = fmt::format("{}  {} {}", label, Domain(host_fd), Type(host_fd));
  if (!extra.empty())
    label += fmt::format("  {}", extra);
  return {std::move(label), fmt::format("{}  {}", State(host_fd), Name(host_fd))};
}
}  // namespace

std::vector<Socket> List(Core::System& system)
{
  std::vector<Socket> sockets;
  if (system.IsTriforce())
  {
    for (u32 fd = 0; fd < AMMediaboard::SOCKET_FD_MAX; ++fd)
    {
      const s32 host_fd = AMMediaboard::DebuggerGetSocket(fd);
      if (host_fd >= 0)
        sockets.push_back(Describe(fmt::format("Socket {}", fd), host_fd));
    }
    return sockets;
  }

  IOS::HLE::EmulationKernel* const ios = system.GetIOS();
  if (!system.IsWii() || !ios)
    return sockets;
  const auto socket_manager = ios->GetSocketManager();
  if (socket_manager)
  {
    for (s32 fd = 0; fd < IOS::HLE::WII_SOCKET_FD_MAX; ++fd)
    {
      const s32 host_fd = socket_manager->GetHostSocket(fd);
      if (host_fd >= 0)
      {
        sockets.push_back(Describe(fmt::format("Socket {}", fd), host_fd,
                                   socket_manager->IsSocketBlocking(fd) ? "blocking" : ""));
      }
    }
  }
  for (int id = 0; id < IOS::HLE::NET_SSL_MAXINSTANCES; ++id)
  {
    if (!IOS::HLE::IsSSLIDValid(id))
      continue;
    const auto& ssl = IOS::HLE::NetSSLDevice::_SSL[id];
    sockets.push_back(Describe(fmt::format("SSL {}", id), ssl.hostfd, ssl.hostname));
  }
  return sockets;
}
}  // namespace DolphinUWP::DebuggerNetwork

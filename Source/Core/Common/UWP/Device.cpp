// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/UWP/Device.h"

#include <vector>

#include <Windows.h>
#include <appmodel.h>
#include <gamingdeviceinformation.h>

#include <fmt/format.h>

namespace Common::UWP
{
bool IsXboxConsole()
{
  static const bool is_xbox = [] {
    GAMING_DEVICE_MODEL_INFORMATION info{};
    return SUCCEEDED(GetGamingDeviceModelInformation(&info)) &&
           info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT;
  }();
  return is_xbox;
}

std::string_view GetDeviceName()
{
  GAMING_DEVICE_MODEL_INFORMATION info{};
  if (FAILED(GetGamingDeviceModelInformation(&info)) ||
      info.vendorId != GAMING_DEVICE_VENDOR_ID_MICROSOFT)
  {
    return "Windows";
  }

  switch (info.deviceId)
  {
  case GAMING_DEVICE_DEVICE_ID_XBOX_ONE:
    return "Xbox One";
  case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_S:
    return "Xbox One S";
  case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X:
    return "Xbox One X";
  case GAMING_DEVICE_DEVICE_ID_XBOX_ONE_X_DEVKIT:
    return "Xbox One X Dev Kit";
  case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_S:
    return "Xbox Series S";
  case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X:
    return "Xbox Series X";
  case GAMING_DEVICE_DEVICE_ID_XBOX_SERIES_X_DEVKIT:
    return "Xbox Series X Dev Kit";
  default:
    return "Xbox";
  }
}

std::string GetPackageVersion()
{
  UINT32 length = 0;
  if (GetCurrentPackageId(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
    return {};
  std::vector<BYTE> buffer(length);
  if (GetCurrentPackageId(&length, buffer.data()) != ERROR_SUCCESS)
    return {};
  const PACKAGE_VERSION& version = reinterpret_cast<const PACKAGE_ID*>(buffer.data())->version;
  return fmt::format("{}.{}.{}.{}", version.Major, version.Minor, version.Build, version.Revision);
}
}  // namespace Common::UWP

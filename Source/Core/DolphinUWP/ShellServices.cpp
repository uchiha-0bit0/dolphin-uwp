// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/ShellServices.h"

#include <memory>
#include <utility>

#include <fmt/format.h>
#include <fmt/xchar.h>

#include "Common/StringUtil.h"
#include "Core/State.h"

namespace DolphinUWP
{
namespace
{
UwpImGuiFrontend::PickerCallbacks MakeCallbacks(const ShellServices& services,
                                                std::function<void(std::string)> completed)
{
  auto shared = std::make_shared<std::function<void(std::string)>>(std::move(completed));
  return {
      .completed =
          [shared](std::unique_ptr<UwpImGuiFrontend::PickedItem> item) {
            (*shared)(item ? PathToString(item->path) : std::string());
          },
      .failed =
          [shared, notify = services.notify](std::string_view message) {
            if (notify)
              notify(std::string(message));
            (*shared)({});
          },
  };
}
}  // namespace

bool ShellServices::PickFile(std::wstring extension, std::wstring token,
                             std::function<void(std::string path)> completed) const
{
  std::vector<std::wstring> extensions;
  if (!extension.empty())
    extensions.push_back(std::move(extension));
  return PickFile(std::move(extensions), std::move(token), std::move(completed));
}

bool ShellServices::PickFile(std::vector<std::wstring> extensions, std::wstring token,
                             std::function<void(std::string path)> completed) const
{
  if (!picker)
    return false;

  UwpImGuiFrontend::PickerRequest request;
  request.fileTypes = std::move(extensions);
  request.futureAccessToken = std::move(token);
  if (picker->PickFile(std::move(request), MakeCallbacks(*this, completed)))
    return true;
  Notify("A file picker is already open");
  completed({});
  return false;
}

bool ShellServices::PickFolder(std::wstring token,
                               std::function<void(std::string path)> completed) const
{
  if (!picker)
    return false;

  UwpImGuiFrontend::PickerRequest request;
  request.futureAccessToken = std::move(token);
  if (picker->PickFolder(std::move(request), MakeCallbacks(*this, completed)))
    return true;
  Notify("A folder picker is already open");
  completed({});
  return false;
}

std::vector<std::wstring> GetBootableFileTypes()
{
  return {L".elf", L".dol", L".gcm", L".bin", L".iso", L".tgc", L".wbfs", L".ciso",
          L".gcz", L".wia", L".rvz", L".nfs", L".wad", L".dff", L".m3u",  L".json"};
}

std::vector<std::wstring> GetStateFileTypes()
{
  std::vector<std::wstring> types{L".sav"};
  for (u32 slot = 1; slot <= State::NUM_STATES; ++slot)
    types.push_back(fmt::format(L".s{:02d}", slot));
  return types;
}
}  // namespace DolphinUWP

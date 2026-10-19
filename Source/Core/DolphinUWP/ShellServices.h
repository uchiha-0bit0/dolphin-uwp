// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <UwpImGuiFrontend/Pickers.h>
#include <UwpImGuiFrontend/Types.h>

#include "Common/CommonTypes.h"
#include "Core/Boot/Boot.h"

struct ImFont;

namespace DolphinUWP
{
class BackgroundTask;
class GameLibrary;

struct BootRequest
{
  std::unique_ptr<BootParameters> parameters;
  std::string title;
  std::optional<UwpImGuiFrontend::ItemId> item;
};

// What shell pages need from the frontend host.
struct ShellServices
{
  GameLibrary* library = nullptr;
  BackgroundTask* task = nullptr;
  UwpImGuiFrontend::IPickerService* picker = nullptr;
  ImFont* text_font = nullptr;
  std::function<void(std::string)> notify;
  std::function<void(BootRequest)> boot;
  // Boots a file by path, using its library entry when it has one.
  std::function<void(std::string)> boot_file;
  std::function<UwpImGuiFrontend::TextureHandle(const u8* rgba, u32 width, u32 height)>
      create_texture;
  std::function<void(UwpImGuiFrontend::TextureHandle)> release_texture;

  void Notify(std::string message) const
  {
    if (notify)
      notify(std::move(message));
  }

  void BootFile(std::string path) const
  {
    if (boot_file)
      boot_file(std::move(path));
  }

  // The completion always runs once, with an empty path when nothing was picked.
  bool PickFile(std::wstring extension, std::wstring token,
                std::function<void(std::string path)> completed) const;
  bool PickFile(std::vector<std::wstring> extensions, std::wstring token,
                std::function<void(std::string path)> completed) const;
  bool PickFolder(std::wstring token, std::function<void(std::string path)> completed) const;
};

std::vector<std::wstring> GetBootableFileTypes();
std::vector<std::wstring> GetStateFileTypes();
}  // namespace DolphinUWP

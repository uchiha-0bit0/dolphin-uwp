// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>

#include "Common/CommonTypes.h"

namespace DolphinUWP
{
struct ShellServices;

// Wii NAND management. Every operation needs emulation to be stopped and runs as a background
// task; results are reported with dialogs.
namespace WiiSystemTools
{
struct SystemMenuInfo
{
  bool installed = false;
  bool vwii = false;
  std::string version;
};

SystemMenuInfo QuerySystemMenu();

using Refresh = std::function<void()>;

// An empty region updates the installed system menu's region.
void UpdateOnline(const ShellServices& services, std::string region, Refresh refresh);
void UpdateFromDisc(const ShellServices& services, std::string disc_path, Refresh refresh);
void InstallWad(const ShellServices& services, std::string path, Refresh refresh);
void UninstallTitle(const ShellServices& services, u64 title_id, std::string name, Refresh refresh);
void ImportSave(const ShellServices& services, std::string path);
void ExportSave(const ShellServices& services, u64 title_id, std::string directory);
void ExportAllSaves(const ShellServices& services, std::string directory);
void CheckNand(const ShellServices& services, Refresh refresh);
void ImportBootMiiBackup(const ShellServices& services, std::string path, Refresh refresh);
void ExtractCertificates(const ShellServices& services);
}  // namespace WiiSystemTools
}  // namespace DolphinUWP

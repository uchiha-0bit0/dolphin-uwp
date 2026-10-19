// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

class GBAHostInterface;

namespace HW::GBA
{
class Core;
}

namespace DolphinUWP::GBAScreens
{
constexpr size_t MAX_SCREENS = 4;

struct Status
{
  bool has_rom = false;
  bool has_ereader = false;
  bool connected = true;
  std::string title;
  int volume = 100;
  bool muted = false;
};

std::unique_ptr<GBAHostInterface> CreateHost(std::weak_ptr<HW::GBA::Core> core);

// Copies the screen when it has a frame newer than *frame. Returns false while the GBA is off.
bool CopyScreen(size_t index, u64* frame, u32* width, u32* height, std::vector<u32>* pixels);

std::optional<Status> GetStatus(size_t index);
bool IsRunning(size_t index);

// These act on the core of a running GBA, on the CPU thread.
void Reset(size_t index);
void SetConnected(size_t index, bool connected);
// Restarts the core with the ROM, or without one for an empty path.
void LoadROM(size_t index, std::string path);
void QueueEReaderCard(size_t index, std::string path);
void ImportState(size_t index, std::string path);
void ExportState(size_t index, std::string path);
void ImportSave(size_t index, std::string path);
void ExportSave(size_t index, std::string path);

// Percent.
void SetVolume(size_t index, int volume);
void SetMuted(size_t index, bool muted);
}  // namespace DolphinUWP::GBAScreens

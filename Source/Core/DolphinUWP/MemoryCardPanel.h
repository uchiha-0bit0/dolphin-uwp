// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <UwpImGuiFrontend/Types.h>

#include "Common/CommonTypes.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "DiscIO/Enums.h"

namespace Memcard
{
class GCMemcard;
struct Savefile;
}  // namespace Memcard

namespace DolphinUWP
{
struct ShellServices;

class MemoryCardPanel
{
public:
  explicit MemoryCardPanel(ExpansionInterface::Slot slot);
  ~MemoryCardPanel();

  MemoryCardPanel(const MemoryCardPanel&) = delete;
  MemoryCardPanel& operator=(const MemoryCardPanel&) = delete;

  void Open();
  void Close(const ShellServices& services);
  void Draw(const ShellServices& services, MemoryCardPanel& other);

private:
  struct Entry
  {
    u8 index = 0;
    std::string title;
    std::string comment;
    std::string file_name;
    std::string gci_path;
    u16 blocks = 0;
    std::vector<u8> banner;
    UwpImGuiFrontend::TextureHandle texture;
  };

  bool IsFolder() const;
  std::string SlotName() const;
  void Load(const ShellServices& services);
  void ReleaseTextures(const ShellServices& services);
  std::optional<Memcard::Savefile> GetSavefile(const Entry& entry) const;
  bool Import(const Memcard::Savefile& save, std::string* error);
  void ImportFile(const ShellServices& services, const std::string& path);
  void Export(const ShellServices& services, size_t entry);
  void CopyToOther(const ShellServices& services, size_t entry, MemoryCardPanel& other);
  void Delete(const ShellServices& services, size_t entry);
  void FixChecksums(const ShellServices& services);
  void CreateCard(const ShellServices& services);
  void DrawActions(const ShellServices& services, MemoryCardPanel& other);

  ExpansionInterface::Slot m_slot;
  ExpansionInterface::EXIDeviceType m_device = ExpansionInterface::EXIDeviceType::None;
  DiscIO::Region m_region = DiscIO::Region::NTSC_U;
  std::string m_path;
  std::unique_ptr<Memcard::GCMemcard> m_card;
  std::string m_status;
  std::vector<Entry> m_entries;
  std::optional<size_t> m_action_entry;
  bool m_loaded = false;
};
}  // namespace DolphinUWP

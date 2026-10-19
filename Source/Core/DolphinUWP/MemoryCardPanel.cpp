// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/MemoryCardPanel.h"

#include <array>
#include <utility>
#include <variant>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/Views.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/ShellWidgets.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;
using ExpansionInterface::EXIDeviceType;

constexpr u32 BANNER_WIDTH = 96;
constexpr u32 BANNER_HEIGHT = 32;

struct RegionChoice
{
  DiscIO::Region region;
  const char* label;
};

constexpr std::array<RegionChoice, 3> REGIONS{{
    {DiscIO::Region::NTSC_U, "USA"},
    {DiscIO::Region::PAL, "Europe"},
    {DiscIO::Region::NTSC_J, "Japan"},
}};

u16 GetConfiguredSizeMbits()
{
  const int size_override = Config::Get(Config::MAIN_MEMORY_CARD_SIZE);
  if (size_override >= 0 && size_override <= 4)
    return Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_override;
  return Memcard::MBIT_SIZE_MEMORY_CARD_2043;
}

std::vector<u8> ToRGBA(const std::vector<u32>& argb)
{
  std::vector<u8> rgba;
  rgba.reserve(argb.size() * 4);
  for (const u32 pixel : argb)
  {
    rgba.push_back(static_cast<u8>(pixel >> 16));
    rgba.push_back(static_cast<u8>(pixel >> 8));
    rgba.push_back(static_cast<u8>(pixel));
    rgba.push_back(static_cast<u8>(pixel >> 24));
  }
  return rgba;
}

std::string DescribeImportResult(Memcard::GCMemcardImportFileRetVal result)
{
  switch (result)
  {
  case Memcard::GCMemcardImportFileRetVal::OUTOFDIRENTRIES:
    return "The memory card has no free file entries.";
  case Memcard::GCMemcardImportFileRetVal::OUTOFBLOCKS:
    return "The memory card does not have enough free blocks.";
  case Memcard::GCMemcardImportFileRetVal::TITLEPRESENT:
    return "A save for this title already exists on the memory card.";
  default:
    return "The save could not be written to the memory card.";
  }
}
}  // namespace

MemoryCardPanel::MemoryCardPanel(ExpansionInterface::Slot slot) : m_slot(slot)
{
}

MemoryCardPanel::~MemoryCardPanel() = default;

bool MemoryCardPanel::IsFolder() const
{
  return m_device == EXIDeviceType::MemoryCardFolder;
}

std::string MemoryCardPanel::SlotName() const
{
  return m_slot == ExpansionInterface::Slot::A ? "A" : "B";
}

void MemoryCardPanel::Open()
{
  m_loaded = false;
  m_action_entry.reset();

  const DiscIO::Region fallback =
      Config::ToGameCubeRegion(Config::Get(Config::MAIN_FALLBACK_REGION));
  m_region = fallback;
  const EXIDeviceType device = Config::Get(Config::GetInfoForEXIDevice(m_slot));
  for (const RegionChoice& choice : REGIONS)
  {
    const std::string path =
        device == EXIDeviceType::MemoryCardFolder ?
            Config::GetGCIFolderPath(m_slot, choice.region) :
            Config::GetMemcardPath(m_slot, choice.region, GetConfiguredSizeMbits());
    if (File::Exists(path))
    {
      m_region = choice.region;
      break;
    }
  }
}

void MemoryCardPanel::Close(const ShellServices& services)
{
  ReleaseTextures(services);
  m_entries.clear();
  m_card.reset();
  m_action_entry.reset();
  m_loaded = false;
}

void MemoryCardPanel::ReleaseTextures(const ShellServices& services)
{
  for (Entry& entry : m_entries)
  {
    if (entry.texture && services.release_texture)
      services.release_texture(entry.texture);
    entry.texture = {};
  }
}

void MemoryCardPanel::Load(const ShellServices& services)
{
  ReleaseTextures(services);
  m_entries.clear();
  m_card.reset();
  m_status.clear();
  m_loaded = true;
  m_device = Config::Get(Config::GetInfoForEXIDevice(m_slot));

  const auto read_entries = [this](const Memcard::GCMemcard& card) {
    for (u8 i = 0; i < card.GetNumFiles(); ++i)
    {
      const u8 index = card.GetFileIndex(i);
      Entry entry;
      entry.index = index;
      if (const auto comments = card.GetSaveComments(index))
      {
        entry.title = comments->first;
        entry.comment = comments->second;
      }
      if (const auto dentry = card.GetDEntry(index))
        entry.file_name = Memcard::GenerateFilename(*dentry);
      if (entry.title.empty())
        entry.title = entry.file_name;
      entry.blocks = card.DEntry_BlockCount(index);
      if (const auto banner = card.ReadBannerRGBA8(index))
        entry.banner = ToRGBA(*banner);
      m_entries.push_back(std::move(entry));
    }
  };

  if (m_device == EXIDeviceType::MemoryCard)
  {
    m_path = Config::GetMemcardPath(m_slot, m_region, GetConfiguredSizeMbits());
    if (!File::Exists(m_path))
    {
      m_status = "This memory card has not been created yet.";
      return;
    }
    auto [error, card] = Memcard::GCMemcard::Open(m_path);
    if (error.HasCriticalErrors() || !card || !card->IsValid())
    {
      m_status = "The memory card could not be read.";
      return;
    }
    m_card = std::make_unique<Memcard::GCMemcard>(std::move(*card));
    read_entries(*m_card);
    m_status = fmt::format("{} free blocks, {} of {} file entries free", m_card->GetFreeBlocks(),
                           Memcard::DIRLEN - m_card->GetNumFiles(), Memcard::DIRLEN);
    return;
  }

  if (m_device != EXIDeviceType::MemoryCardFolder)
    return;

  m_path = Config::GetGCIFolderPath(m_slot, m_region);
  if (!File::IsDirectory(m_path))
  {
    m_status = "This folder has not been created yet. It is created when a game saves.";
    return;
  }

  // Banners and comments are decoded by a scratch card holding the folder's saves.
  const std::string scratch = File::GetUserPath(D_CACHE_IDX) + "MemcardPreview.raw";
  auto preview = Memcard::GCMemcard::Create(scratch, {}, Memcard::MBIT_SIZE_MEMORY_CARD_2043,
                                            m_region == DiscIO::Region::NTSC_J, 0, 0, 0);
  File::Delete(scratch);

  std::vector<std::pair<std::string, Memcard::Savefile>> saves;
  for (const std::string& path : Common::DoFileSearch(m_path, ".gci"))
  {
    auto result = Memcard::ReadSavefile(path);
    if (auto* save = std::get_if<Memcard::Savefile>(&result))
      saves.emplace_back(path, std::move(*save));
  }

  for (auto& [path, save] : saves)
  {
    Entry entry;
    entry.gci_path = path;
    entry.file_name = Memcard::GenerateFilename(save.dir_entry);
    entry.blocks = static_cast<u16>(save.blocks.size());
    if (preview && preview->ImportFile(save) == Memcard::GCMemcardImportFileRetVal::SUCCESS)
    {
      if (const auto index = preview->TitlePresent(save.dir_entry))
      {
        if (const auto comments = preview->GetSaveComments(*index))
        {
          entry.title = comments->first;
          entry.comment = comments->second;
        }
        if (const auto banner = preview->ReadBannerRGBA8(*index))
          entry.banner = ToRGBA(*banner);
      }
    }
    if (entry.title.empty())
      SplitPath(path, nullptr, &entry.title, nullptr);
    m_entries.push_back(std::move(entry));
  }
  m_status = fmt::format("{} save(s) in this folder", m_entries.size());
}

std::optional<Memcard::Savefile> MemoryCardPanel::GetSavefile(const Entry& entry) const
{
  if (IsFolder())
  {
    auto result = Memcard::ReadSavefile(entry.gci_path);
    if (auto* save = std::get_if<Memcard::Savefile>(&result))
      return std::move(*save);
    return std::nullopt;
  }
  if (!m_card)
    return std::nullopt;
  const std::array<u8, 1> indices{entry.index};
  std::vector<Memcard::Savefile> saves = Memcard::GetSavefiles(*m_card, indices);
  if (saves.empty())
    return std::nullopt;
  return std::move(saves.front());
}

bool MemoryCardPanel::Import(const Memcard::Savefile& save, std::string* error)
{
  if (IsFolder())
  {
    if (m_path.empty())
    {
      *error = "This slot has no GCI folder.";
      return false;
    }
    File::CreateFullPath(m_path + DIR_SEP);
    const std::string path =
        fmt::format("{}{}{}.gci", m_path, DIR_SEP, Memcard::GenerateFilename(save.dir_entry));
    if (File::Exists(path))
    {
      *error = "A save with the same name already exists in the GCI folder.";
      return false;
    }
    if (!Memcard::WriteSavefile(path, save, Memcard::SavefileFormat::GCI))
    {
      *error = "The save could not be written to the GCI folder.";
      return false;
    }
    m_loaded = false;
    return true;
  }

  if (!m_card)
  {
    *error = "This slot has no readable memory card.";
    return false;
  }
  const Memcard::GCMemcardImportFileRetVal result = m_card->ImportFile(save);
  if (result != Memcard::GCMemcardImportFileRetVal::SUCCESS)
  {
    *error = DescribeImportResult(result);
    return false;
  }
  if (!m_card->Save())
  {
    *error = "The memory card could not be saved.";
    m_loaded = false;
    return false;
  }
  m_loaded = false;
  return true;
}

void MemoryCardPanel::ImportFile(const ShellServices& services, const std::string& path)
{
  auto result = Memcard::ReadSavefile(path);
  const auto* save = std::get_if<Memcard::Savefile>(&result);
  if (!save)
  {
    services.Notify("The selected file is not a valid GameCube save");
    return;
  }
  std::string error;
  if (Import(*save, &error))
    services.Notify(fmt::format("Imported the save to memory card {}", SlotName()));
  else
    PromptService::GetInstance().Inform("Import save", error, Common::MsgType::Warning);
}

void MemoryCardPanel::Export(const ShellServices& services, size_t index)
{
  const auto save = GetSavefile(m_entries[index]);
  if (!save)
  {
    services.Notify("The save could not be read");
    return;
  }
  services.PickFolder(L"dolphin-memcard-export", [&services, save = *save](std::string directory) {
    if (directory.empty())
      return;
    const std::string path =
        fmt::format("{}{}{}.gci", directory, DIR_SEP, Memcard::GenerateFilename(save.dir_entry));
    if (Memcard::WriteSavefile(path, save, Memcard::SavefileFormat::GCI))
      services.Notify("Exported the save");
    else
      services.Notify("The save could not be exported");
  });
}

void MemoryCardPanel::CopyToOther(const ShellServices& services, size_t index,
                                  MemoryCardPanel& other)
{
  const auto save = GetSavefile(m_entries[index]);
  if (!save)
  {
    services.Notify("The save could not be read");
    return;
  }
  if (!other.m_loaded)
    other.Load(services);
  std::string error;
  if (other.Import(*save, &error))
    services.Notify(fmt::format("Copied the save to memory card {}", other.SlotName()));
  else
    PromptService::GetInstance().Inform("Copy save", error, Common::MsgType::Warning);
}

void MemoryCardPanel::Delete(const ShellServices& services, size_t index)
{
  const Entry entry = m_entries[index];
  PromptService::GetInstance().Confirm(
      "Delete save", fmt::format("Delete \"{}\" from memory card {}?", entry.title, SlotName()),
      [this, &services, entry](bool confirmed) {
        if (!confirmed)
          return;
        bool deleted = false;
        if (IsFolder())
        {
          deleted = File::Delete(entry.gci_path);
        }
        else if (m_card)
        {
          deleted =
              m_card->RemoveFile(entry.index) == Memcard::GCMemcardRemoveFileRetVal::SUCCESS &&
              m_card->Save();
        }
        m_loaded = false;
        services.Notify(deleted ? "Deleted the save" : "The save could not be deleted");
      });
}

void MemoryCardPanel::FixChecksums(const ShellServices& services)
{
  if (!m_card)
    return;
  const bool fixed = m_card->FixChecksums() && m_card->Save();
  m_loaded = false;
  services.Notify(fixed ? "Fixed the memory card checksums" : "The checksums could not be fixed");
}

void MemoryCardPanel::CreateCard(const ShellServices& services)
{
  File::CreateFullPath(m_path);
  const u64 format_time =
      Common::Timer::GetLocalTimeSinceJan1970() - ExpansionInterface::CEXIIPL::GC_EPOCH;
  auto card = Memcard::GCMemcard::Create(m_path, {}, GetConfiguredSizeMbits(),
                                         m_region == DiscIO::Region::NTSC_J, 0, 0, format_time);
  m_loaded = false;
  services.Notify(card && card->Save() ? "Created the memory card" :
                                         "The memory card could not be created");
}

void MemoryCardPanel::Draw(const ShellServices& services, MemoryCardPanel& other)
{
  if (!m_loaded)
    Load(services);

  Frontend::SectionTitle(fmt::format("Memory Card {}", SlotName()).c_str());
  if (m_device != EXIDeviceType::MemoryCard && m_device != EXIDeviceType::MemoryCardFolder)
  {
    ShellWidgets::HintText(fmt::format("Slot {} has no memory card. Choose a memory card or a GCI "
                                       "folder for this slot in Settings, GameCube.",
                                       SlotName()));
    return;
  }

  std::vector<Frontend::ChoiceItem> regions;
  size_t selected = 0;
  for (size_t i = 0; i < REGIONS.size(); ++i)
  {
    regions.push_back({REGIONS[i].label, REGIONS[i].label});
    if (REGIONS[i].region == m_region)
      selected = i;
  }
  if (Frontend::ShellCombo("Region", &selected, regions) && selected < REGIONS.size())
  {
    m_region = REGIONS[selected].region;
    Load(services);
  }
  ShellWidgets::HintText(m_path);
  ShellWidgets::HintText(m_status);

  const bool missing_card = !IsFolder() && !m_card && !File::Exists(m_path);
  if (missing_card && Frontend::ShellButton("Create memory card"))
    CreateCard(services);
  if (!missing_card && Frontend::ShellButton("Import save"))
  {
    services.PickFile(std::vector<std::wstring>{L".gci", L".gcs", L".sav"},
                      L"dolphin-memcard-import", [this, &services](std::string path) {
                        if (!path.empty())
                          ImportFile(services, path);
                      });
  }
  if (m_card && Frontend::ShellButton("Fix checksums"))
    FixChecksums(services);

  Frontend::SectionTitle("Saves");
  if (m_entries.empty())
    ShellWidgets::HintText("There are no saves.");
  for (size_t i = 0; i < m_entries.size(); ++i)
  {
    Entry& entry = m_entries[i];
    if (!entry.texture && !entry.banner.empty() && services.create_texture)
      entry.texture = services.create_texture(entry.banner.data(), BANNER_WIDTH, BANNER_HEIGHT);
    ImGui::PushID(static_cast<int>(i));
    if (ShellWidgets::DetailRow(
            "save", entry.texture, {BANNER_WIDTH, BANNER_HEIGHT}, entry.title, entry.comment,
            fmt::format("{} block{}", entry.blocks, entry.blocks == 1 ? "" : "s")))
    {
      m_action_entry = i;
    }
    ImGui::PopID();
  }
  DrawActions(services, other);
}

void MemoryCardPanel::DrawActions(const ShellServices& services, MemoryCardPanel& other)
{
  const std::string popup = fmt::format("memory-card-actions-{}", SlotName());
  if (!m_action_entry || *m_action_entry >= m_entries.size())
  {
    m_action_entry.reset();
    return;
  }
  if (!ImGui::IsPopupOpen(popup.c_str()))
    ImGui::OpenPopup(popup.c_str());

  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  const Frontend::ShellLayoutContext context =
      Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  const bool other_available =
      other.m_device == EXIDeviceType::MemoryCardFolder ||
      (other.m_device == EXIDeviceType::MemoryCard && (!other.m_loaded || other.m_card));

  Frontend::SelectionDialogModel model;
  model.popupId = popup;
  model.title = m_entries[*m_action_entry].title;
  model.entries = {
      {"export", "Export to a folder"},
      {"copy", fmt::format("Copy to memory card {}", other.SlotName()), {}, other_available},
      {"delete", "Delete"},
  };
  model.closeLabel = "Cancel";
  const Frontend::SelectionDialogResult result =
      Frontend::DrawSelectionDialog(model, context, Frontend::CurrentWidgetTheme());
  if (!result.selected && !result.closed)
    return;

  const size_t entry = *std::exchange(m_action_entry, std::nullopt);
  if (!result.selected)
    return;
  switch (*result.selected)
  {
  case 0:
    Export(services, entry);
    break;
  case 1:
    CopyToOther(services, entry, other);
    break;
  case 2:
    Delete(services, entry);
    break;
  default:
    break;
  }
}
}  // namespace DolphinUWP

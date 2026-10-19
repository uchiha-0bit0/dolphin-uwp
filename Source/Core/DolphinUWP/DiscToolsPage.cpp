// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/DiscToolsPage.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <imgui.h>

#include <UwpImGuiFrontend/Widgets.h>

#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/ScrubbedBlob.h"
#include "DiscIO/Volume.h"
#include "DolphinUWP/BackgroundTask.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/ShellWidgets.h"
#include "UICommon/GameFile.h"
#include "UICommon/UICommon.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

struct FormatChoice
{
  DiscIO::BlobType type;
  const char* label;
  const char* extension;
};

constexpr std::array<FormatChoice, 4> FORMATS{{
    {DiscIO::BlobType::PLAIN, "ISO (uncompressed)", ".iso"},
    {DiscIO::BlobType::GCZ, "GCZ", ".gcz"},
    {DiscIO::BlobType::WIA, "WIA", ".wia"},
    {DiscIO::BlobType::RVZ, "RVZ (recommended)", ".rvz"},
}};

struct CompressionChoice
{
  DiscIO::WIARVZCompressionType type;
  const char* label;
};

std::vector<CompressionChoice> GetCompressionChoices(DiscIO::BlobType format)
{
  using DiscIO::WIARVZCompressionType;
  switch (format)
  {
  case DiscIO::BlobType::GCZ:
    return {{WIARVZCompressionType::None, "Deflate"}};
  case DiscIO::BlobType::WIA:
    return {{WIARVZCompressionType::None, "No compression"},
            {WIARVZCompressionType::Purge, "Purge"},
            {WIARVZCompressionType::Bzip2, "bzip2 (slow)"},
            {WIARVZCompressionType::LZMA, "LZMA (slow)"},
            {WIARVZCompressionType::LZMA2, "LZMA2 (slow)"}};
  case DiscIO::BlobType::RVZ:
    return {{WIARVZCompressionType::None, "No compression"},
            {WIARVZCompressionType::Bzip2, "bzip2 (slow)"},
            {WIARVZCompressionType::LZMA, "LZMA (slow)"},
            {WIARVZCompressionType::LZMA2, "LZMA2 (slow)"},
            {WIARVZCompressionType::Zstd, "Zstandard (recommended)"}};
  default:
    return {};
  }
}

std::vector<int> GetBlockSizes(DiscIO::BlobType format, u64 volume_size)
{
  std::vector<int> sizes;
  switch (format)
  {
  case DiscIO::BlobType::GCZ:
    for (int size = DiscIO::PREFERRED_MIN_BLOCK_SIZE; size <= DiscIO::PREFERRED_MAX_BLOCK_SIZE;
         size *= 2)
    {
      if (DiscIO::IsGCZBlockSizeLegacyCompatible(size, volume_size))
        sizes.push_back(size);
    }
    if (sizes.empty())
      sizes.push_back(DiscIO::GCZ_FALLBACK_BLOCK_SIZE);
    break;
  case DiscIO::BlobType::WIA:
    sizes.push_back(DiscIO::WIA_MIN_BLOCK_SIZE);
    break;
  case DiscIO::BlobType::RVZ:
    for (int size = DiscIO::PREFERRED_MIN_BLOCK_SIZE; size <= DiscIO::PREFERRED_MAX_BLOCK_SIZE;
         size *= 2)
    {
      sizes.push_back(size);
    }
    break;
  default:
    break;
  }
  return sizes;
}

int GetDefaultBlockSize(const std::vector<int>& sizes)
{
  int selected = sizes.empty() ? 0 : sizes.front();
  for (const int size : sizes)
  {
    if (size <= DiscIO::GCZ_RVZ_PREFERRED_BLOCK_SIZE)
      selected = size;
  }
  return selected;
}

std::string FormatHash(const std::vector<u8>& hash)
{
  return fmt::format("{:02x}", fmt::join(hash, ""));
}

const char* GetSeverityLabel(DiscIO::VolumeVerifier::Severity severity)
{
  switch (severity)
  {
  case DiscIO::VolumeVerifier::Severity::Low:
    return "Low";
  case DiscIO::VolumeVerifier::Severity::Medium:
    return "Medium";
  case DiscIO::VolumeVerifier::Severity::High:
    return "High";
  default:
    return "";
  }
}
}  // namespace

void DiscToolsPage::Open(std::shared_ptr<const UICommon::GameFile> game, std::string title)
{
  m_game = std::move(game);
  m_title = std::move(title);
  m_view = {};
  m_verify_result.reset();
  const DiscIO::Hashes<bool> hashes = DiscIO::VolumeVerifier::GetDefaultHashesToCalculate();
  m_calculate_crc32 = hashes.crc32;
  m_calculate_md5 = hashes.md5;
  m_calculate_sha1 = hashes.sha1;
  m_verify_redump = true;

  m_format = DiscIO::BlobType::RVZ;
  m_block_size = GetDefaultBlockSize(GetBlockSizes(m_format, m_game->GetVolumeSize()));
  m_compression = DiscIO::WIARVZCompressionType::Zstd;
  m_compression_level = 5;
  m_remove_junk = false;
  m_delete_original = false;
}

void DiscToolsPage::Close()
{
  m_game.reset();
  m_verify_result.reset();
}

bool DiscToolsPage::Draw(float controller_scroll)
{
  if (!m_game)
    return true;

  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = controller_scroll;
  model.pages = {
      {"verify", "Verify", true, [this] { DrawVerify(); }},
      {"convert", "Convert", true, [this] { DrawConvert(); }},
  };
  return m_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
}

void DiscToolsPage::DrawVerify()
{
  Frontend::SectionTitle("Verify the disc image");
  (void)Frontend::ShellCheckbox("Calculate CRC32", &m_calculate_crc32);
  (void)Frontend::ShellCheckbox("Calculate MD5", &m_calculate_md5);
  (void)Frontend::ShellCheckbox("Calculate SHA-1", &m_calculate_sha1);
  const bool can_verify_redump = m_calculate_md5 || m_calculate_sha1;
  if (can_verify_redump)
    (void)Frontend::ShellCheckbox("Compare with Redump.org", &m_verify_redump);
  else
    ShellWidgets::HintText("Comparing with Redump.org needs MD5 or SHA-1.");
  if (Frontend::ShellButton("Verify"))
    Verify();

  if (!m_verify_result)
    return;

  Frontend::SectionTitle("Result");
  ShellWidgets::HintText(m_verify_result->summary_text);
  for (size_t i = 0; i < m_verify_result->problems.size(); ++i)
  {
    const DiscIO::VolumeVerifier::Problem& problem = m_verify_result->problems[i];
    ShellWidgets::HintText(
        fmt::format("{} severity: {}", GetSeverityLabel(problem.severity), problem.text));
  }

  const DiscIO::Hashes<std::vector<u8>>& hashes = m_verify_result->hashes;
  if (!hashes.crc32.empty())
    ShellWidgets::ValueRow("crc32", "CRC32", FormatHash(hashes.crc32));
  if (!hashes.md5.empty())
    ShellWidgets::ValueRow("md5", "MD5", FormatHash(hashes.md5));
  if (!hashes.sha1.empty())
    ShellWidgets::ValueRow("sha1", "SHA-1", FormatHash(hashes.sha1));
  if (!m_verify_result->redump.message.empty())
    ShellWidgets::HintText(fmt::format("Redump.org: {}", m_verify_result->redump.message));
}

void DiscToolsPage::Verify()
{
  const bool redump = m_verify_redump && (m_calculate_md5 || m_calculate_sha1);
  const DiscIO::Hashes<bool> hashes{m_calculate_crc32, m_calculate_md5, m_calculate_sha1};
  m_verify_result.reset();
  m_services.task->Start(
      fmt::format("Verifying {}", m_title), true,
      [this, path = m_game->GetFilePath(), redump,
       hashes](BackgroundTask::Context& context) -> BackgroundTask::Completion {
        const std::unique_ptr<DiscIO::Volume> volume = DiscIO::CreateVolume(path);
        if (!volume)
        {
          return [] {
            PromptService::GetInstance().Inform("Verify", "The disc image could not be opened.",
                                                Common::MsgType::Warning);
          };
        }

        DiscIO::VolumeVerifier verifier(*volume, redump, hashes);
        verifier.Start();
        const u64 total = verifier.GetTotalBytes();
        while (verifier.GetBytesProcessed() != total)
        {
          if (context.IsCancelled())
            return {};
          context.SetProgress(static_cast<float>(verifier.GetBytesProcessed()) /
                              static_cast<float>(total));
          verifier.Process();
        }
        verifier.Finish();
        return [this, result = verifier.GetResult()] { m_verify_result = result; };
      });
}

void DiscToolsPage::DrawConvert()
{
  Frontend::SectionTitle("Convert the disc image");
  std::vector<Frontend::ChoiceItem> formats;
  size_t format_index = 0;
  for (size_t i = 0; i < FORMATS.size(); ++i)
  {
    if (FORMATS[i].type == m_format)
      format_index = i;
    formats.push_back({FORMATS[i].extension, FORMATS[i].label});
  }
  if (Frontend::ShellCombo("Format", &format_index, formats))
  {
    m_format = FORMATS[format_index].type;
    m_block_size = GetDefaultBlockSize(GetBlockSizes(m_format, m_game->GetVolumeSize()));
    const std::vector<CompressionChoice> compressions = GetCompressionChoices(m_format);
    m_compression =
        compressions.empty() ? DiscIO::WIARVZCompressionType::None : compressions.back().type;
    m_compression_level = 5;
  }

  const std::vector<int> block_sizes = GetBlockSizes(m_format, m_game->GetVolumeSize());
  if (block_sizes.size() > 1)
  {
    std::vector<Frontend::ChoiceItem> items;
    size_t selected = 0;
    for (size_t i = 0; i < block_sizes.size(); ++i)
    {
      if (block_sizes[i] == m_block_size)
        selected = i;
      items.push_back({std::to_string(block_sizes[i]), UICommon::FormatSize(block_sizes[i], 0)});
    }
    if (Frontend::ShellCombo("Block size", &selected, items))
      m_block_size = block_sizes[selected];
  }

  const std::vector<CompressionChoice> compressions = GetCompressionChoices(m_format);
  if (compressions.size() > 1)
  {
    std::vector<Frontend::ChoiceItem> items;
    size_t selected = 0;
    for (size_t i = 0; i < compressions.size(); ++i)
    {
      if (compressions[i].type == m_compression)
        selected = i;
      items.push_back({std::to_string(i), compressions[i].label});
    }
    if (Frontend::ShellCombo("Compression", &selected, items))
    {
      m_compression = compressions[selected].type;
      m_compression_level = 5;
    }
  }

  if (m_format == DiscIO::BlobType::WIA || m_format == DiscIO::BlobType::RVZ)
  {
    const auto [minimum, maximum] = DiscIO::GetAllowedCompressionLevels(m_compression, true);
    if (maximum > minimum)
    {
      m_compression_level = std::clamp(m_compression_level, minimum, maximum);
      (void)Frontend::ShellIntegerStepper("Compression level", &m_compression_level, minimum,
                                          maximum);
    }
  }

  if (m_format != DiscIO::BlobType::RVZ && !m_game->IsDatelDisc())
    (void)Frontend::ShellCheckbox("Remove junk data (irreversible)", &m_remove_junk);
  (void)Frontend::ShellCheckbox("Delete the original file after converting", &m_delete_original);
  ShellWidgets::HintText(
      "The converted file is saved next to the original, with the extension of its format.");

  if (Frontend::ShellButton("Convert"))
    Convert();
}

void DiscToolsPage::Convert()
{
  const bool remove_junk =
      m_remove_junk && m_format != DiscIO::BlobType::RVZ && !m_game->IsDatelDisc();
  std::vector<std::string> warnings;
  if (remove_junk && m_format == DiscIO::BlobType::PLAIN)
  {
    warnings.emplace_back("Removing junk data does not save any space when converting to ISO "
                          "(unless you package the ISO file in a compressed file format such as "
                          "ZIP afterwards).");
  }
  if (!remove_junk && m_format == DiscIO::BlobType::GCZ &&
      m_game->GetPlatform() == DiscIO::Platform::WiiDisc && !m_game->IsDatelDisc())
  {
    warnings.emplace_back("Converting Wii disc images to GCZ without removing junk data does "
                          "not save any noticeable amount of space compared to converting to "
                          "ISO.");
  }
  if (m_game->IsNKit())
  {
    warnings.emplace_back("Dolphin can't convert NKit files to non-NKit files. Converting an "
                          "NKit file results in another NKit file.");
  }
  if (m_delete_original)
    warnings.emplace_back("The original file is deleted when the conversion succeeds.");

  if (warnings.empty())
  {
    StartConversion();
    return;
  }
  PromptService::GetInstance().Confirm(
      "Convert", fmt::format("{}\n\nDo you want to continue?", fmt::join(warnings, "\n\n")),
      [this](bool confirmed) {
        if (confirmed)
          StartConversion();
      });
}

void DiscToolsPage::StartConversion()
{
  if (!m_game)
    return;

  const std::string source = m_game->GetFilePath();
  const auto format = std::ranges::find(FORMATS, m_format, &FormatChoice::type);
  const std::filesystem::path source_path = StringToPath(source);
  const std::string destination =
      PathToString(source_path.parent_path() / source_path.stem()) + format->extension;
  if (destination == source || File::Exists(destination))
  {
    PromptService::GetInstance().Inform(
        "Convert", fmt::format("{} already exists. Move or rename it and try again.", destination),
        Common::MsgType::Warning);
    return;
  }

  const bool remove_junk =
      m_remove_junk && m_format != DiscIO::BlobType::RVZ && !m_game->IsDatelDisc();
  const bool wii = m_game->GetPlatform() == DiscIO::Platform::WiiDisc;
  m_services.task->Start(
      fmt::format("Converting {}", m_title), true,
      [this, source, destination, remove_junk, wii, format = m_format, block_size = m_block_size,
       compression = m_compression, level = m_compression_level,
       delete_original =
           m_delete_original](BackgroundTask::Context& context) -> BackgroundTask::Completion {
        std::unique_ptr<DiscIO::BlobReader> reader;
        if (remove_junk)
          reader = DiscIO::ScrubbedBlob::Create(source);
        else
          reader = DiscIO::CreateBlobReader(source);
        if (!reader)
        {
          return [] {
            PromptService::GetInstance().Inform("Convert", "The disc image could not be opened.",
                                                Common::MsgType::Warning);
          };
        }

        const auto callback = [&context](const std::string& text, float percent) {
          context.SetProgress(percent, text);
          return !context.IsCancelled();
        };
        bool converted = false;
        switch (format)
        {
        case DiscIO::BlobType::PLAIN:
          converted = DiscIO::ConvertToPlain(reader.get(), source, destination, callback);
          break;
        case DiscIO::BlobType::GCZ:
          converted = DiscIO::ConvertToGCZ(reader.get(), source, destination, wii ? 1 : 0,
                                           block_size, callback);
          break;
        default:
          converted = DiscIO::ConvertToWIAOrRVZ(reader.get(), source, destination,
                                                format == DiscIO::BlobType::RVZ, compression, level,
                                                block_size, callback);
          break;
        }
        reader.reset();

        const bool cancelled = context.IsCancelled();
        if (!converted)
          File::Delete(destination);
        else if (delete_original)
          File::Delete(source);
        return [this, converted, cancelled, destination] {
          if (converted)
          {
            m_services.library->Refresh();
            m_services.Notify(fmt::format("Saved {}", destination));
          }
          else if (!cancelled)
          {
            PromptService::GetInstance().Inform("Convert", "The conversion failed.",
                                                Common::MsgType::Warning);
          }
        };
      });
}
}  // namespace DolphinUWP

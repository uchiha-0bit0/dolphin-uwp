// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/WiiSystemTools.h"

#include <future>
#include <memory>
#include <utility>

#include <fmt/format.h>

#include "Common/Align.h"
#include "Common/StringUtil.h"
#include "Core/CommonTitles.h"
#include "Core/ConfigManager.h"
#include "Core/HW/WiiSave.h"
#include "Core/IOS/ES/ES.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/IOS.h"
#include "Core/TitleDatabase.h"
#include "Core/WiiUtils.h"
#include "DiscIO/Enums.h"
#include "DiscIO/NANDImporter.h"
#include "DiscIO/WiiSaveBanner.h"
#include "DolphinUWP/BackgroundTask.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"

namespace DolphinUWP::WiiSystemTools
{
namespace
{
void Inform(std::string caption, std::string text,
            Common::MsgType style = Common::MsgType::Information)
{
  PromptService::GetInstance().Inform(std::move(caption), std::move(text), style);
}

bool Ask(std::string caption, std::string text)
{
  return PromptService::GetInstance().Ask(std::move(caption), std::move(text),
                                          Common::MsgType::Question, false);
}

void ReportUpdate(WiiUtils::UpdateResult result)
{
  using WiiUtils::UpdateResult;
  switch (result)
  {
  case UpdateResult::Succeeded:
    DiscIO::NANDImporter().ExtractCertificates();
    Inform("Update completed", "The emulated Wii console has been updated.");
    break;
  case UpdateResult::AlreadyUpToDate:
    DiscIO::NANDImporter().ExtractCertificates();
    Inform("Update completed", "The emulated Wii console is already up-to-date.");
    break;
  case UpdateResult::ServerFailed:
    Inform("Update failed",
           "Could not download update information from Nintendo. Please check your Internet "
           "connection and try again.",
           Common::MsgType::Critical);
    break;
  case UpdateResult::DownloadFailed:
    Inform("Update failed",
           "Could not download update files from Nintendo. Please check your Internet "
           "connection and try again.",
           Common::MsgType::Critical);
    break;
  case UpdateResult::ImportFailed:
    Inform("Update failed",
           "Could not install an update to the Wii system memory. Please refer to logs for more "
           "information.",
           Common::MsgType::Critical);
    break;
  case UpdateResult::Cancelled:
    Inform("Update cancelled",
           "The update has been cancelled. It is strongly recommended to finish it in order to "
           "avoid inconsistent system software versions.",
           Common::MsgType::Warning);
    break;
  case UpdateResult::RegionMismatch:
    Inform("Update failed",
           "The game's region does not match your console's. To avoid issues with the system "
           "menu, it is not possible to update the emulated console using this disc.",
           Common::MsgType::Critical);
    break;
  case UpdateResult::MissingUpdatePartition:
  case UpdateResult::DiscReadFailed:
    Inform("Update failed", "The game disc does not contain any usable update information.",
           Common::MsgType::Critical);
    break;
  default:
    break;
  }
}

WiiUtils::UpdateCallback MakeUpdateProgress(BackgroundTask::Context& context)
{
  return [&context](size_t processed, size_t total, u64 title_id) {
    context.SetProgress(total == 0 ? -1.0f : static_cast<float>(processed) / total,
                        fmt::format("Updating title {:016x} ({} of {})", title_id,
                                    std::min(processed + 1, total), total));
    return !context.IsCancelled();
  };
}

std::string DescribeTitle(const Core::TitleDatabase& database, u64 title_id)
{
  std::string text = fmt::format("{:016x}", title_id);
  const std::string& name =
      database.GetChannelName(title_id, SConfig::GetInstance().GetCurrentLanguage(true));
  if (!name.empty())
    return text + " - " + name;

  const DiscIO::WiiSaveBanner banner(title_id);
  if (banner.IsValid())
    text += " - " + banner.GetName();
  return text;
}

std::string DescribeClusters(std::string_view area, u64 used, u64 allowed)
{
  using namespace IOS::HLE::FS;
  return fmt::format("The {} part of your NAND contains {} blocks ({} KiB) of data, out of an "
                     "allowed maximum of {} blocks ({} KiB).",
                     area, Common::AlignUp(used, CLUSTERS_PER_BLOCK) / CLUSTERS_PER_BLOCK,
                     used * CLUSTER_SIZE / 1024, allowed / CLUSTERS_PER_BLOCK,
                     allowed * CLUSTER_SIZE / 1024);
}
}  // namespace

SystemMenuInfo QuerySystemMenu()
{
  IOS::HLE::Kernel ios;
  const auto tmd = ios.GetESCore().FindInstalledTMD(Titles::SYSTEM_MENU);
  if (!tmd.IsValid())
    return {};
  return {true, tmd.IsvWii(), DiscIO::GetSysMenuVersionString(tmd.GetTitleVersion(), tmd.IsvWii())};
}

void UpdateOnline(const ShellServices& services, std::string region, Refresh refresh)
{
  services.task->Start("Updating the Wii system", true,
                       [region = std::move(region), refresh = std::move(refresh)](
                           BackgroundTask::Context& context) -> BackgroundTask::Completion {
                         const WiiUtils::UpdateResult result =
                             WiiUtils::DoOnlineUpdate(MakeUpdateProgress(context), region);
                         ReportUpdate(result);
                         return refresh;
                       });
}

void UpdateFromDisc(const ShellServices& services, std::string disc_path, Refresh refresh)
{
  services.task->Start("Updating the Wii system", true,
                       [disc_path = std::move(disc_path), refresh = std::move(refresh)](
                           BackgroundTask::Context& context) -> BackgroundTask::Completion {
                         const WiiUtils::UpdateResult result =
                             WiiUtils::DoDiscUpdate(MakeUpdateProgress(context), disc_path);
                         ReportUpdate(result);
                         return refresh;
                       });
}

void InstallWad(const ShellServices& services, std::string path, Refresh refresh)
{
  services.task->Start("Installing title", false,
                       [path = std::move(path), refresh = std::move(refresh)](
                           BackgroundTask::Context&) -> BackgroundTask::Completion {
                         if (WiiUtils::InstallWAD(path))
                           Inform("Install title",
                                  "Successfully installed this title to the NAND.");
                         else
                           Inform("Install title", "Failed to install this title to the NAND.",
                                  Common::MsgType::Critical);
                         return refresh;
                       });
}

void UninstallTitle(const ShellServices& services, u64 title_id, std::string name, Refresh refresh)
{
  services.task->Start(
      "Removing title", false,
      [title_id, name = std::move(name),
       refresh = std::move(refresh)](BackgroundTask::Context&) -> BackgroundTask::Completion {
        if (!Ask("Uninstall title",
                 fmt::format("Uninstalling {} removes the installed version of this title from the "
                             "NAND without deleting its save data. Continue?",
                             name)))
        {
          return {};
        }
        if (WiiUtils::UninstallTitle(title_id))
          Inform("Uninstall title", "Successfully removed this title from the NAND.");
        else
          Inform("Uninstall title", "Failed to remove this title from the NAND.",
                 Common::MsgType::Critical);
        return refresh;
      });
}

void ImportSave(const ShellServices& services, std::string path)
{
  services.task->Start(
      "Importing Wii save", false,
      [path = std::move(path)](BackgroundTask::Context&) -> BackgroundTask::Completion {
        const auto can_overwrite = [] {
          return Ask("Save import",
                     "Save data for this title already exists in the NAND. Consider backing up "
                     "the current data before overwriting.\n\nOverwrite existing save data?");
        };
        switch (WiiSave::Import(path, can_overwrite))
        {
        case WiiSave::CopyResult::Success:
          Inform("Save import", "Successfully imported save file.");
          break;
        case WiiSave::CopyResult::CorruptedSource:
          Inform("Save import",
                 "Failed to import save file. The given file appears to be corrupted or is not a "
                 "valid Wii save.",
                 Common::MsgType::Critical);
          break;
        case WiiSave::CopyResult::TitleMissing:
          Inform("Save import",
                 "Failed to import save file. Please launch the game once, then try again.",
                 Common::MsgType::Critical);
          break;
        case WiiSave::CopyResult::Cancelled:
          break;
        default:
          Inform("Save import",
                 "Failed to import save file. Your NAND may be corrupt, or something is "
                 "preventing access to files within it. Try checking your NAND, then import the "
                 "save again.",
                 Common::MsgType::Critical);
          break;
        }
        return {};
      });
}

void ExportSave(const ShellServices& services, u64 title_id, std::string directory)
{
  services.task->Start("Exporting Wii save", false,
                       [title_id, directory = std::move(directory)](
                           BackgroundTask::Context&) -> BackgroundTask::Completion {
                         if (WiiSave::Export(title_id, directory) == WiiSave::CopyResult::Success)
                           Inform("Save export", fmt::format("Exported the save to {}", directory));
                         else
                           Inform("Save export", "Failed to export the save file.",
                                  Common::MsgType::Critical);
                         return {};
                       });
}

void ExportAllSaves(const ShellServices& services, std::string directory)
{
  services.task->Start(
      "Exporting Wii saves", false,
      [directory = std::move(directory)](BackgroundTask::Context&) -> BackgroundTask::Completion {
        const size_t count = WiiSave::ExportAll(directory);
        Inform("Save export", fmt::format("Exported {} save(s) to {}", count, directory));
        return {};
      });
}

void CheckNand(const ShellServices& services, Refresh refresh)
{
  services.task->Start(
      "Checking NAND", false,
      [refresh = std::move(refresh)](BackgroundTask::Context&) -> BackgroundTask::Completion {
        IOS::HLE::Kernel ios;
        const WiiUtils::NANDCheckResult result = WiiUtils::CheckNAND(ios);
        if (!result.bad)
        {
          using namespace IOS::HLE::FS;
          const bool overfull = result.used_clusters_user > USER_CLUSTERS ||
                                result.used_clusters_system > SYSTEM_CLUSTERS;
          Inform("NAND check",
                 fmt::format(
                     "{}\n\n{}\n\n{}",
                     overfull ? "Your NAND contains more data than allowed. Wii software "
                                "may behave incorrectly or not allow saving." :
                                "No issues have been detected.",
                     DescribeClusters("user-accessible", result.used_clusters_user, USER_CLUSTERS),
                     DescribeClusters("system-reserved", result.used_clusters_system,
                                      SYSTEM_CLUSTERS)),
                 overfull ? Common::MsgType::Warning : Common::MsgType::Information);
          return {};
        }

        std::string question = "The emulated NAND is damaged.";
        if (!result.titles_to_remove.empty())
        {
          question += " Fixing it requires the deletion of titles that have incomplete data on "
                      "the NAND, including all associated save data. The following title(s) "
                      "will be removed:\n";
          const Core::TitleDatabase database;
          for (const u64 title_id : result.titles_to_remove)
            question += "\n" + DescribeTitle(database, title_id);
          question += "\n\nLaunching these titles may also fix the issues.";
        }
        question += "\n\nDo you want to try to repair the NAND?";
        if (!Ask("NAND check", std::move(question)))
          return {};

        if (WiiUtils::RepairNAND(ios))
        {
          Inform("NAND check", "The NAND has been repaired.");
        }
        else
        {
          Inform("NAND check",
                 "The NAND could not be repaired. It is recommended to back up your current data "
                 "and start over with a fresh NAND.",
                 Common::MsgType::Critical);
        }
        return refresh;
      });
}

void ImportBootMiiBackup(const ShellServices& services, std::string path, Refresh refresh)
{
  services.task->Start(
      "Importing NAND backup", true,
      [&services, path = std::move(path), refresh = std::move(refresh)](
          BackgroundTask::Context& context) -> BackgroundTask::Completion {
        if (!Ask("Import NAND backup",
                 "Merging a new NAND over your current NAND will overwrite any channels and "
                 "save games that already exist. This process is not reversible, so it is "
                 "recommended that you keep backups of both NANDs. Are you sure you want to "
                 "continue?"))
        {
          return {};
        }

        const auto request_keys = [&services, &context] {
          auto promise = std::make_shared<std::promise<std::string>>();
          std::future<std::string> result = promise->get_future();
          context.Post([&services, promise] {
            services.Notify("Select the keys file (OTP/SEEPROM dump) for this backup");
            services.PickFile(L".bin", L"dolphin-nand-keys",
                              [promise](std::string keys) { promise->set_value(std::move(keys)); });
          });
          return result.get();
        };

        DiscIO::NANDImporter().ImportNANDBin(
            path,
            [&context](DiscIO::NANDImporter::Step step, int current, int maximum) {
              context.SetProgress(maximum > 0 ? static_cast<float>(current) / maximum : -1.0f,
                                  step == DiscIO::NANDImporter::Step::Loading ?
                                      "Loading NAND..." :
                                      "Extracting NAND...");
              return context.IsCancelled();
            },
            request_keys);
        Inform("Import NAND backup", "The NAND backup import has finished.");
        return refresh;
      });
}

void ExtractCertificates(const ShellServices& services)
{
  services.task->Start(
      "Extracting certificates", false, [](BackgroundTask::Context&) -> BackgroundTask::Completion {
        if (DiscIO::NANDImporter().ExtractCertificates())
        {
          Inform("Extract certificates", "Successfully extracted certificates from NAND.");
        }
        else
        {
          Inform("Extract certificates", "Failed to extract certificates from NAND.",
                 Common::MsgType::Critical);
        }
        return {};
      });
}
}  // namespace DolphinUWP::WiiSystemTools

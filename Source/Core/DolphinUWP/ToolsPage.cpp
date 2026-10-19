// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/ToolsPage.h"

#include <algorithm>
#include <array>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/Views.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"
#include "Common/Logging/LogManager.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/CommonTitles.h"
#include "Core/Config/MainSettings.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "DolphinUWP/BackgroundTask.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/ShellWidgets.h"
#include "UICommon/GameFile.h"
#include "UICommon/ResourcePack/Manager.h"
#include "UICommon/ResourcePack/Manifest.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

struct UpdateRegion
{
  const char* code;
  const char* label;
};

constexpr std::array<UpdateRegion, 4> UPDATE_REGIONS{{
    {"EUR", "Europe"},
    {"JPN", "Japan"},
    {"KOR", "Korea"},
    {"USA", "United States"},
}};

bool Toggle(const char* label, const Config::Info<bool>& info)
{
  bool value = Config::Get(info);
  if (!Frontend::ShellCheckbox(label, &value))
    return false;
  Config::SetBaseOrCurrent(info, value);
  Config::Save();
  return true;
}
}  // namespace

ToolsPage::ToolsPage(const ShellServices& services) : m_services(services)
{
}

void ToolsPage::Open(std::string_view tab)
{
  Refresh();
  RefreshResourcePacks();
  m_card_a.Open();
  m_card_b.Open();
  m_region_dialog = false;
  m_view = {};
  if (!tab.empty())
    m_view.SelectPage(tab, BuildModel());
  m_view.State().RequestFocus(Frontend::SplitPane::Navigation);
}

void ToolsPage::Close()
{
  m_card_a.Close(m_services);
  m_card_b.Close(m_services);
  m_region_dialog = false;
  m_resource_pack_action.reset();
  ReleaseResourcePackLogos();
  m_resource_packs.clear();
}

void ToolsPage::Refresh()
{
  m_movies = Recordings::ListMovies();
  m_fifo_logs = Recordings::ListFifoLogs();
  m_system_menu = WiiSystemTools::QuerySystemMenu();
  m_ipl_regions.clear();
  constexpr std::array<std::pair<DiscIO::Region, const char*>, 4> ipl_regions{{
      {DiscIO::Region::NTSC_U, USA_DIR},
      {DiscIO::Region::PAL, EUR_DIR},
      {DiscIO::Region::NTSC_J, JAP_DIR},
      {DiscIO::Region::DEV, DEV_DIR},
  }};
  for (const auto& [region, directory] : ipl_regions)
  {
    if (File::Exists(Config::GetBootROMPath(directory)))
      m_ipl_regions.emplace_back(region, directory);
  }
}

Frontend::GraphicsViewModel ToolsPage::BuildModel()
{
  Frontend::GraphicsViewModel model;
  model.pages = {
      {"start", "Start a File", true, [this] { DrawStart(); }},
      {"movies", "Input Recordings", true, [this] { DrawMovies(); }},
      {"fifo", "FIFO Player", true, [this] { DrawFifo(); }},
      {"system", "System", true, [this] { DrawSystem(); }},
      {"memory-card-a", "Memory Card A", true, [this] { m_card_a.Draw(m_services, m_card_b); }},
      {"memory-card-b", "Memory Card B", true, [this] { m_card_b.Draw(m_services, m_card_a); }},
      {"skylanders", "Skylanders Portal", true, [this] { m_skylanders.Draw(m_services); }},
      {"infinity", "Infinity Base", true, [this] { m_infinity.Draw(m_services); }},
      {"resource-packs", "Resource Packs", true, [this] { DrawResourcePacks(); }},
      {"log", "Log", true, [this] { DrawLog(); }},
      {"log-types", "Log Types", true, [this] { DrawLogTypes(); }},
  };
  return model;
}

bool ToolsPage::Draw(float controller_scroll)
{
  Frontend::GraphicsViewModel model = BuildModel();
  model.contentControllerScroll = controller_scroll;
  return m_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
}

void ToolsPage::Boot(std::unique_ptr<BootParameters> parameters, std::string title)
{
  if (!parameters)
  {
    m_services.Notify("The selected file could not be started");
    return;
  }
  m_services.boot({std::move(parameters), std::move(title), std::nullopt});
}

void ToolsPage::PickFileToStart()
{
  m_services.PickFile(GetBootableFileTypes(), L"dolphin-open", [this](std::string path) {
    if (!path.empty())
      m_services.BootFile(std::move(path));
  });
}

void ToolsPage::PickMovie()
{
  m_services.PickFile(L".dtm", L"dolphin-movie", [this](std::string path) {
    if (!path.empty())
      PlayMovie(path);
  });
}

void ToolsPage::PlayMovie(const std::string& path)
{
  const auto game_id = Recordings::ReadMovieGameId(path);
  if (!game_id)
  {
    m_services.Notify("The selected file is not a Dolphin input recording");
    return;
  }
  const auto game = m_services.library->FindGameById(*game_id);
  if (!game)
  {
    PromptService::GetInstance().Inform(
        "Play recording",
        fmt::format("The recording is for the game {}, which is not in the library.", *game_id),
        Common::MsgType::Warning);
    return;
  }

  auto& movie = Core::System::GetInstance().GetMovie();
  movie.SetReadOnly(true);
  std::optional<std::string> savestate;
  if (!movie.PlayInput(path, &savestate))
  {
    m_services.Notify("The recording could not be played");
    return;
  }
  m_services.boot({BootParameters::GenerateFromFile(
                       game->game->GetFilePath(),
                       BootSessionData(std::move(savestate), DeleteSavestateAfterBoot::No)),
                   game->name, game->id});
}

void ToolsPage::PlayFifo(const std::string& path)
{
  Boot(BootParameters::GenerateFromFile(path),
       fmt::format("FIFO log {}", PathToString(StringToPath(path).stem())));
}

void ToolsPage::DrawStart()
{
  Frontend::SectionTitle("Start a file");
  if (Frontend::ShellButton("Choose a file to start"))
    PickFileToStart();
  ShellWidgets::HintText("Starts a disc image, WAD, DOL or ELF file, FIFO log or M3U playlist "
                         "without adding it to the library.");
}

void ToolsPage::DrawLog()
{
  using Common::Log::LogLevel;
  using Common::Log::LogListener;
  auto* const manager = Common::Log::LogManager::GetInstance();

  Frontend::SectionTitle("Log settings");
  constexpr std::array<std::pair<LogLevel, const char*>, 5> levels{{
      {LogLevel::LNOTICE, "Notice"},
      {LogLevel::LERROR, "Error"},
      {LogLevel::LWARNING, "Warning"},
      {LogLevel::LINFO, "Info"},
      {LogLevel::LDEBUG, "Debug"},
  }};
  const LogLevel current = Config::Get(Common::Log::LOGGER_VERBOSITY);
  std::vector<Frontend::ChoiceItem> items;
  size_t selected = 0;
  for (const auto& [level, label] : levels)
  {
    if (level > Common::Log::MAX_EFFECTIVE_LOGLEVEL)
      continue;
    if (level == current)
      selected = items.size();
    items.push_back({std::to_string(static_cast<int>(level)), label});
  }
  if (Frontend::ShellCombo("Verbosity", &selected, items))
  {
    manager->SetConfigLogLevel(levels[selected].first);
    manager->SaveSettings();
  }

  bool write_to_file = manager->IsListenerEnabled(LogListener::FILE_LISTENER);
  if (Frontend::ShellCheckbox("Write to file", &write_to_file))
  {
    manager->EnableListener(LogListener::FILE_LISTENER, write_to_file);
    manager->SaveSettings();
  }
  ShellWidgets::HintText(fmt::format("The log file is {}", File::GetUserPath(F_MAINLOG_IDX)));

  Frontend::SectionTitle("Messages");
  if (Frontend::ShellButton("Clear the log"))
    LogBuffer::Clear();

  if (const u64 version = LogBuffer::GetVersion(); version != m_log_version)
  {
    m_log_version = version;
    m_log_entries = LogBuffer::GetEntries();
  }
  if (m_log_entries.empty())
  {
    ShellWidgets::HintText("There are no messages. Enable log types to see their messages.");
    return;
  }

  const Frontend::ShellTheme& theme = Frontend::CurrentWidgetTheme();
  ImGui::PushTextWrapPos(0.0f);
  for (const LogBuffer::Entry& entry : m_log_entries)
  {
    const ImVec4 color = entry.level == LogLevel::LERROR   ? ImVec4(0.90f, 0.25f, 0.25f, 1.0f) :
                         entry.level == LogLevel::LWARNING ? ImVec4(0.85f, 0.60f, 0.10f, 1.0f) :
                                                             theme.textPrimary;
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(entry.text.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::PopTextWrapPos();
}

void ToolsPage::DrawLogTypes()
{
  auto* const manager = Common::Log::LogManager::GetInstance();
  const auto set_all = [manager](bool enable) {
    for (int i = 0; i < static_cast<int>(Common::Log::LogType::NUMBER_OF_LOGS); ++i)
      manager->SetEnable(static_cast<Common::Log::LogType>(i), enable);
    manager->SaveSettings();
  };

  Frontend::SectionTitle("Log types");
  ShellWidgets::HintText("The IOS types are for the Wii's system software, which is called IOS.");
  if (Frontend::ShellButton("Enable all"))
    set_all(true);
  if (Frontend::ShellButton("Disable all"))
    set_all(false);

  const std::vector<Common::Log::LogManager::LogContainer> types = manager->GetLogTypes();
  for (size_t i = 0; i < types.size(); ++i)
  {
    // Only the OpenGL and Vulkan backends write Host GPU messages, and nothing writes Memory Card
    // Manager messages.
    const auto type = static_cast<Common::Log::LogType>(i);
    if (type == Common::Log::LogType::HOST_GPU || type == Common::Log::LogType::MEMCARD_MANAGER)
      continue;
    bool enabled = types[i].m_enable;
    ImGui::PushID(static_cast<int>(i));
    if (Frontend::ShellCheckbox(types[i].m_full_name, &enabled))
    {
      manager->SetEnable(type, enabled);
      manager->SaveSettings();
    }
    ImGui::PopID();
  }
}

void ToolsPage::DrawMovies()
{
  Frontend::SectionTitle("Input recordings");
  if (Frontend::ShellButton("Play a recording file"))
    PickMovie();
  ShellWidgets::HintText(fmt::format(
      "Start recording from a game's options or from the in-game menu. Recordings are saved to {}",
      Recordings::GetMovieDirectory()));

  if (m_movies.empty())
    ShellWidgets::HintText("There are no saved recordings yet.");
  for (size_t i = 0; i < m_movies.size(); ++i)
  {
    const Recordings::RecordingFile& movie = m_movies[i];
    const auto game = m_services.library->FindGameById(movie.game_id);
    ImGui::PushID(static_cast<int>(i));
    if (ShellWidgets::DetailRow("movie", {}, {}, movie.name, game ? game->name : movie.game_id))
      PlayMovie(movie.path);
    ImGui::PopID();
  }

  Frontend::SectionTitle("Playback and display");
  Toggle("Pause at the end of a recording", Config::MAIN_MOVIE_PAUSE_MOVIE);
  Toggle("Show movie window", Config::MAIN_MOVIE_SHOW_OSD);
  Toggle("Show input display", Config::MAIN_MOVIE_SHOW_INPUT_DISPLAY);
  Toggle("Show frame counter", Config::MAIN_SHOW_FRAME_COUNT);
  Toggle("Show lag counter", Config::MAIN_SHOW_LAG);
  Toggle("Show system clock", Config::MAIN_MOVIE_SHOW_RTC);
  Toggle("Show rerecord counter", Config::MAIN_MOVIE_SHOW_RERECORD);
}

void ToolsPage::DrawFifo()
{
  Frontend::SectionTitle("FIFO player");
  if (Frontend::ShellButton("Play a FIFO log file"))
  {
    m_services.PickFile(L".dff", L"dolphin-fifo", [this](std::string path) {
      if (!path.empty())
        PlayFifo(path);
    });
  }
  ShellWidgets::HintText(fmt::format("Record FIFO logs from the in-game menu. Logs are saved to {}",
                                     Recordings::GetFifoDirectory()));

  if (m_fifo_logs.empty())
    ShellWidgets::HintText("There are no saved FIFO logs yet.");
  for (size_t i = 0; i < m_fifo_logs.size(); ++i)
  {
    ImGui::PushID(static_cast<int>(i));
    if (ShellWidgets::DetailRow("fifo", {}, {}, m_fifo_logs[i].name, {}))
      PlayFifo(m_fifo_logs[i].path);
    ImGui::PopID();
  }

  Frontend::SectionTitle("Playback");
  Toggle("Loop playback", Config::MAIN_FIFOPLAYER_LOOP_REPLAY);
  Toggle("Early memory updates", Config::MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES);
}

void ToolsPage::DrawSystem()
{
  const auto refresh = [this] { Refresh(); };

  Frontend::SectionTitle("Wii System Menu");
  if (m_system_menu.installed)
  {
    ShellWidgets::HintText(fmt::format("Installed version: {}{}", m_system_menu.version,
                                       m_system_menu.vwii ? " (vWii)" : ""));
    if (Frontend::ShellButton(m_system_menu.vwii ? "Start the vWii System Menu" :
                                                   "Start the Wii System Menu"))
    {
      Boot(std::make_unique<BootParameters>(BootParameters::NANDTitle{Titles::SYSTEM_MENU}),
           "Wii System Menu");
    }
    if (Frontend::ShellButton("Update the Wii system online"))
      WiiSystemTools::UpdateOnline(m_services, {}, refresh);
  }
  else
  {
    ShellWidgets::HintText("The Wii System Menu is not installed. Install it by updating the Wii "
                           "system online.");
    if (Frontend::ShellButton("Install the Wii system online"))
      m_region_dialog = true;
  }

  Frontend::SectionTitle("Titles and saves");
  if (Frontend::ShellButton("Install a WAD file"))
  {
    m_services.PickFile(L".wad", L"dolphin-wad", [this, refresh](std::string path) {
      if (!path.empty())
        WiiSystemTools::InstallWad(m_services, path, refresh);
    });
  }
  if (Frontend::ShellButton("Import a Wii save"))
  {
    m_services.PickFile(L".bin", L"dolphin-wii-save", [this](std::string path) {
      if (!path.empty())
        WiiSystemTools::ImportSave(m_services, path);
    });
  }
  if (Frontend::ShellButton("Export all Wii saves"))
  {
    m_services.PickFolder(L"dolphin-wii-save-export", [this](std::string directory) {
      if (!directory.empty())
        WiiSystemTools::ExportAllSaves(m_services, directory);
    });
  }

  Frontend::SectionTitle("NAND");
  if (Frontend::ShellButton("Check the NAND"))
    WiiSystemTools::CheckNand(m_services, refresh);
  if (Frontend::ShellButton("Import a BootMii NAND backup"))
  {
    m_services.PickFile(L".bin", L"dolphin-nand-backup", [this, refresh](std::string path) {
      if (!path.empty())
        WiiSystemTools::ImportBootMiiBackup(m_services, path, refresh);
    });
  }
  if (Frontend::ShellButton("Extract certificates from the NAND"))
    WiiSystemTools::ExtractCertificates(m_services);

  Frontend::SectionTitle("GameCube");
  if (m_ipl_regions.empty())
  {
    ShellWidgets::HintText(fmt::format("To start the GameCube main menu, place an IPL dump named "
                                       "{} in the region folder under {}.",
                                       GC_IPL, File::GetUserPath(D_GCUSER_IDX)));
  }
  for (const auto& [region, directory] : m_ipl_regions)
  {
    const bool triforce = region == DiscIO::Region::DEV;
    const std::string label = triforce ?
                                  std::string("Start the Triforce system menu") :
                                  fmt::format("Start the GameCube main menu ({})", directory);
    if (Frontend::ShellButton(label.c_str()))
    {
      Boot(std::make_unique<BootParameters>(BootParameters::IPL{region}),
           triforce ? "Triforce System Menu" : "GameCube Main Menu");
    }
  }
  DrawRegionDialog();
}

void ToolsPage::RefreshResourcePacks()
{
  ReleaseResourcePackLogos();
  m_resource_packs.clear();
  ResourcePack::Init();

  for (const ResourcePack::ResourcePack& pack : ResourcePack::GetPacks())
  {
    const ResourcePack::Manifest* const manifest = pack.GetManifest();
    ResourcePackRow row{.path = pack.GetPath(), .title = manifest->GetName()};
    if (!manifest->GetVersion().empty())
      row.title += fmt::format(" {}", manifest->GetVersion());
    if (const auto& authors = manifest->GetAuthors())
      row.detail = fmt::format("By {}", *authors);
    if (const auto& description = manifest->GetDescription())
      row.detail =
          row.detail.empty() ? *description : fmt::format("{}. {}", row.detail, *description);
    row.valid = true;
    row.installed = ResourcePack::IsInstalled(pack);

    const std::vector<char>& logo = pack.GetLogo();
    Common::UniqueBuffer<u8> pixels;
    u32 width = 0;
    u32 height = 0;
    if (!logo.empty() && m_services.create_texture &&
        Common::LoadPNG({reinterpret_cast<const u8*>(logo.data()), logo.size()}, &pixels, &width,
                        &height))
    {
      row.logo = m_services.create_texture(pixels.data(), width, height);
    }
    m_resource_packs.push_back(std::move(row));
  }

  for (const std::string& path :
       Common::DoFileSearch(File::GetUserPath(D_RESOURCEPACK_IDX), ".zip"))
  {
    if (std::ranges::any_of(m_resource_packs,
                            [&path](const ResourcePackRow& row) { return row.path == path; }))
    {
      continue;
    }
    std::string name;
    std::string extension;
    SplitPath(path, nullptr, &name, &extension);
    m_resource_packs.push_back({.path = path,
                                .title = name + extension,
                                .detail = ResourcePack::ResourcePack(path).GetError()});
  }
}

void ToolsPage::ReleaseResourcePackLogos()
{
  for (ResourcePackRow& row : m_resource_packs)
  {
    if (row.logo && m_services.release_texture)
      m_services.release_texture(row.logo);
    row.logo = {};
  }
}

void ToolsPage::DrawResourcePacks()
{
  Frontend::SectionTitle("Resource packs");
  if (Frontend::ShellButton("Import a resource pack"))
  {
    m_services.PickFile(L".zip", L"dolphin-resource-pack", [this](std::string path) {
      if (!path.empty())
        ImportResourcePack(path);
    });
  }
  ShellWidgets::HintText(fmt::format("Resource packs replace game textures when they are "
                                     "installed. Imported packs are stored in {}",
                                     File::GetUserPath(D_RESOURCEPACK_IDX)));

  if (m_resource_packs.empty())
    ShellWidgets::HintText("No resource packs have been imported.");
  for (size_t i = 0; i < m_resource_packs.size(); ++i)
  {
    const ResourcePackRow& row = m_resource_packs[i];
    ImGui::PushID(static_cast<int>(i));
    const std::string_view status = !row.valid ? "Unreadable" : row.installed ? "Installed" : "";
    if (ShellWidgets::DetailRow("resource-pack", row.logo, {56.0f, 56.0f}, row.title, row.detail,
                                status))
    {
      m_resource_pack_action = i;
    }
    ImGui::PopID();
  }
  DrawResourcePackActions();
}

void ToolsPage::DrawResourcePackActions()
{
  constexpr const char* popup = "resource-pack-actions";
  if (!m_resource_pack_action || *m_resource_pack_action >= m_resource_packs.size())
  {
    m_resource_pack_action.reset();
    return;
  }
  if (!ImGui::IsPopupOpen(popup))
    ImGui::OpenPopup(popup);

  const ResourcePackRow& row = m_resource_packs[*m_resource_pack_action];
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  const Frontend::ShellLayoutContext context =
      Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  Frontend::SelectionDialogModel model;
  model.popupId = popup;
  model.title = row.title;
  if (row.valid)
    model.entries.push_back(
        {row.installed ? "uninstall" : "install", row.installed ? "Uninstall" : "Install"});
  model.entries.push_back({"delete", "Delete"});
  model.closeLabel = "Cancel";
  const Frontend::SelectionDialogResult result =
      Frontend::DrawSelectionDialog(model, context, Frontend::CurrentWidgetTheme());
  if (!result.selected && !result.closed)
    return;

  const ResourcePackRow selected = row;
  m_resource_pack_action.reset();
  if (result.selected && *result.selected < model.entries.size())
    ChangeResourcePack(selected, model.entries[*result.selected].id);
}

void ToolsPage::ImportResourcePack(const std::string& path)
{
  const std::string file_name = PathToString(StringToPath(path).filename());
  const std::string directory = File::GetUserPath(D_RESOURCEPACK_IDX);
  const std::string destination = directory + file_name;
  if (File::Exists(destination))
  {
    PromptService::GetInstance().Inform(
        "Import resource pack", fmt::format("A resource pack is already stored as {}.", file_name),
        Common::MsgType::Warning);
    return;
  }

  // The pack is read from the user folder, which the zip reader can always open.
  m_services.task->Start(
      "Importing resource pack", false,
      [this, path, directory, destination](BackgroundTask::Context&) -> BackgroundTask::Completion {
        const bool copied = File::CreateFullPath(directory) && File::Copy(path, destination);
        return [this, copied, destination] {
          std::string error;
          if (!copied)
          {
            error = "The resource pack could not be copied.";
          }
          else if (const ResourcePack::ResourcePack pack(destination); !pack.IsValid())
          {
            error = fmt::format("The file is not a valid resource pack. {}", pack.GetError());
          }
          else if (std::ranges::any_of(
                       ResourcePack::GetPacks(), [&pack](const ResourcePack::ResourcePack& other) {
                         return other.GetManifest()->GetID() == pack.GetManifest()->GetID();
                       }))
          {
            error = "This resource pack has already been imported.";
          }
          if (!error.empty())
          {
            File::Delete(destination);
            PromptService::GetInstance().Inform("Import resource pack", error,
                                                Common::MsgType::Warning);
          }
          RefreshResourcePacks();
        };
      });
}

void ToolsPage::ChangeResourcePack(const ResourcePackRow& row, std::string_view action)
{
  const auto run = [this, path = row.path](std::string title, std::string operation) {
    m_services.task->Start(
        std::move(title), false,
        [this, path, operation](BackgroundTask::Context&) -> BackgroundTask::Completion {
          std::string error;
          auto& packs = ResourcePack::GetPacks();
          const auto pack = std::ranges::find(packs, path, &ResourcePack::ResourcePack::GetPath);
          const std::string load_path = File::GetUserPath(D_LOAD_IDX);
          if (pack != packs.end() && (operation == "uninstall" ||
                                      (operation == "delete" && ResourcePack::IsInstalled(*pack))))
          {
            if (!pack->Uninstall(load_path))
              error = pack->GetError();
          }
          else if (pack != packs.end() && operation == "install")
          {
            if (!pack->Install(load_path))
              error = pack->GetError();
          }
          if (operation == "delete" && error.empty() && !File::Delete(path))
            error = "The pack file could not be deleted.";
          return [this, error] {
            if (!error.empty())
            {
              PromptService::GetInstance().Inform("Resource packs", error,
                                                  Common::MsgType::Warning);
            }
            RefreshResourcePacks();
          };
        });
  };

  if (action == "install")
  {
    run("Installing resource pack", "install");
  }
  else if (action == "uninstall")
  {
    run("Uninstalling resource pack", "uninstall");
  }
  else if (action == "delete")
  {
    PromptService::GetInstance().Confirm(
        "Delete resource pack",
        fmt::format("Delete {}? Its textures are uninstalled and the pack file is deleted.",
                    row.title),
        [run](bool confirmed) {
          if (confirmed)
            run("Deleting resource pack", "delete");
        });
  }
}

void ToolsPage::DrawRegionDialog()
{
  constexpr const char* popup = "wii-update-region";
  if (!m_region_dialog)
    return;
  if (!ImGui::IsPopupOpen(popup))
    ImGui::OpenPopup(popup);

  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  const Frontend::ShellLayoutContext context =
      Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  Frontend::SelectionDialogModel model;
  model.popupId = popup;
  model.title = "Choose the console region";
  for (const UpdateRegion& region : UPDATE_REGIONS)
    model.entries.push_back({region.code, region.label});
  model.closeLabel = "Cancel";
  const Frontend::SelectionDialogResult result =
      Frontend::DrawSelectionDialog(model, context, Frontend::CurrentWidgetTheme());
  if (!result.selected && !result.closed)
    return;
  m_region_dialog = false;
  if (result.selected && *result.selected < UPDATE_REGIONS.size())
  {
    WiiSystemTools::UpdateOnline(m_services, UPDATE_REGIONS[*result.selected].code,
                                 [this] { Refresh(); });
  }
}
}  // namespace DolphinUWP

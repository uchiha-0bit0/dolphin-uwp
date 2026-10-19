// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/QuickMenu.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <tuple>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/AchievementManager.h"
#include "Core/CheatGeneration.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"
#include "Core/NetPlayProto.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"
#include "DolphinUWP/FifoAnalysis.h"
#include "DolphinUWP/GBAScreens.h"
#include "DolphinUWP/GameLibrary.h"
#include "DolphinUWP/InputMappingHost.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/Recordings.h"
#include "DolphinUWP/Translation.h"
#include "DolphinUWP/UWPSettings.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace DolphinUWP
{
namespace
{
constexpr u32 STATE_SLOT_COUNT = 10;
constexpr int MAX_RESOLUTION_CHOICE = 8;
constexpr std::array<float, 6> SPEED_CHOICES{0.0f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
constexpr std::array<const char*, MAX_RESOLUTION_CHOICE + 1> RESOLUTION_LABELS{
    "Auto (Window Size)",    "Native (640x528)",      "2x Native (1280x1056)",
    "3x Native (1920x1584)", "4x Native (2560x2112)", "5x Native (3200x2640)",
    "6x Native (3840x3168)", "7x Native (4480x3696)", "8x Native (5120x4224)"};
constexpr std::array<s32, 4> FIFO_FRAME_COUNTS{1, 10, 60, 300};
constexpr std::array<const char*, 6> CHAT_MESSAGES{
    "Ready", "Pause please", "One moment", "Good game", "Restarting", "Is it lagging for you?"};
constexpr size_t CHEAT_SEARCH_LISTED_RESULTS = 50;
constexpr size_t FIFO_DESCRIPTION_LIMIT = 1500;

constexpr std::array<std::pair<Cheats::DataType, const char*>, 7> SEARCH_TYPES{{
    {Cheats::DataType::U8, "8-bit unsigned"},
    {Cheats::DataType::U16, "16-bit unsigned"},
    {Cheats::DataType::U32, "32-bit unsigned"},
    {Cheats::DataType::S8, "8-bit signed"},
    {Cheats::DataType::S16, "16-bit signed"},
    {Cheats::DataType::S32, "32-bit signed"},
    {Cheats::DataType::F32, "Floating point"},
}};

constexpr std::array<std::pair<Cheats::CompareType, const char*>, 6> SEARCH_COMPARISONS{{
    {Cheats::CompareType::Equal, "Equal to"},
    {Cheats::CompareType::NotEqual, "Not equal to"},
    {Cheats::CompareType::Less, "Less than"},
    {Cheats::CompareType::LessOrEqual, "Less than or equal to"},
    {Cheats::CompareType::Greater, "Greater than"},
    {Cheats::CompareType::GreaterOrEqual, "Greater than or equal to"},
}};

constexpr std::array<const char*, 12> NUMBER_PAD{"1", "2", "3", "4", "5", "6",
                                                 "7", "8", "9", "0", "-", "."};

template <typename T, size_t N>
const char* LabelOf(const std::array<std::pair<T, const char*>, N>& labels, T value)
{
  const auto found = std::ranges::find(labels, value, &std::pair<T, const char*>::first);
  return found != labels.end() ? found->second : "";
}

std::string OnOff(bool value)
{
  return value ? "On" : "Off";
}

std::string ResolutionValue(int scale)
{
  if (scale <= 0)
    return "Auto";
  if (scale == 1)
    return "Native";
  return fmt::format("{}x", scale);
}

std::string ResultCount(size_t count)
{
  return fmt::format("{} result{}", count, count == 1 ? "" : "s");
}

std::string AchievementSummary()
{
  AchievementManager& achievements = AchievementManager::GetInstance();
  std::lock_guard lock(achievements.GetLock());
  rc_client_t* const client = achievements.GetClient();
  if (!client)
    return {};
  rc_client_user_game_summary_t summary{};
  rc_client_get_user_game_summary(client, &summary);
  return fmt::format("{} of {}", summary.num_unlocked_achievements, summary.num_core_achievements);
}

std::string SpeedLabel(float speed)
{
  if (speed <= 0.0f)
    return "Unlimited";
  return fmt::format("{}%", std::lround(speed * 100.0f));
}

size_t NearestSpeedChoice(float speed)
{
  size_t best = 0;
  for (size_t i = 1; i < SPEED_CHOICES.size(); ++i)
  {
    if (std::abs(SPEED_CHOICES[i] - speed) < std::abs(SPEED_CHOICES[best] - speed))
      best = i;
  }
  return best;
}

bool SlotExists(u32 slot)
{
  return File::Exists(fmt::format("{}{}.s{:02d}", File::GetUserPath(D_STATESAVES_IDX),
                                  SConfig::GetInstance().GetGameID(), slot));
}

constexpr std::array<std::pair<Config::GBALayout, const char*>, 6> GBA_LAYOUTS{{
    {Config::GBALayout::Bottom, "Along the bottom"},
    {Config::GBALayout::Top, "Along the top"},
    {Config::GBALayout::Left, "Down the left side"},
    {Config::GBALayout::Right, "Down the right side"},
    {Config::GBALayout::Corners, "In the corners"},
    {Config::GBALayout::Hidden, "Hidden"},
}};
constexpr std::array<const char*, 5> GBA_SCALES{"Automatic", "1x", "2x", "3x", "4x"};

// Labels a setting of one GBA that can follow the setting of all GBAs.
std::string OwnSetting(bool shared, std::string own, std::string_view all)
{
  if (!shared)
    return own;
  return fmt::format("{} ({})", Translation::Translate("Default"), Translation::Translate(all));
}

std::string GBALayoutLabel(Config::GBALayout layout)
{
  const auto it =
      std::ranges::find(GBA_LAYOUTS, layout, &std::pair<Config::GBALayout, const char*>::first);
  return it != GBA_LAYOUTS.end() ? it->second : GBA_LAYOUTS[0].second;
}

bool CanControlGBA()
{
  return !Core::System::GetInstance().GetMovie().IsMovieActive() && !NetPlay::IsNetPlayRunning();
}

// Parses ids like "gecko-3" into 3.
std::optional<size_t> ParseIndex(std::string_view id, std::string_view prefix)
{
  if (!id.starts_with(prefix))
    return std::nullopt;
  size_t value = 0;
  if (!TryParse(std::string(id.substr(prefix.size())), &value))
    return std::nullopt;
  return value;
}

// The emulated Wii Remotes of the running game, which have settings in the quick menu.
std::vector<int> GetEmulatedWiiRemotes()
{
  std::vector<int> remotes;
  if (!Core::System::GetInstance().IsWii())
    return remotes;
  for (int index = 0; index < MAX_WIIMOTES; ++index)
  {
    if (Config::Get(Config::GetInfoForWiimoteSource(index)) == WiimoteSource::Emulated)
      remotes.push_back(index);
  }
  return remotes;
}

std::string OrientationLabel(const InputMappingHost::WiiRemoteOptions& options)
{
  if (options.sideways && options.upright)
    return "Sideways and upright";
  return options.sideways ? "Sideways" : options.upright ? "Upright" : "Normal";
}

std::string ExtensionLabel(const InputMappingHost::WiiRemoteOptions& options)
{
  const auto extensions = InputMappingHost::GetWiiRemoteExtensions();
  const auto found =
      std::ranges::find(extensions, options.type_id, &std::pair<std::string, std::string>::first);
  return found != extensions.end() ? found->second : std::string();
}

void Toggle(const Config::Info<bool>& info)
{
  Config::SetBaseOrCurrent(info, !Config::Get(info));
  Config::Save();
}
}  // namespace

void QuickMenu::ShowMessage(std::string message)
{
  OSD::AddMessage(std::move(message), OSD::Duration::NORMAL);
}

void QuickMenu::Open()
{
  m_page = Page::Main;
  m_selected_row = 0;
  m_choice_row.reset();
  m_choices.clear();
  m_creating_figure = false;
  m_return_rows.clear();
}

QuickMenu::Effect QuickMenu::OpenAt(std::string_view id)
{
  Open();
  m_rows = BuildRows();
  const auto row = std::ranges::find(m_rows, id, &Row::id);
  if (row == m_rows.end())
    return Effect::None;
  m_selected_row = static_cast<size_t>(row - m_rows.begin());
  return ActivateMain(row->id);
}

std::vector<QuickMenu::Row> QuickMenu::BuildRows()
{
  auto& system = Core::System::GetInstance();
  std::vector<Row> rows;
  switch (m_page)
  {
  case Page::Main:
  {
    auto& movie = system.GetMovie();
    const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
    rows.push_back({"resume", "Resume"});
    rows.push_back({"save-state", "Save State"});
    rows.push_back({"load-state", "Load State", {}, !hardcore});
    if (!system.IsTriforce())
      rows.push_back({"change-disc", "Change Disc"});
    rows.push_back({"controllers", "Controllers"});
    rows.push_back({"internal-resolution", "Internal Resolution",
                    ResolutionValue(Config::Get(Config::GFX_EFB_SCALE))});
    rows.push_back({"emulation-speed", "Emulation Speed",
                    SpeedLabel(Config::Get(Config::MAIN_EMULATION_SPEED))});
    rows.push_back({"show-fps", "Show FPS", OnOff(Config::Get(Config::GFX_SHOW_FPS))});
    rows.push_back(
        {"free-look", "Free Look", OnOff(Config::Get(Config::FREE_LOOK_ENABLED)), !hardcore});
    rows.push_back({"cheats", "Cheats", OnOff(Config::AreCheatsEnabled()), !hardcore});
    rows.push_back({"graphics-mods", "Graphics Mods", OnOff(Config::Get(Config::GFX_MODS_ENABLE))});
    if (Config::Get(Config::MAIN_EMULATE_SKYLANDER_PORTAL))
      rows.push_back({"skylanders", "Skylanders Portal"});
    if (Config::Get(Config::MAIN_EMULATE_INFINITY_BASE))
      rows.push_back({"infinity", "Infinity Base"});
    {
      size_t running = 0;
      for (size_t port = 0; port < GBAScreens::MAX_SCREENS; ++port)
        running += GBAScreens::IsRunning(port) ? 1 : 0;
      if (running != 0)
        rows.push_back({"gba", "Game Boy Advance", fmt::format("{} connected", running)});
    }
    rows.push_back(
        {"recording", "Recording",
         movie.IsRecordingInput() ? "Recording" :
         movie.IsPlayingInput()   ? "Playing" :
         Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES) || Config::Get(Config::MAIN_DUMP_AUDIO) ?
                                  "Dumping" :
                                  ""});
    rows.push_back({"fifo", "FIFO Recording", m_fifo_recording ? "Recording" : ""});
    if (FifoAnalysis::IsAvailable())
      rows.push_back({"fifo-analyzer", "FIFO Analyzer"});
    rows.push_back({"cheat-search", "Cheat Search",
                    m_search && m_search->WasFirstSearchDone() ?
                        ResultCount(m_search->GetResultCount()) :
                        std::string(),
                    !hardcore});
    if (AchievementManager::GetInstance().IsGameLoaded())
      rows.push_back({"achievements", "Achievements", AchievementSummary()});
    if (Leaderboards::Available())
      rows.push_back({"leaderboards", "Leaderboards"});
    if (m_chat_sender && NetPlay::IsNetPlayRunning())
      rows.push_back({"netplay-chat", "Send Chat Message"});
    if (Config::IsDebuggingEnabled())
      rows.push_back({"debugger", "Debugger"});
    rows.push_back({"reset", "Reset"});
    rows.push_back({"exit", m_exit_label});
    break;
  }
  case Page::Cheats:
  {
    const bool enabled = Config::AreCheatsEnabled();
    rows.push_back({"cheats-enabled", "Enable cheats", OnOff(enabled)});
    for (size_t i = 0; i < m_cheats.gecko.size(); ++i)
    {
      rows.push_back({fmt::format("gecko-{}", i), m_cheats.gecko[i].name,
                      OnOff(m_cheats.gecko[i].enabled), enabled});
    }
    for (size_t i = 0; i < m_cheats.action_replay.size(); ++i)
    {
      rows.push_back({fmt::format("ar-{}", i),
                      fmt::format("{} (Action Replay)", m_cheats.action_replay[i].name),
                      OnOff(m_cheats.action_replay[i].enabled), enabled});
    }
    if (m_cheats.gecko.empty() && m_cheats.action_replay.empty())
      rows.push_back({"cheats-none", "There are no codes for this game", {}, false});
    break;
  }
  case Page::GraphicsMods:
  {
    const bool enabled = Config::Get(Config::GFX_MODS_ENABLE);
    rows.push_back({"mods-enabled", "Enable graphics mods", OnOff(enabled)});
    const std::vector<GraphicsModList::Mod> mods = m_mods.GetMods();
    for (size_t i = 0; i < mods.size(); ++i)
      rows.push_back({fmt::format("mod-{}", i), mods[i].title, OnOff(mods[i].enabled), enabled});
    if (mods.empty())
      rows.push_back({"mods-none", "There are no graphics mods for this game", {}, false});
    break;
  }
  case Page::Skylanders:
    for (u8 slot = 0; slot < Portals::SKYLANDER_SLOTS; ++slot)
    {
      const std::string name = Portals::GetSkylanderName(slot);
      rows.push_back({fmt::format("sky-{}", slot), fmt::format("Slot {}", slot + 1),
                      name.empty() ? "Empty" : name});
    }
    break;
  case Page::Infinity:
    for (u8 slot = 0; slot < Portals::INFINITY_SLOTS; ++slot)
    {
      const std::string name = Portals::GetInfinityName(slot);
      rows.push_back({fmt::format("inf-{}", slot), Portals::GetInfinitySlotLabel(slot),
                      name.empty() ? "Empty" : name});
    }
    break;
  case Page::GBAs:
    for (size_t port = 0; port < GBAScreens::MAX_SCREENS; ++port)
    {
      if (const std::optional<GBAScreens::Status> status = GBAScreens::GetStatus(port))
      {
        rows.push_back({fmt::format("gba-port-{}", port), fmt::format("GBA {}", port + 1),
                        status->has_rom ? status->title : std::string("No ROM")});
      }
    }
    rows.push_back(
        {"gba-layout", "Screen position", GBALayoutLabel(Config::Get(Config::UWP_GBA_LAYOUT))});
    rows.push_back({"gba-scale", "Screen size",
                    GBA_SCALES[std::clamp(Config::Get(Config::UWP_GBA_SCALE), 0, 4)]});
    rows.push_back({"gba-frame", "Frame", OnOff(Config::Get(Config::UWP_GBA_FRAME))});
    rows.push_back({"gba-blending", "Interframe blending",
                    OnOff(Config::Get(Config::UWP_GBA_INTERFRAME_BLENDING))});
    rows.push_back({"gba-show-streamed", "Show screens that phones show",
                    OnOff(Config::Get(Config::UWP_GBA_SHOW_STREAMED))});
    break;
  case Page::GBA:
  {
    const std::optional<GBAScreens::Status> status = GBAScreens::GetStatus(m_gba_port);
    if (!status)
    {
      rows.push_back({"gba-off", "This GBA is no longer running", {}, false});
      break;
    }
    const bool control = CanControlGBA();
    rows.push_back({"gba-connected", "Connected", OnOff(status->connected), control});
    rows.push_back({"gba-reset", "Reset"});
    rows.push_back({"gba-load-rom", "Load ROM", status->has_rom ? status->title : "", control});
    rows.push_back({"gba-unload-rom", "Unload ROM", {}, control && status->has_rom});
    rows.push_back({"gba-ereader", "Scan e-Reader cards", {}, status->has_ereader});
    rows.push_back({"gba-import-save", "Import save game", {}, control && status->has_rom});
    rows.push_back({"gba-export-save", "Export save game", {}, status->has_rom});
    rows.push_back({"gba-import-state", "Import state", {}, control});
    rows.push_back({"gba-export-state", "Export state"});
    rows.push_back({"gba-mute", "Mute", OnOff(status->muted)});
    rows.push_back({"gba-volume", "Volume", fmt::format("{}%", status->volume)});
    const int scale = Config::Get(Config::UWP_GBA_SCALES[m_gba_port]);
    rows.push_back({"gba-own-scale", "Screen size",
                    OwnSetting(scale < 0, GBA_SCALES[std::clamp(scale, 0, 4)],
                               GBA_SCALES[std::clamp(Config::Get(Config::UWP_GBA_SCALE), 0, 4)])});
    const int blending = Config::Get(Config::UWP_GBA_BLENDING[m_gba_port]);
    rows.push_back({"gba-own-blending", "Interframe blending",
                    OwnSetting(blending < 0, OnOff(blending > 0),
                               OnOff(Config::Get(Config::UWP_GBA_INTERFRAME_BLENDING)))});
    rows.push_back({"gba-on-top", "Always on top",
                    OnOff(Config::Get(Config::UWP_GBA_ALWAYS_ON_TOP[m_gba_port]))});
    break;
  }
  case Page::Recording:
  {
    auto& movie = system.GetMovie();
    if (!movie.IsMovieActive())
      rows.push_back({"record-start", "Start recording"});
    if (movie.IsRecordingInput())
      rows.push_back({"record-save", "Stop and save the recording"});
    if (movie.IsPlayingInput())
      rows.push_back({"playback-stop", "Stop playback"});
    if (movie.IsMovieActive())
    {
      rows.push_back({"read-only", "Read-only mode", OnOff(movie.IsReadOnly())});
      rows.push_back({"frames", "Frame",
                      movie.IsPlayingInput() ?
                          fmt::format("{} of {}", movie.GetCurrentFrame(), movie.GetTotalFrames()) :
                          std::to_string(movie.GetCurrentFrame()),
                      false});
    }
    m_tas_targets = TasView::GetTargets();
    const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
    for (size_t i = 0; i < m_tas_targets.size(); ++i)
    {
      rows.push_back({fmt::format("tas-{}", i),
                      fmt::format("TAS input: {}", TasView::GetName(m_tas_targets[i])),
                      {},
                      !hardcore});
    }
    rows.push_back(
        {"dump-frames", "Dump frames", OnOff(Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))});
    rows.push_back({"dump-audio", "Dump audio", OnOff(Config::Get(Config::MAIN_DUMP_AUDIO))});
    break;
  }
  case Page::CheatSearch:
  {
    rows.push_back({"search-type", "Value type", LabelOf(SEARCH_TYPES, m_search_type)});
    if (!m_search)
    {
      rows.push_back({"search-start", "Start a new search"});
      break;
    }
    const bool first = !m_search->WasFirstSearchDone();
    const size_t count = m_search->GetResultCount();
    rows.push_back({"search-source", "Compare with",
                    m_search_source == SearchSource::Specific ? "A value" :
                    m_search_source == SearchSource::Any      ? "Any value (first search)" :
                                                                "The previous value"});
    if (m_search_source != SearchSource::Any)
    {
      rows.push_back(
          {"search-compare", "Keep values", LabelOf(SEARCH_COMPARISONS, m_search_compare)});
    }
    if (m_search_source == SearchSource::Specific)
      rows.push_back({"search-value", "Value", m_search_value});
    rows.push_back({"search-run", first ? "Search" : "Search again",
                    !m_search_status.empty() ? m_search_status :
                    first                    ? std::string() :
                                               ResultCount(count)});
    if (m_search->WasFirstSearchDone() && count <= CHEAT_SEARCH_LISTED_RESULTS)
    {
      const auto results = m_search->ClonePartial(0, count);
      results->SetFilterType(Cheats::FilterType::DoNotFilter);
      const bool refreshed =
          results->RunSearch(Core::CPUThreadGuard{system}) == Cheats::SearchErrorCode::Success;
      const auto& source = refreshed ? *results : *m_search;
      for (size_t i = 0; i < count; ++i)
      {
        rows.push_back({fmt::format("result-{}", i),
                        fmt::format("Address {:08X}", m_search->GetResultAddress(i)),
                        source.GetResultValueAsString(i, false)});
      }
    }
    else if (m_search->WasFirstSearchDone())
    {
      rows.push_back({"search-count",
                      fmt::format("Results are listed once there are {} or fewer",
                                  CHEAT_SEARCH_LISTED_RESULTS),
                      {},
                      false});
    }
    rows.push_back({"search-reset", "Start over"});
    break;
  }
  case Page::FifoFrames:
    if (!FifoAnalysis::IsAvailable())
    {
      rows.push_back({"fifo-none", "No FIFO log is playing", {}, false});
      break;
    }
    for (u32 frame = 0; frame < FifoAnalysis::GetFrameCount(); ++frame)
    {
      rows.push_back({fmt::format("fifo-frame-{}", frame), fmt::format("Frame {}", frame),
                      fmt::format("{} objects", FifoAnalysis::GetObjectCount(frame))});
    }
    break;
  case Page::FifoObjects:
    for (size_t i = 0; i < m_fifo_objects.size(); ++i)
      rows.push_back({fmt::format("fifo-object-{}", i), m_fifo_objects[i].label});
    break;
  case Page::FifoCommands:
    for (size_t i = 0; i < m_fifo_commands.size(); ++i)
    {
      rows.push_back(
          {fmt::format("fifo-command-{}", i),
           fmt::format("{:08X}  {}", m_fifo_commands[i].offset, m_fifo_commands[i].text)});
    }
    break;
  case Page::Achievements:
  {
    m_achievements.clear();
    AchievementManager& achievements = AchievementManager::GetInstance();
    std::lock_guard lock(achievements.GetLock());
    rc_client_t* const client = achievements.GetClient();
    rc_client_achievement_list_t* const list =
        client ?
            rc_client_create_achievement_list(client,
                                              RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                              RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS) :
            nullptr;
    if (list)
    {
      for (u32 bucket_index = 0; bucket_index < list->num_buckets; ++bucket_index)
      {
        const rc_client_achievement_bucket_t& bucket = list->buckets[bucket_index];
        rows.push_back({fmt::format("bucket-{}", bucket_index), bucket.label, {}, false});
        for (u32 i = 0; i < bucket.num_achievements; ++i)
        {
          const rc_client_achievement_t& achievement = *bucket.achievements[i];
          const std::string progress = achievement.unlocked ? std::string("Unlocked") :
                                       achievement.measured_progress[0] != '\0' ?
                                                              achievement.measured_progress :
                                                              std::string();
          rows.push_back(
              {fmt::format("achievement-{}", m_achievements.size()),
               achievement.title ? achievement.title : "",
               progress.empty() ? fmt::format("{} points", achievement.points) : progress});
          m_achievements.push_back(
              {achievement.title ? achievement.title : "",
               fmt::format("{}\n\n{} points{}",
                           achievement.description ? achievement.description : "",
                           achievement.points, progress.empty() ? "" : " · " + progress)});
        }
      }
      rc_client_destroy_achievement_list(list);
    }
    if (m_achievements.empty())
      rows.push_back({"achievements-none", "This game has no achievements", {}, false});
    break;
  }
  case Page::Leaderboards:
  {
    m_leaderboards = Leaderboards::List();
    std::string group;
    for (size_t i = 0; i < m_leaderboards.size(); ++i)
    {
      const Leaderboards::Board& board = m_leaderboards[i];
      if (board.group != group)
      {
        group = board.group;
        rows.push_back({fmt::format("group-{}", i), group, {}, false});
      }
      rows.push_back({fmt::format("board-{}", i), board.title, board.tracker});
    }
    if (m_leaderboards.empty())
      rows.push_back({"boards-none", "This game has no leaderboards", {}, false});
    break;
  }
  case Page::Leaderboard:
  {
    if (m_leaderboard >= m_leaderboards.size())
      break;
    const Leaderboards::Board& board = m_leaderboards[m_leaderboard];
    if (!board.description.empty())
      rows.push_back({"board-description", board.description, {}, false});
    const Leaderboards::Entries entries = Leaderboards::Get(board.id);
    for (size_t i = 0; i < entries.entries.size(); ++i)
    {
      const Leaderboards::Entry& entry = entries.entries[i];
      rows.push_back({fmt::format("entry-{}", i),
                      fmt::format("{}. {}{}", entry.rank, entry.user, entry.player ? " (you)" : ""),
                      entry.score});
    }
    if (entries.entries.empty())
    {
      rows.push_back({"entries-none",
                      entries.loading ? "Loading" :
                      entries.failed  ? "The leaderboard could not be loaded" :
                                        "No one has a score yet",
                      {},
                      false});
    }
    break;
  }
  case Page::Controllers:
    rows.push_back({"mapping", "Controller mapping"});
    for (const int remote : GetEmulatedWiiRemotes())
    {
      const InputMappingHost::WiiRemoteOptions options =
          InputMappingHost::GetWiiRemoteOptions(remote);
      rows.push_back({fmt::format("orientation-{}", remote),
                      fmt::format("Wii Remote {} orientation", remote + 1),
                      OrientationLabel(options)});
      rows.push_back({fmt::format("extension-{}", remote),
                      fmt::format("Wii Remote {} extension", remote + 1), ExtensionLabel(options)});
    }
    break;
  case Page::Fifo:
    if (m_fifo_recording)
    {
      rows.push_back({"fifo-stop", "Stop recording"});
    }
    else
    {
      for (const s32 frames : FIFO_FRAME_COUNTS)
      {
        rows.push_back({fmt::format("fifo-{}", frames),
                        fmt::format("Record {} frame{}", frames, frames == 1 ? "" : "s")});
      }
    }
    break;
  }
  return rows;
}

UwpImGuiFrontend::QuickMenuModel QuickMenu::BuildModel()
{
  m_rows = BuildRows();

  UwpImGuiFrontend::QuickMenuModel model;
  model.windowId = "DolphinQuickMenu";
  switch (m_page)
  {
  case Page::Main:
    model.title = "Quick Menu";
    break;
  case Page::Cheats:
    model.title = "Cheats";
    break;
  case Page::GraphicsMods:
    model.title = "Graphics Mods";
    break;
  case Page::Skylanders:
    model.title = "Skylanders Portal";
    break;
  case Page::Infinity:
    model.title = "Infinity Base";
    break;
  case Page::Recording:
    model.title = "Recording";
    break;
  case Page::GBAs:
    model.title = "Game Boy Advance";
    break;
  case Page::GBA:
    model.title = fmt::format("GBA {}", m_gba_port + 1);
    break;
  case Page::Fifo:
    model.title = "FIFO Recording";
    break;
  case Page::Controllers:
    model.title = "Controllers";
    break;
  case Page::CheatSearch:
    model.title = "Cheat Search";
    break;
  case Page::Achievements:
    model.title = "Achievements";
    break;
  case Page::Leaderboards:
    model.title = "Leaderboards";
    break;
  case Page::Leaderboard:
    model.title = m_leaderboard < m_leaderboards.size() ? m_leaderboards[m_leaderboard].title :
                                                          std::string("Leaderboards");
    break;
  case Page::FifoFrames:
    model.title = "FIFO Analyzer";
    break;
  case Page::FifoObjects:
    model.title = fmt::format("Frame {}", m_fifo_frame);
    break;
  case Page::FifoCommands:
    model.title =
        m_fifo_object < m_fifo_objects.size() ?
            fmt::format("Frame {}, {}", m_fifo_frame, m_fifo_objects[m_fifo_object].label) :
            std::string("FIFO Analyzer");
    break;
  }
  model.footer = m_title;
  for (const Row& row : m_rows)
    model.rows.push_back({row.id, row.label, row.value, row.enabled});
  m_selected_row = m_rows.empty() ? 0 : std::min(m_selected_row, m_rows.size() - 1);
  model.selectedRow = m_selected_row;

  if (m_choice_row)
  {
    model.choicePopupOpen = true;
    const auto row = std::ranges::find(m_rows, *m_choice_row, &Row::id);
    model.choiceTitle = m_creating_figure               ? "Create a new figure" :
                        *m_choice_row == "search-value" ? fmt::format("Value: {}", m_search_value) :
                        row != m_rows.end()             ? row->label :
                                                          std::string();
    for (const Choice& choice : m_choices)
      model.choices.push_back({choice.id, choice.label, choice.enabled});
    model.selectedChoice = m_selected_choice;
  }
  return model;
}

std::vector<QuickMenu::Choice> QuickMenu::BuildChoices(const std::string& row, size_t* selected)
{
  std::vector<Choice> choices;
  *selected = 0;
  if (row == "save-state" || row == "load-state")
  {
    for (u32 slot = 1; slot <= STATE_SLOT_COUNT; ++slot)
    {
      choices.push_back({fmt::format("slot-{}", slot),
                         fmt::format("Slot {} - {}", slot, State::GetInfoStringOfSlot(slot, false)),
                         row == "save-state" || SlotExists(slot)});
    }
    *selected = std::clamp<size_t>(m_last_slot, 1, STATE_SLOT_COUNT) - 1;
    choices.push_back({"file", row == "save-state" ? "Save to a file" : "Load from a file"});
  }
  else if (row == "change-disc")
  {
    auto& system = Core::System::GetInstance();
    choices.push_back({"eject", "Eject the disc", system.GetDVDInterface().IsDiscInside()});
    choices.push_back({"file", "Choose a file"});
    m_disc_paths.clear();
    if (m_library)
    {
      const DiscIO::Platform platform =
          system.IsWii() ? DiscIO::Platform::WiiDisc : DiscIO::Platform::GameCubeDisc;
      const std::string& game_id = SConfig::GetInstance().GetGameID();
      std::vector<GameLibrary::GameEntry> discs;
      for (GameLibrary::GameEntry& entry : m_library->GetGameEntries())
      {
        if (entry.game->GetPlatform() == platform)
          discs.push_back(std::move(entry));
      }
      // Other discs of the running game come first.
      std::ranges::stable_sort(discs, {}, [&game_id](const GameLibrary::GameEntry& entry) {
        return std::tuple(entry.game->GetGameID() != game_id, entry.name,
                          entry.game->GetDiscNumber());
      });
      for (const GameLibrary::GameEntry& entry : discs)
      {
        const u8 disc = entry.game->GetDiscNumber();
        choices.push_back({fmt::format("disc-{}", m_disc_paths.size()),
                           disc > 0 || entry.game->GetGameID() == game_id ?
                               fmt::format("{} (Disc {})", entry.name, disc + 1) :
                               entry.name});
        m_disc_paths.push_back(entry.game->GetFilePath());
      }
    }
  }
  else if (row == "internal-resolution")
  {
    for (int scale = 0; scale <= MAX_RESOLUTION_CHOICE; ++scale)
      choices.push_back({fmt::format("scale-{}", scale), RESOLUTION_LABELS[scale]});
    *selected = static_cast<size_t>(
        std::clamp(Config::Get(Config::GFX_EFB_SCALE), 0, MAX_RESOLUTION_CHOICE));
  }
  else if (row == "emulation-speed")
  {
    // Hardcore mode does not allow slowing down.
    const bool hardcore = AchievementManager::GetInstance().IsHardcoreModeActive();
    for (const float speed : SPEED_CHOICES)
    {
      choices.push_back({fmt::format("speed-{}", std::lround(speed * 100.0f)), SpeedLabel(speed),
                         !hardcore || speed == 0.0f || speed >= 1.0f});
    }
    *selected = NearestSpeedChoice(Config::Get(Config::MAIN_EMULATION_SPEED));
  }
  else if (row == "netplay-chat")
  {
    for (size_t i = 0; i < CHAT_MESSAGES.size(); ++i)
      choices.push_back({fmt::format("chat-{}", i), CHAT_MESSAGES[i]});
  }
  else if (row == "search-type")
  {
    for (size_t i = 0; i < SEARCH_TYPES.size(); ++i)
    {
      choices.push_back({fmt::format("type-{}", i), SEARCH_TYPES[i].second});
      if (SEARCH_TYPES[i].first == m_search_type)
        *selected = i;
    }
  }
  else if (row == "search-source")
  {
    choices.push_back(
        {"previous", "The previous value", m_search && m_search->WasFirstSearchDone()});
    choices.push_back({"specific", "A value"});
    choices.push_back(
        {"any", "Any value (first search)", !m_search || !m_search->WasFirstSearchDone()});
    *selected = static_cast<size_t>(m_search_source == SearchSource::Previous ? 0 :
                                    m_search_source == SearchSource::Specific ? 1 :
                                                                                2);
  }
  else if (row == "search-compare")
  {
    for (size_t i = 0; i < SEARCH_COMPARISONS.size(); ++i)
    {
      choices.push_back({fmt::format("compare-{}", i), SEARCH_COMPARISONS[i].second});
      if (SEARCH_COMPARISONS[i].first == m_search_compare)
        *selected = i;
    }
  }
  else if (row == "search-value")
  {
    for (size_t i = 0; i < NUMBER_PAD.size(); ++i)
      choices.push_back({fmt::format("digit-{}", i), NUMBER_PAD[i]});
    choices.push_back({"erase", "Erase the last character"});
    choices.push_back({"clear", "Clear"});
    choices.push_back({"done", "Done"});
  }
  else if (row == "gba-layout")
  {
    const Config::GBALayout current = Config::Get(Config::UWP_GBA_LAYOUT);
    for (size_t i = 0; i < GBA_LAYOUTS.size(); ++i)
    {
      choices.push_back({fmt::format("layout-{}", i), GBA_LAYOUTS[i].second});
      if (GBA_LAYOUTS[i].first == current)
        *selected = i;
    }
  }
  else if (row == "gba-scale")
  {
    for (size_t i = 0; i < GBA_SCALES.size(); ++i)
      choices.push_back({fmt::format("scale-{}", i), GBA_SCALES[i]});
    *selected = static_cast<size_t>(std::clamp(Config::Get(Config::UWP_GBA_SCALE), 0, 4));
  }
  else if (row == "gba-volume")
  {
    for (int volume = 0; volume <= 100; volume += 10)
      choices.push_back({fmt::format("volume-{}", volume), fmt::format("{}%", volume)});
    if (const std::optional<GBAScreens::Status> status = GBAScreens::GetStatus(m_gba_port))
      *selected = static_cast<size_t>(std::clamp(status->volume, 0, 100) / 10);
  }
  else if (row == "gba-own-scale")
  {
    choices.push_back({"scale-default", "Default"});
    for (size_t i = 0; i < GBA_SCALES.size(); ++i)
      choices.push_back({fmt::format("scale-{}", i), GBA_SCALES[i]});
    *selected =
        static_cast<size_t>(std::clamp(Config::Get(Config::UWP_GBA_SCALES[m_gba_port]), -1, 4) + 1);
  }
  else if (row == "gba-own-blending")
  {
    choices.push_back({"blending-default", "Default"});
    choices.push_back({"blending-on", "On"});
    choices.push_back({"blending-off", "Off"});
    const int blending = Config::Get(Config::UWP_GBA_BLENDING[m_gba_port]);
    *selected = blending < 0 ? 0 : (blending > 0 ? 1 : 2);
  }
  else if (row.starts_with("orientation-"))
  {
    const InputMappingHost::WiiRemoteOptions options = InputMappingHost::GetWiiRemoteOptions(
        static_cast<int>(ParseIndex(row, "orientation-").value_or(0)));
    choices.push_back({"normal", "Normal"});
    choices.push_back({"sideways", "Sideways"});
    choices.push_back({"upright", "Upright"});
    *selected = options.sideways == options.upright ? 0 : (options.sideways ? 1 : 2);
  }
  else if (row.starts_with("extension-"))
  {
    const InputMappingHost::WiiRemoteOptions options = InputMappingHost::GetWiiRemoteOptions(
        static_cast<int>(ParseIndex(row, "extension-").value_or(0)));
    const auto extensions = InputMappingHost::GetWiiRemoteExtensions();
    for (size_t i = 0; i < extensions.size(); ++i)
    {
      choices.push_back({extensions[i].first, extensions[i].second});
      if (extensions[i].first == options.type_id)
        *selected = i;
    }
  }
  else if (row.starts_with("result-"))
  {
    choices.push_back({"code", "Freeze this value"});
    choices.push_back({"remove", "Remove from the results"});
  }
  else if (row.starts_with("sky-") || row.starts_with("inf-"))
  {
    const bool skylander = row.starts_with("sky-");
    const u8 slot = static_cast<u8>(ParseIndex(row, skylander ? "sky-" : "inf-").value_or(0));
    if (m_creating_figure)
    {
      m_figure_types = skylander ? Portals::ListSkylanderTypes() : Portals::ListInfinityTypes(slot);
      for (const Portals::FigureType& type : m_figure_types)
        choices.push_back({fmt::format("type-{}", type.id), type.name});
    }
    else
    {
      const std::string current =
          skylander ? Portals::GetSkylanderName(slot) : Portals::GetInfinityName(slot);
      m_figure_files = skylander ? Portals::ListSkylanderFiles() : Portals::ListInfinityFiles();
      choices.push_back({"remove", "Remove the figure", !current.empty()});
      choices.push_back({"create", "Create a new figure"});
      for (size_t i = 0; i < m_figure_files.size(); ++i)
        choices.push_back({fmt::format("file-{}", i), m_figure_files[i].name});
      *selected = current.empty() ? 1 : 0;
    }
  }
  return choices;
}

QuickMenu::Effect QuickMenu::OpenPage(Page page)
{
  m_return_rows.push_back({m_page, m_selected_row});
  m_page = page;
  m_selected_row = 0;
  const SConfig& config = SConfig::GetInstance();
  if (page == Page::Cheats)
    m_cheats.Load(config.GetGameID(), config.GetRevision());
  else if (page == Page::GraphicsMods)
    m_mods.Load(config.GetGameID());
  return Effect::Refocus;
}

QuickMenu::Effect QuickMenu::OpenChoices(const std::string& row)
{
  m_choice_row = row;
  m_creating_figure = false;
  m_choices = BuildChoices(row, &m_selected_choice);
  return Effect::Refocus;
}

QuickMenu::Effect QuickMenu::Back()
{
  if (m_choice_row)
  {
    if (m_creating_figure)
    {
      m_creating_figure = false;
      m_choices = BuildChoices(*m_choice_row, &m_selected_choice);
      m_selected_choice = 1;
      return Effect::Refocus;
    }
    m_choice_row.reset();
    m_choices.clear();
    return Effect::Refocus;
  }
  if (!m_return_rows.empty())
  {
    std::tie(m_page, m_selected_row) = m_return_rows.back();
    m_return_rows.pop_back();
    m_cheats = {};
    m_mods.Clear();
    return Effect::Refocus;
  }
  return Effect::Close;
}

QuickMenu::Effect QuickMenu::ActivateMain(const std::string& id)
{
  auto& system = Core::System::GetInstance();
  if (id == "resume")
    return Effect::Close;
  if (id == "save-state" || id == "load-state" || id == "change-disc" ||
      id == "internal-resolution" || id == "emulation-speed" || id == "netplay-chat")
  {
    return OpenChoices(id);
  }
  if (id == "show-fps")
    Toggle(Config::GFX_SHOW_FPS);
  else if (id == "free-look")
    Toggle(Config::FREE_LOOK_ENABLED);
  else if (id == "cheats")
    return OpenPage(Page::Cheats);
  else if (id == "graphics-mods")
    return OpenPage(Page::GraphicsMods);
  else if (id == "skylanders")
    return OpenPage(Page::Skylanders);
  else if (id == "infinity")
    return OpenPage(Page::Infinity);
  else if (id == "recording")
    return OpenPage(Page::Recording);
  else if (id == "gba")
    return OpenPage(Page::GBAs);
  else if (id == "fifo")
    return OpenPage(Page::Fifo);
  else if (id == "cheat-search")
    return OpenPage(Page::CheatSearch);
  else if (id == "achievements")
    return OpenPage(Page::Achievements);
  else if (id == "leaderboards")
    return OpenPage(Page::Leaderboards);
  else if (id == "fifo-analyzer")
    return OpenPage(Page::FifoFrames);
  else if (id == "controllers")
    return GetEmulatedWiiRemotes().empty() ? Effect::OpenMapper : OpenPage(Page::Controllers);
  else if (id == "debugger")
    return Effect::OpenDebugger;
  else if (id == "reset")
  {
    system.GetProcessorInterface().ResetButton_Tap();
    return Effect::Close;
  }
  else if (id == "exit")
  {
    return Effect::Exit;
  }
  return Effect::None;
}

QuickMenu::Effect QuickMenu::Activate(size_t row)
{
  if (row >= m_rows.size() || m_choice_row)
    return Effect::None;
  m_selected_row = row;
  const std::string id = m_rows[row].id;
  auto& system = Core::System::GetInstance();

  switch (m_page)
  {
  case Page::Main:
    return ActivateMain(id);
  case Page::Cheats:
    if (id == "cheats-enabled")
    {
      Toggle(Config::MAIN_ENABLE_CHEATS);
      if (Config::AreCheatsEnabled())
        m_cheats.ApplyToRunningGame();
    }
    else if (const auto index = ParseIndex(id, "gecko-"); index && *index < m_cheats.gecko.size())
    {
      m_cheats.gecko[*index].enabled = !m_cheats.gecko[*index].enabled;
      m_cheats.Save();
      m_cheats.ApplyToRunningGame();
    }
    else if (const auto ar = ParseIndex(id, "ar-"); ar && *ar < m_cheats.action_replay.size())
    {
      m_cheats.action_replay[*ar].enabled = !m_cheats.action_replay[*ar].enabled;
      m_cheats.Save();
      m_cheats.ApplyToRunningGame();
    }
    return Effect::None;
  case Page::GraphicsMods:
    if (id == "mods-enabled")
    {
      Toggle(Config::GFX_MODS_ENABLE);
    }
    else if (const auto index = ParseIndex(id, "mod-"))
    {
      const std::vector<GraphicsModList::Mod> mods = m_mods.GetMods();
      if (*index < mods.size())
        m_mods.SetEnabled(mods[*index].path, !mods[*index].enabled);
    }
    return Effect::None;
  case Page::Skylanders:
  case Page::Infinity:
    return OpenChoices(id);
  case Page::GBAs:
    if (const auto port = ParseIndex(id, "gba-port-"); port && GBAScreens::IsRunning(*port))
    {
      m_gba_port = *port;
      return OpenPage(Page::GBA);
    }
    if (id == "gba-layout" || id == "gba-scale")
      return OpenChoices(id);
    if (id == "gba-frame")
      Toggle(Config::UWP_GBA_FRAME);
    else if (id == "gba-blending")
      Toggle(Config::UWP_GBA_INTERFRAME_BLENDING);
    else if (id == "gba-show-streamed")
      Toggle(Config::UWP_GBA_SHOW_STREAMED);
    return Effect::None;
  case Page::GBA:
  {
    const std::optional<GBAScreens::Status> status = GBAScreens::GetStatus(m_gba_port);
    if (!status)
      return Effect::None;
    if (id == "gba-connected")
      GBAScreens::SetConnected(m_gba_port, !status->connected);
    else if (id == "gba-reset")
      GBAScreens::Reset(m_gba_port);
    else if (id == "gba-load-rom")
      return Effect::PickGBAROM;
    else if (id == "gba-unload-rom")
      GBAScreens::LoadROM(m_gba_port, {});
    else if (id == "gba-ereader")
      return Effect::PickEReaderCard;
    else if (id == "gba-import-save")
      return Effect::PickGBASaveToImport;
    else if (id == "gba-export-save")
      return Effect::PickGBASaveFolder;
    else if (id == "gba-import-state")
      return Effect::PickGBAStateToImport;
    else if (id == "gba-export-state")
      return Effect::PickGBAStateFolder;
    else if (id == "gba-mute")
      GBAScreens::SetMuted(m_gba_port, !status->muted);
    else if (id == "gba-volume" || id == "gba-own-scale" || id == "gba-own-blending")
      return OpenChoices(id);
    else if (id == "gba-on-top")
      Toggle(Config::UWP_GBA_ALWAYS_ON_TOP[m_gba_port]);
    return Effect::None;
  }
  case Page::Recording:
  {
    auto& movie = system.GetMovie();
    if (id == "record-start")
    {
      if (Recordings::BeginMovieRecording())
        ShowMessage("Input recording started");
      else
        ShowMessage("Input recording could not be started");
    }
    else if (id == "record-save")
    {
      const std::string path = Recordings::SaveMovieRecording();
      movie.EndPlayInput(false);
      ShowMessage(path.empty() ? std::string("The recording could not be saved") :
                                 fmt::format("Saved the recording to {}", path));
    }
    else if (id == "playback-stop")
    {
      movie.EndPlayInput(false);
    }
    else if (id == "read-only")
    {
      movie.SetReadOnly(!movie.IsReadOnly());
    }
    else if (const auto tas = ParseIndex(id, "tas-"); tas && *tas < m_tas_targets.size())
    {
      m_tas_target = m_tas_targets[*tas];
      return Effect::OpenTasInput;
    }
    else if (id == "dump-frames")
    {
      Toggle(Config::MAIN_MOVIE_DUMP_FRAMES);
      if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
        ShowMessage(fmt::format("Frames are saved to {}", File::GetUserPath(D_DUMPFRAMES_IDX)));
    }
    else if (id == "dump-audio")
    {
      Toggle(Config::MAIN_DUMP_AUDIO);
      if (Config::Get(Config::MAIN_DUMP_AUDIO))
        ShowMessage(fmt::format("Audio is saved to {}", File::GetUserPath(D_DUMPAUDIO_IDX)));
    }
    return Effect::None;
  }
  case Page::FifoFrames:
    if (const auto frame = ParseIndex(id, "fifo-frame-"); frame && FifoAnalysis::IsAvailable())
    {
      m_fifo_frame = static_cast<u32>(*frame);
      m_fifo_objects = FifoAnalysis::GetObjects(m_fifo_frame);
      return OpenPage(Page::FifoObjects);
    }
    return Effect::None;
  case Page::FifoObjects:
    if (const auto index = ParseIndex(id, "fifo-object-");
        index && *index < m_fifo_objects.size() && FifoAnalysis::IsAvailable())
    {
      m_fifo_object = *index;
      m_fifo_commands = FifoAnalysis::GetCommands(m_fifo_objects[m_fifo_object]);
      return OpenPage(Page::FifoCommands);
    }
    return Effect::None;
  case Page::FifoCommands:
    if (const auto index = ParseIndex(id, "fifo-command-");
        index && *index < m_fifo_commands.size() && m_fifo_object < m_fifo_objects.size() &&
        FifoAnalysis::IsAvailable())
    {
      std::string description =
          FifoAnalysis::Describe(m_fifo_objects[m_fifo_object], m_fifo_commands[*index].offset);
      if (description.size() > FIFO_DESCRIPTION_LIMIT)
      {
        description.resize(FIFO_DESCRIPTION_LIMIT);
        description += "...";
      }
      PromptService::GetInstance().Inform(fmt::format("{:08X}", m_fifo_commands[*index].offset),
                                          std::move(description));
    }
    return Effect::None;
  case Page::Achievements:
    if (const auto index = ParseIndex(id, "achievement-"); index && *index < m_achievements.size())
    {
      PromptService::GetInstance().Inform(m_achievements[*index].first,
                                          m_achievements[*index].second);
    }
    return Effect::None;
  case Page::Leaderboards:
    if (const auto index = ParseIndex(id, "board-"); index && *index < m_leaderboards.size())
    {
      m_leaderboard = *index;
      Leaderboards::Fetch(m_leaderboards[m_leaderboard].id);
      return OpenPage(Page::Leaderboard);
    }
    return Effect::None;
  case Page::Leaderboard:
    return Effect::None;
  case Page::Controllers:
    if (id == "mapping")
      return Effect::OpenMapper;
    if (id.starts_with("orientation-") || id.starts_with("extension-"))
      return OpenChoices(id);
    return Effect::None;
  case Page::CheatSearch:
    if (id == "search-start")
      StartCheatSearch();
    else if (id == "search-run")
      RunCheatSearch();
    else if (id == "search-reset")
    {
      m_search.reset();
      m_search_status.clear();
    }
    else if (id == "search-type" || id == "search-source" || id == "search-compare" ||
             id == "search-value" || id.starts_with("result-"))
    {
      return OpenChoices(id);
    }
    return Effect::Refocus;
  case Page::Fifo:
    if (id == "fifo-stop")
    {
      system.GetFifoRecorder().StopRecording();
      return Effect::Close;
    }
    if (const auto frames = ParseIndex(id, "fifo-"))
    {
      StartFifoRecording(static_cast<s32>(*frames));
      return Effect::Close;
    }
    return Effect::None;
  }
  return Effect::None;
}

QuickMenu::Effect QuickMenu::Choose(size_t index)
{
  if (!m_choice_row || index >= m_choices.size())
    return Effect::None;
  m_selected_choice = index;
  Effect effect = Effect::Refocus;
  const std::string row = *m_choice_row;
  const Choice choice = m_choices[index];
  ApplyChoice(row, choice, &effect);
  return effect;
}

void QuickMenu::ApplyChoice(const std::string& row, const Choice& choice, Effect* effect)
{
  auto& system = Core::System::GetInstance();
  const auto close_choices = [this] {
    m_choice_row.reset();
    m_choices.clear();
    m_creating_figure = false;
  };

  if ((row == "save-state" || row == "load-state") && choice.id == "file")
  {
    close_choices();
    *effect = row == "save-state" ? Effect::PickStateToSave : Effect::PickStateToLoad;
    return;
  }
  if (row == "save-state" || row == "load-state")
  {
    m_last_slot = static_cast<u32>(m_selected_choice) + 1;
    close_choices();
    if (row == "save-state")
    {
      State::Save(system, m_last_slot);
    }
    else
    {
      State::Load(system, m_last_slot);
      *effect = Effect::Close;
    }
    return;
  }
  if (row == "change-disc")
  {
    close_choices();
    if (choice.id == "file")
    {
      *effect = Effect::PickDisc;
      return;
    }
    if (choice.id == "eject")
    {
      system.GetDVDInterface().EjectDisc(Core::CPUThreadGuard{system}, DVD::EjectCause::User);
      *effect = Effect::Close;
      return;
    }
    if (const auto disc = ParseIndex(choice.id, "disc-"); disc && *disc < m_disc_paths.size())
    {
      system.GetDVDInterface().ChangeDisc(Core::CPUThreadGuard{system}, m_disc_paths[*disc]);
      ShowMessage(fmt::format("Changing the disc to {}", choice.label));
      *effect = Effect::Close;
    }
    return;
  }
  if (row == "gba-layout")
  {
    Config::SetBaseOrCurrent(
        Config::UWP_GBA_LAYOUT,
        GBA_LAYOUTS[std::min(m_selected_choice, GBA_LAYOUTS.size() - 1)].first);
    Config::Save();
    close_choices();
    return;
  }
  if (row == "gba-scale")
  {
    Config::SetBaseOrCurrent(Config::UWP_GBA_SCALE,
                             static_cast<int>(std::min<size_t>(m_selected_choice, 4)));
    Config::Save();
    close_choices();
    return;
  }
  if (row == "gba-volume")
  {
    GBAScreens::SetVolume(m_gba_port, static_cast<int>(m_selected_choice) * 10);
    close_choices();
    return;
  }
  if (row == "gba-own-scale")
  {
    Config::SetBaseOrCurrent(Config::UWP_GBA_SCALES[m_gba_port],
                             static_cast<int>(std::min<size_t>(m_selected_choice, 5)) - 1);
    Config::Save();
    close_choices();
    return;
  }
  if (row == "gba-own-blending")
  {
    Config::SetBaseOrCurrent(Config::UWP_GBA_BLENDING[m_gba_port],
                             m_selected_choice == 0 ? -1 : (m_selected_choice == 1 ? 1 : 0));
    Config::Save();
    close_choices();
    return;
  }
  if (row == "internal-resolution")
  {
    Config::SetBaseOrCurrent(Config::GFX_EFB_SCALE, std::clamp(static_cast<int>(m_selected_choice),
                                                               0, MAX_RESOLUTION_CHOICE));
    Config::Save();
    close_choices();
    return;
  }
  if (row == "emulation-speed")
  {
    Config::SetBaseOrCurrent(Config::MAIN_EMULATION_SPEED,
                             SPEED_CHOICES[std::min(m_selected_choice, SPEED_CHOICES.size() - 1)]);
    Config::Save();
    close_choices();
    return;
  }
  if (row == "netplay-chat")
  {
    if (m_chat_sender && m_selected_choice < CHAT_MESSAGES.size())
      m_chat_sender(CHAT_MESSAGES[m_selected_choice]);
    close_choices();
    return;
  }

  if (row == "search-type")
  {
    if (const auto index = ParseIndex(choice.id, "type-"); index && *index < SEARCH_TYPES.size())
    {
      if (SEARCH_TYPES[*index].first != m_search_type)
      {
        m_search_type = SEARCH_TYPES[*index].first;
        m_search.reset();
        m_search_status.clear();
      }
    }
    close_choices();
    return;
  }
  if (row == "search-source")
  {
    m_search_source = choice.id == "previous" ? SearchSource::Previous :
                      choice.id == "any"      ? SearchSource::Any :
                                                SearchSource::Specific;
    close_choices();
    return;
  }
  if (row == "search-compare")
  {
    if (const auto index = ParseIndex(choice.id, "compare-");
        index && *index < SEARCH_COMPARISONS.size())
    {
      m_search_compare = SEARCH_COMPARISONS[*index].first;
    }
    close_choices();
    return;
  }
  if (row == "search-value")
  {
    if (choice.id == "done")
    {
      if (m_search_value.empty())
        m_search_value = "0";
      close_choices();
    }
    else if (choice.id == "erase")
    {
      if (!m_search_value.empty())
        m_search_value.pop_back();
    }
    else if (choice.id == "clear")
    {
      m_search_value.clear();
    }
    else if (const auto digit = ParseIndex(choice.id, "digit-");
             digit && *digit < NUMBER_PAD.size())
    {
      if (m_search_value == "0")
        m_search_value.clear();
      if (m_search_value.size() < 24)
        m_search_value += NUMBER_PAD[*digit];
    }
    return;
  }
  if (const auto remote = ParseIndex(row, "orientation-"))
  {
    close_choices();
    const UwpImGuiFrontend::MappingSaveResult saved = InputMappingHost::SetWiiRemoteOrientation(
        static_cast<int>(*remote), choice.id == "sideways", choice.id == "upright");
    if (!saved.succeeded)
      ShowMessage(saved.error);
    return;
  }
  if (const auto remote = ParseIndex(row, "extension-"))
  {
    close_choices();
    const UwpImGuiFrontend::MappingSaveResult saved =
        InputMappingHost::SetWiiRemoteExtension(static_cast<int>(*remote), choice.id);
    if (!saved.succeeded)
      ShowMessage(saved.error);
    return;
  }
  if (row.starts_with("result-"))
  {
    const auto index = ParseIndex(row, "result-");
    close_choices();
    if (!index || !m_search || *index >= m_search->GetResultCount())
      return;
    if (choice.id == "code")
      CreateCheatSearchCode(*index);
    else if (choice.id == "remove")
      m_search->RemoveResult(*index);
    return;
  }

  const bool skylander = row.starts_with("sky-");
  if (!skylander && !row.starts_with("inf-"))
  {
    close_choices();
    return;
  }
  const u8 slot = static_cast<u8>(ParseIndex(row, skylander ? "sky-" : "inf-").value_or(0));
  std::string error;
  if (choice.id == "create")
  {
    m_creating_figure = true;
    m_choices = BuildChoices(row, &m_selected_choice);
    return;
  }
  if (choice.id == "remove")
  {
    if (skylander)
      error = Portals::ClearSkylander(slot);
    else
      Portals::ClearInfinity(slot);
  }
  else if (const auto type = ParseIndex(choice.id, "type-"))
  {
    error = skylander ? Portals::CreateSkylander(slot, static_cast<u32>(*type)) :
                        Portals::CreateInfinity(slot, static_cast<u32>(*type));
  }
  else if (const auto file = ParseIndex(choice.id, "file-"); file && *file < m_figure_files.size())
  {
    const std::string& path = m_figure_files[*file].path;
    error = skylander ? Portals::LoadSkylander(slot, path) : Portals::LoadInfinity(slot, path);
  }
  if (!error.empty())
    ShowMessage(error);
  close_choices();
}

void QuickMenu::StartCheatSearch()
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  std::vector<Cheats::MemoryRange> ranges;
  ranges.emplace_back(0x80000000, memory.GetRamSizeReal());
  if (system.IsWii())
    ranges.emplace_back(0x90000000, memory.GetExRamSizeReal());
  m_search = Cheats::MakeSession(std::move(ranges), PowerPC::RequestedAddressSpace::Virtual, true,
                                 m_search_type);
  m_search_source = SearchSource::Specific;
  m_search_status.clear();
}

void QuickMenu::RunCheatSearch()
{
  if (!m_search)
    return;

  switch (m_search_source)
  {
  case SearchSource::Previous:
    if (!m_search->WasFirstSearchDone())
    {
      m_search_status = "Search once before comparing with previous values";
      return;
    }
    m_search->SetFilterType(Cheats::FilterType::CompareAgainstLastValue);
    break;
  case SearchSource::Specific:
    m_search->SetFilterType(Cheats::FilterType::CompareAgainstSpecificValue);
    if (!m_search->SetValueFromString(m_search_value, false))
    {
      m_search_status = "The value does not fit the value type";
      return;
    }
    break;
  case SearchSource::Any:
    m_search->SetFilterType(Cheats::FilterType::DoNotFilter);
    break;
  }
  m_search->SetCompareType(m_search_compare);

  auto& system = Core::System::GetInstance();
  switch (m_search->RunSearch(Core::CPUThreadGuard{system}))
  {
  case Cheats::SearchErrorCode::Success:
    m_search_status.clear();
    if (m_search_source == SearchSource::Any)
      m_search_source = SearchSource::Previous;
    break;
  case Cheats::SearchErrorCode::VirtualAddressesCurrentlyNotAccessible:
    m_search_status = "Memory is not accessible right now. Play for a moment and try again";
    break;
  case Cheats::SearchErrorCode::DisabledInHardcoreMode:
    m_search_status = "Cheat search is disabled in hardcore mode";
    break;
  default:
    m_search_status = "The search failed";
    break;
  }
}

void QuickMenu::CreateCheatSearchCode(size_t index)
{
  auto code = Cheats::GenerateActionReplayCode(*m_search, index);
  if (!code)
  {
    ShowMessage("An Action Replay code could not be created for this address");
    return;
  }

  const SConfig& config = SConfig::GetInstance();
  GameCheats cheats;
  cheats.Load(config.GetGameID(), config.GetRevision());
  cheats.action_replay.push_back(std::move(*code));
  cheats.Save();
  cheats.ApplyToRunningGame();
  ShowMessage(Config::AreCheatsEnabled() ? std::string("Added the code to this game's cheats") :
                                           std::string("Added the code. Enable cheats to use it"));
}

void QuickMenu::StartFifoRecording(s32 frames)
{
  m_fifo_finished = false;
  m_fifo_recording = true;
  Core::System::GetInstance().GetFifoRecorder().StartRecording(frames,
                                                               [this] { m_fifo_finished = true; });
  ShowMessage(
      fmt::format("Recording {} frame{} of graphics commands", frames, frames == 1 ? "" : "s"));
}

bool QuickMenu::ConsumeChanged()
{
  return Leaderboards::ConsumeChanged() && m_page == Page::Leaderboard;
}

void QuickMenu::Update()
{
  if (!m_fifo_recording || !m_fifo_finished.exchange(false))
    return;
  m_fifo_recording = false;

  FifoDataFile* const file = Core::System::GetInstance().GetFifoRecorder().GetRecordedFile();
  if (!file)
  {
    ShowMessage("The FIFO recording failed");
    return;
  }
  const std::string directory = Recordings::GetFifoDirectory();
  File::CreateFullPath(directory);
  const std::string path =
      Recordings::MakeFileName(directory, SConfig::GetInstance().GetGameID(), ".dff");
  ShowMessage(file->Save(path) ? fmt::format("Saved the FIFO log to {}", path) :
                                 std::string("The FIFO log could not be saved"));
}
}  // namespace DolphinUWP

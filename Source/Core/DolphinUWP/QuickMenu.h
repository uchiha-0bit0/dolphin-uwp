// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <UwpImGuiFrontend/Views.h>

#include "Common/CommonTypes.h"
#include "Core/CheatSearch.h"
#include "DolphinUWP/FifoAnalysis.h"
#include "DolphinUWP/GameCheats.h"
#include "DolphinUWP/GraphicsMods.h"
#include "DolphinUWP/Leaderboards.h"
#include "DolphinUWP/Portals.h"
#include "DolphinUWP/TasView.h"

namespace DolphinUWP
{
class GameLibrary;

// The pages and rows of the in-game menu. Runs on the UI thread.
class QuickMenu
{
public:
  enum class Effect
  {
    None,
    Refocus,
    Close,
    Exit,
    OpenDebugger,
    PickDisc,
    PickStateToLoad,
    PickStateToSave,
    PickGBAROM,
    PickEReaderCard,
    PickGBASaveToImport,
    PickGBASaveFolder,
    PickGBAStateToImport,
    PickGBAStateFolder,
    OpenTasInput,
    OpenMapper,
  };

  using ChatSender = std::function<void(const std::string&)>;

  void SetChatSender(ChatSender sender) { m_chat_sender = std::move(sender); }
  void SetLibrary(const GameLibrary* library) { m_library = library; }
  void SetTitle(std::string title) { m_title = std::move(title); }
  void SetExitLabel(std::string label) { m_exit_label = std::move(label); }

  void Open();
  // Opens the menu at the page or choices of a main-page row, when that row is available.
  Effect OpenAt(std::string_view id);
  UwpImGuiFrontend::QuickMenuModel BuildModel();

  Effect Activate(size_t row);
  Effect Choose(size_t choice);
  Effect Back();

  // Saves FIFO logs whose recording finished. Call regularly while a game runs.
  void Update();
  // True when the open page shows information that changed since it was built.
  bool ConsumeChanged();

  u32 GetSelectedSlot() const { return m_last_slot; }
  void SetSelectedSlot(u32 slot) { m_last_slot = slot; }
  // The controller port of the GBA that the GBA page and its file pickers act on.
  size_t GetGBAPort() const { return m_gba_port; }
  void SetGBAPort(size_t port) { m_gba_port = port; }
  // The controller that the TAS input panel opens for.
  TasView::Target GetTasTarget() const { return m_tas_target; }

private:
  enum class Page
  {
    Main,
    Cheats,
    GraphicsMods,
    Skylanders,
    Infinity,
    Recording,
    Fifo,
    CheatSearch,
    Achievements,
    FifoFrames,
    FifoObjects,
    FifoCommands,
    GBAs,
    GBA,
    Leaderboards,
    Leaderboard,
    Controllers,
  };

  enum class SearchSource
  {
    Previous,
    Specific,
    Any,
  };

  struct Row
  {
    std::string id;
    std::string label;
    std::string value;
    bool enabled = true;
  };

  struct Choice
  {
    std::string id;
    std::string label;
    bool enabled = true;
  };

  std::vector<Row> BuildRows();
  std::vector<Choice> BuildChoices(const std::string& row, size_t* selected);
  Effect OpenPage(Page page);
  Effect OpenChoices(const std::string& row);
  Effect ActivateMain(const std::string& id);
  void ApplyChoice(const std::string& row, const Choice& choice, Effect* effect);
  void StartFifoRecording(s32 frames);
  void StartCheatSearch();
  void RunCheatSearch();
  void CreateCheatSearchCode(size_t index);
  static void ShowMessage(std::string message);

  ChatSender m_chat_sender;
  const GameLibrary* m_library = nullptr;
  std::string m_title;
  std::string m_exit_label = "Exit to Library";

  Page m_page = Page::Main;
  std::vector<Row> m_rows;
  size_t m_selected_row = 0;
  std::optional<std::string> m_choice_row;
  std::vector<Choice> m_choices;
  size_t m_selected_choice = 0;
  bool m_creating_figure = false;
  std::vector<std::pair<Page, size_t>> m_return_rows;

  u32 m_last_slot = 1;
  GameCheats m_cheats;
  GraphicsModList m_mods;
  std::vector<Portals::FigureFile> m_figure_files;
  std::vector<std::string> m_disc_paths;
  std::vector<Portals::FigureType> m_figure_types;
  std::atomic<bool> m_fifo_finished = false;
  bool m_fifo_recording = false;

  std::unique_ptr<Cheats::CheatSearchSessionBase> m_search;
  // Title and details of each listed achievement.
  std::vector<std::pair<std::string, std::string>> m_achievements;
  u32 m_fifo_frame = 0;
  std::vector<FifoAnalysis::Object> m_fifo_objects;
  size_t m_fifo_object = 0;
  std::vector<FifoAnalysis::Command> m_fifo_commands;
  Cheats::DataType m_search_type = Cheats::DataType::U32;
  Cheats::CompareType m_search_compare = Cheats::CompareType::Equal;
  SearchSource m_search_source = SearchSource::Specific;
  std::string m_search_value = "0";
  std::string m_search_status;
  size_t m_gba_port = 0;
  std::vector<Leaderboards::Board> m_leaderboards;
  size_t m_leaderboard = 0;
  std::vector<TasView::Target> m_tas_targets;
  TasView::Target m_tas_target;
};
}  // namespace DolphinUWP

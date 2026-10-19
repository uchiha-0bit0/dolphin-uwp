// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <UwpImGuiFrontend/Input.h>

#include "Common/CommonTypes.h"

struct ImFont;

namespace UwpImGuiFrontend
{
struct ShellTheme;
}

namespace DolphinUWP
{
// What the in-game debugger shows. Built on the UI thread, drawn on the video thread.
struct DebuggerSnapshot
{
  struct CodeLine
  {
    u32 address = 0;
    std::string symbol;
    std::string mnemonic;
    std::string operands;
    bool breakpoint = false;
    bool pc = false;
    bool patched = false;
  };

  struct ListEntry
  {
    std::string label;
    std::string value;
  };

  int tab = 0;
  bool stepping = false;
  bool commands_focused = false;
  size_t command = 0;
  std::vector<std::string> commands;
  std::string status;
  // A line above the content of the tab, such as the number of branch candidates.
  std::string header;
  std::vector<CodeLine> code;
  size_t code_selected = 0;
  std::vector<ListEntry> registers;
  size_t register_selected = 0;
  u32 memory_address = 0;
  std::vector<std::optional<u8>> memory;
  std::vector<bool> memory_breakpoints;
  size_t memory_selected = 0;
  size_t memory_column = 0;
  // A window of a long list: the entries from list_first, of list_total in all.
  std::vector<ListEntry> list;
  size_t list_first = 0;
  size_t list_total = 0;
  size_t list_selected = 0;
  // Plain text, such as the source and result of the assembler.
  std::vector<std::string> text_lines;
  // An entry is open while its title is set.
  std::string entry_title;
  std::vector<std::string> entry_cells;
  size_t entry_cursor = 0;
  // A list of choices is open while its title is set.
  std::string choice_title;
  std::vector<std::string> choices;
  size_t choice_selected = 0;
  // Text being typed is open while its title is set.
  std::string text_title;
  std::string text;
  size_t text_caret = 0;
  bool text_multiline = false;
  std::string hint;
};

class DebuggerView
{
public:
  enum class Effect
  {
    None,
    Close,
  };

  struct PickRequest
  {
    bool folder = false;
    std::vector<std::wstring> file_types;
    std::wstring token;
    std::function<void(const std::string& path)> picked;
  };

  void Open();
  // Ends the text input that the debugger may have started.
  void Close();
  // Handles one frame of edge-triggered input. Runs on the UI thread.
  Effect HandleInput(const UwpImGuiFrontend::FrameInput& input);
  // Called when the CPU stopped, for example at a breakpoint.
  void OnCPUStopped();
  bool ConsumeChanged();
  DebuggerSnapshot BuildSnapshot();
  std::optional<PickRequest> TakePickRequest() { return std::exchange(m_pick, std::nullopt); }

  void Step();
  void StepOver();
  void StepOut();
  void Skip();
  void ShowPC();
  void ToggleBreakpointAtPC();
  // Toggles a memory breakpoint on the selected word, or asks for an address outside the memory
  // view.
  void AddMemoryBreakpoint();

  static void Draw(const DebuggerSnapshot& snapshot, const UwpImGuiFrontend::ShellTheme& theme,
                   ImFont* font);

private:
  enum class Tab
  {
    Code,
    Registers,
    Memory,
    Breakpoints,
    Watches,
    CallStack,
    Symbols,
    Threads,
    BranchWatch,
    JIT,
    Network,
    Assembler,
  };

  enum class Command
  {
    Continue,
    Step,
    StepOver,
    StepOut,
    Skip,
    GoToPC,
    GoToAddress,
    FindValue,
    AddWatch,
    AddMemoryBreakpoint,
    SetCondition,
    AssembleInstruction,
    ReplaceWithNop,
    RestoreInstruction,
    RenameSymbol,
    FollowBranch,
    FindSymbol,
    GenerateSymbols,
    LoadSymbols,
    SaveSymbols,
    ClearSymbols,
    StartBranchWatch,
    CodePathTaken,
    CodePathNotTaken,
    BranchOverwritten,
    BranchNotOverwritten,
    WipeRecentHits,
    ClearBranchWatch,
    BranchWatchFile,
    ClearJitCache,
    JitProfiling,
    WipeProfileData,
    EditSource,
    BaseAddress,
    Assemble,
    Inject,
    AddAsCode,
    AssemblerFile,
  };

  enum class EntryPurpose
  {
    GoToAddress,
    EditMemory,
    EditRegister,
    FindValue,
    AddWatch,
    AddMemoryBreakpoint,
    Condition,
    AssemblerBase,
  };

  enum class ChoicePurpose
  {
    GenerateSymbols,
    LoadSymbols,
    SaveSymbols,
    ClearSymbols,
    RsoModule,
    BranchWatchFile,
    AddAsCode,
    AssemblerFile,
  };

  enum class TextPurpose
  {
    AssembleInstruction,
    RenameSymbol,
    FindSymbol,
    EditSource,
  };

  // Digits and other choices edited one cell at a time with the directional buttons.
  struct Entry
  {
    EntryPurpose purpose = EntryPurpose::GoToAddress;
    std::string title;
    std::vector<std::span<const char* const>> cells;
    std::vector<size_t> values;
    size_t cursor = 0;
    u32 target = 0;
  };

  struct Choice
  {
    ChoicePurpose purpose = ChoicePurpose::GenerateSymbols;
    std::string title;
    std::vector<std::string> items;
    std::vector<u32> values;
    size_t selected = 0;
  };

  struct TextEdit
  {
    TextPurpose purpose = TextPurpose::RenameSymbol;
    std::string title;
    u32 target = 0;
  };

  void RunCommand(Command command);
  Effect Continue();
  void ShowCode(u32 address);
  void ShowMemory(u32 address);
  void MoveSelection(int delta);
  void MoveColumn(int delta);
  void Activate();
  void Secondary();
  bool IsStepping() const;
  bool IsListTab() const;
  std::vector<Command> GetCommands() const;
  std::string GetCommandLabel(Command command) const;
  u32 SelectedCodeAddress() const;

  void OpenHexEntry(EntryPurpose purpose, std::string title, u32 value, u32 target = 0);
  void OpenConditionEntry(u32 address);
  void ConfirmEntry();
  void OpenChoice(ChoicePurpose purpose, std::string title, std::vector<std::string> items,
                  std::vector<u32> values = {});
  void ConfirmChoice();
  void OpenText(TextPurpose purpose, std::string title, std::string text, u32 target = 0);
  void HandleTextInput(const UwpImGuiFrontend::FrameInput& input);
  void ConfirmText(std::string text);
  void Pick(bool folder, std::vector<std::wstring> file_types, std::wstring token,
            std::function<void(const std::string&)> picked);

  u32 SelectedMemoryAddress() const;
  void ToggleMemoryBreakpoint(u32 address);
  void AddMemoryBreakpointAt(u32 address);
  void SetCondition(u32 address, std::optional<std::string> condition);
  void FindValue(u32 value);
  void WriteRegister(size_t index, u32 value);

  void AssembleInstruction(u32 address, const std::string& text);
  void SetInstruction(u32 address, u32 instruction);
  void RestoreInstruction(u32 address);
  void FollowBranch();
  void RenameSymbol(u32 address, const std::string& name);
  void GenerateSymbols(size_t choice);
  void LoadSymbols(size_t choice);
  void SaveSymbols(size_t choice);
  void StartRSOSearch();
  void ApplyRSOModule(u32 address);
  void BranchWatchFile(size_t choice);
  void AssembleSource();
  void InjectSource();
  void AddSourceAsCode(size_t choice);
  void AssemblerFile(size_t choice);

  Tab m_tab = Tab::Code;
  bool m_commands_focused = false;
  size_t m_command = 0;
  u32 m_code_address = 0;
  size_t m_code_selected = 0;
  size_t m_register_selected = 0;
  u32 m_memory_address = 0x80000000;
  size_t m_memory_selected = 0;
  size_t m_memory_column = 0;
  u32 m_last_find = 0;
  size_t m_list_selected = 0;
  std::vector<u32> m_list_addresses;
  std::vector<u32> m_list_second_addresses;
  std::vector<bool> m_list_code_breakpoints;
  std::optional<Entry> m_entry;
  std::optional<Choice> m_choice;
  std::optional<TextEdit> m_text;
  std::optional<PickRequest> m_pick;
  std::string m_symbol_filter;
  std::future<std::vector<std::pair<u32, std::string>>> m_rso_search;
  std::string m_assembler_source;
  u32 m_assembler_base = 0x80000000;
  std::vector<std::string> m_assembler_result;
  bool m_changed = true;
};
}  // namespace DolphinUWP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/DebuggerView.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/ImGuiShell.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/Debug/Threads.h"
#include "Common/Debug/Watches.h"
#include "Common/Event.h"
#include "Common/FileUtil.h"
#include "Common/GekkoDisassembler.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Common/SymbolDB.h"
#include "Common/UWP/Device.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/Debugger/RSO.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/AddressSpace.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Expression.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCAnalyst.h"
#include "Core/PowerPC/PPCSymbolDB.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/PowerPC/SignatureDB/SignatureDB.h"
#include "Core/System.h"
#include "DolphinUWP/DebuggerNetwork.h"
#include "DolphinUWP/GameCheats.h"
#include "DolphinUWP/GameTextInput.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;
using Common::GekkoAssembler::CodeBlock;

constexpr size_t CODE_LINES = 20;
constexpr size_t MEMORY_ROWS = 16;
constexpr u32 MEMORY_ROW_BYTES = 16;
constexpr u32 MEMORY_WORD_BYTES = 4;
constexpr size_t MEMORY_ROW_WORDS = MEMORY_ROW_BYTES / MEMORY_WORD_BYTES;
constexpr size_t REGISTER_COLUMNS = 4;
constexpr size_t INTEGER_REGISTERS = 40;
constexpr size_t HEX_DIGITS_PER_WORD = 8;
// The entries of a long list that a snapshot carries, around the selection.
constexpr size_t LIST_WINDOW = 64;
constexpr int LIST_PAGE = 12;
constexpr size_t ASSEMBLER_SOURCE_LINES = 14;
constexpr u32 NOP = 0x60000000;
constexpr std::array<const char*, 12> TAB_NAMES{
    "Code",    "Registers", "Memory",       "Breakpoints", "Watches", "Call Stack",
    "Symbols", "Threads",   "Branch Watch", "JIT",         "Network", "Assembler"};

constexpr std::array<const char*, 16> HEX_DIGITS{"0", "1", "2", "3", "4", "5", "6", "7",
                                                 "8", "9", "A", "B", "C", "D", "E", "F"};
constexpr std::array<const char*, 36> CONDITION_REGISTERS{
    "None", "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10",
    "r11",  "r12", "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22",
    "r23",  "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31", "pc",  "lr",  "ctr"};
constexpr std::array<const char*, 6> COMPARISONS{"==", "!=", "<", "<=", ">", ">="};

ImU32 Color(const ImVec4& color)
{
  return ImGui::ColorConvertFloat4ToU32(color);
}

// Returns true on rfi, blr or a bclr whose condition holds.
bool WillInstructionReturn(Core::System& system, UGeckoInstruction inst)
{
  if (inst.hex == 0x4C000064u)
    return true;
  const auto& ppc_state = system.GetPPCState();
  const bool counter =
      (inst.BO_2 >> 2 & 1) != 0 || (CTR(ppc_state) != 0) != ((inst.BO_2 >> 1 & 1) != 0);
  const bool condition =
      inst.BO_2 >> 4 != 0 || ppc_state.cr.GetBit(inst.BI_2) == (inst.BO_2 >> 3 & 1);
  const bool bclr = inst.OPCD_7 == 0b010011 && inst.XO == 16;
  return bclr && counter && condition && !inst.LK_3;
}

const char* RegisterName(size_t index)
{
  static constexpr std::array<const char*, 8> special{"pc",  "lr",  "ctr",  "cr",
                                                      "xer", "msr", "srr0", "srr1"};
  static const std::array<std::string, 32> general = [] {
    std::array<std::string, 32> names;
    for (size_t i = 0; i < names.size(); ++i)
      names[i] = fmt::format("r{}", i);
    return names;
  }();
  return index < general.size() ? general[index].c_str() : special[index - general.size()];
}

u32 ReadRegister(const PowerPC::PowerPCState& ppc_state, size_t index)
{
  if (index < 32)
    return ppc_state.gpr[index];
  switch (index)
  {
  case 32:
    return ppc_state.pc;
  case 33:
    return LR(ppc_state);
  case 34:
    return CTR(ppc_state);
  case 35:
    return ppc_state.cr.Get();
  case 36:
    return ppc_state.GetXER().Hex;
  case 37:
    return ppc_state.msr.Hex;
  case 38:
    return ppc_state.spr[SPR_SRR0];
  default:
    return ppc_state.spr[SPR_SRR1];
  }
}

const char* ThreadStateName(u16 state)
{
  switch (state)
  {
  case 1:
    return "Ready";
  case 2:
    return "Running";
  case 4:
    return "Waiting";
  case 8:
    return "Moribund";
  default:
    return "Unknown";
  }
}

bool ContainsIgnoringCase(std::string_view text, std::string_view part)
{
  return std::ranges::search(
             text, part,
             [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
             }).begin() != text.end() ||
         part.empty();
}

std::string Disassemble(const Core::CPUThreadGuard& guard, u32 address)
{
  const auto instruction = PowerPC::MMU::HostTryReadInstruction(guard, address);
  if (!instruction)
    return {};
  std::string text = Common::GekkoDisassembler::Disassemble(instruction->value, address);
  std::ranges::replace(text, '\t', ' ');
  return text;
}

// The 32-bit words of a block, with the last one padded with zeros.
std::vector<u32> BlockWords(const CodeBlock& block)
{
  std::vector<u32> words;
  for (size_t i = 0; i < block.instructions.size(); i += 4)
  {
    u32 word = 0;
    for (size_t j = 0; j < 4; ++j)
    {
      const size_t index = i + j;
      word = (word << 8) | (index < block.instructions.size() ? block.instructions[index] : 0);
    }
    words.push_back(word);
  }
  return words;
}

// Gecko and Action Replay codes that write the assembled bytes.
std::string WriteCode(const std::vector<CodeBlock>& blocks)
{
  std::string text;
  for (const CodeBlock& block : blocks)
  {
    const std::vector<u8>& bytes = block.instructions;
    size_t i = 0;
    for (; i + 3 < bytes.size(); i += 4)
    {
      text += fmt::format("{:08X} {:02X}{:02X}{:02X}{:02X}\n",
                          ((block.block_address + i) & 0x1FFFFFF) | 0x04000000, bytes[i],
                          bytes[i + 1], bytes[i + 2], bytes[i + 3]);
    }
    for (; i < bytes.size(); ++i)
      text += fmt::format("{:08X} 000000{:02X}\n", (block.block_address + i) & 0x1FFFFFF, bytes[i]);
  }
  return text;
}

// A Gecko code that runs the assembled code in place of the instruction at its address.
std::string InsertCode(const std::vector<CodeBlock>& blocks)
{
  std::string text;
  for (const CodeBlock& block : blocks)
  {
    if (block.instructions.empty())
      continue;
    const size_t size = block.instructions.size();
    const bool extra_line = size % 8 == 0 || size % 8 > 4;
    const u32 lines = 1 + static_cast<u32>((size - 1) / 8) + (extra_line ? 1 : 0);
    text += fmt::format("C{:07X} {:08X}\n", (block.block_address & 0x1FFFFFF) | 0x02000000, lines);
    for (const u32 word : BlockWords(block))
      text += fmt::format("{:08X} ", word);
    text += extra_line ? "60000000 00000000\n" : "00000000\n";
  }
  return text;
}

// Finds the RSO modules in memory by the file names that they keep.
std::vector<std::pair<u32, std::string>> DetectRSOModules()
{
  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  constexpr std::array<std::string_view, 2> search_for{".elf", ".plf"};
  const AddressSpace::Accessors* const accessors =
      AddressSpace::GetAccessors(AddressSpace::Type::Effective);
  std::vector<std::pair<u32, std::string>> matches;
  for (const std::string_view extension : search_for)
  {
    u32 next = 0;
    while (true)
    {
      auto found = accessors->Search(guard, next, reinterpret_cast<const u8*>(extension.data()),
                                     extension.size() + 1, true);
      if (!found)
        break;
      next = *found + 1;

      constexpr u32 MODULE_NAME_MAX_LENGTH = 260;
      u32 max_length = 0;
      for (; max_length < MODULE_NAME_MAX_LENGTH; ++max_length)
      {
        if (!std::isprint(PowerPC::MMU::HostRead<u8>(guard, *found - (max_length + 1))))
          break;
      }

      bool matched = false;
      u32 name_length = 0;
      for (u32 i = 0; i < max_length; ++i)
      {
        const u32 lookup = (*found - max_length) + i;
        const std::array<u8, 4> reference{static_cast<u8>(lookup >> 24),
                                          static_cast<u8>(lookup >> 16),
                                          static_cast<u8>(lookup >> 8), static_cast<u8>(lookup)};
        const auto offset_field =
            accessors->Search(guard, lookup, reference.data(), reference.size(), false);
        if (!offset_field)
          continue;
        name_length = accessors->ReadU32(guard, *offset_field + 4);
        if (name_length == max_length - i + extension.length())
        {
          found = offset_field;
          matched = true;
          break;
        }
      }
      if (!matched)
        continue;

      const u32 name_offset = accessors->ReadU32(guard, *found);
      matches.emplace_back(*found - 16,
                           PowerPC::MMU::HostGetString(guard, name_offset, name_length));
    }
  }
  return matches;
}

std::string GameID()
{
  return SConfig::GetInstance().GetGameID();
}
}  // namespace

bool DebuggerView::IsStepping() const
{
  return Core::System::GetInstance().GetCPU().IsStepping();
}

bool DebuggerView::IsListTab() const
{
  switch (m_tab)
  {
  case Tab::Breakpoints:
  case Tab::Watches:
  case Tab::CallStack:
  case Tab::Symbols:
  case Tab::Threads:
  case Tab::BranchWatch:
  case Tab::JIT:
  case Tab::Network:
    return true;
  default:
    return false;
  }
}

std::vector<DebuggerView::Command> DebuggerView::GetCommands() const
{
  std::vector<Command> commands{Command::Continue,   Command::Step, Command::StepOver,
                                Command::StepOut,    Command::Skip, Command::GoToPC,
                                Command::GoToAddress};
  switch (m_tab)
  {
  case Tab::Code:
    commands.insert(commands.end(),
                    {Command::AssembleInstruction, Command::ReplaceWithNop,
                     Command::RestoreInstruction, Command::RenameSymbol, Command::FollowBranch});
    break;
  case Tab::Memory:
    commands.push_back(Command::FindValue);
    commands.push_back(Command::AddWatch);
    break;
  case Tab::Breakpoints:
    commands.push_back(Command::AddMemoryBreakpoint);
    commands.push_back(Command::SetCondition);
    break;
  case Tab::Watches:
    commands.push_back(Command::AddWatch);
    break;
  case Tab::Symbols:
    commands = {Command::Continue,        Command::FindSymbol,  Command::RenameSymbol,
                Command::GenerateSymbols, Command::LoadSymbols, Command::SaveSymbols,
                Command::ClearSymbols};
    break;
  case Tab::Threads:
    commands = {Command::Continue, Command::Step, Command::GoToPC};
    break;
  case Tab::BranchWatch:
    commands = {
        Command::Continue,         Command::StartBranchWatch,  Command::CodePathTaken,
        Command::CodePathNotTaken, Command::BranchOverwritten, Command::BranchNotOverwritten,
        Command::WipeRecentHits,   Command::ClearBranchWatch,  Command::BranchWatchFile};
    break;
  case Tab::JIT:
    commands = {Command::Continue, Command::ClearJitCache, Command::JitProfiling,
                Command::WipeProfileData};
    break;
  case Tab::Network:
    commands = {Command::Continue};
    break;
  case Tab::Assembler:
    commands = {Command::Continue, Command::EditSource, Command::BaseAddress,  Command::Assemble,
                Command::Inject,   Command::AddAsCode,  Command::AssemblerFile};
    break;
  default:
    break;
  }
  return commands;
}

std::string DebuggerView::GetCommandLabel(Command command) const
{
  switch (command)
  {
  case Command::Continue:
    return "Continue";
  case Command::Step:
    return "Step";
  case Command::StepOver:
    return "Step Over";
  case Command::StepOut:
    return "Step Out";
  case Command::Skip:
    return "Skip";
  case Command::GoToPC:
    return "Go to PC";
  case Command::GoToAddress:
    return "Go to address";
  case Command::FindValue:
    return "Find value";
  case Command::AddWatch:
    return "Watch";
  case Command::AddMemoryBreakpoint:
    return "Memory breakpoint";
  case Command::SetCondition:
    return "Condition";
  case Command::AssembleInstruction:
    return "Assemble";
  case Command::ReplaceWithNop:
    return "Replace with nop";
  case Command::RestoreInstruction:
    return "Restore";
  case Command::RenameSymbol:
    return "Rename";
  case Command::FollowBranch:
    return "Follow branch";
  case Command::FindSymbol:
    return m_symbol_filter.empty() ? "Find" : fmt::format("Find: {}", m_symbol_filter);
  case Command::GenerateSymbols:
    return "Generate";
  case Command::LoadSymbols:
    return "Load";
  case Command::SaveSymbols:
    return "Save";
  case Command::ClearSymbols:
    return "Clear";
  case Command::StartBranchWatch:
    return Core::System::GetInstance().GetPowerPC().GetBranchWatch().GetRecordingActive() ?
               "Pause" :
               "Start";
  case Command::CodePathTaken:
    return "Code path was taken";
  case Command::CodePathNotTaken:
    return "Code path not taken";
  case Command::BranchOverwritten:
    return "Branch was overwritten";
  case Command::BranchNotOverwritten:
    return "Branch not overwritten";
  case Command::WipeRecentHits:
    return "Wipe recent hits";
  case Command::ClearBranchWatch:
    return "Clear";
  case Command::BranchWatchFile:
    return "Save or load";
  case Command::ClearJitCache:
    return "Clear cache";
  case Command::JitProfiling:
    return Config::Get(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING) ? "Stop profiling" :
                                                                  "Start profiling";
  case Command::WipeProfileData:
    return "Wipe profiling data";
  case Command::EditSource:
    return "Edit";
  case Command::BaseAddress:
    return "Base address";
  case Command::Assemble:
    return "Assemble";
  case Command::Inject:
    return "Inject";
  case Command::AddAsCode:
    return "Add as a code";
  case Command::AssemblerFile:
    return "Open or save";
  }
  return {};
}

void DebuggerView::Open()
{
  auto& system = Core::System::GetInstance();
  if (!IsStepping())
    Core::SetState(system, Core::State::Paused);
  m_commands_focused = false;
  m_command = 0;
  m_entry.reset();
  m_choice.reset();
  m_text.reset();
  ShowPC();
}

void DebuggerView::Close()
{
  m_text.reset();
  GameTextInput::End();
}

void DebuggerView::OnCPUStopped()
{
  if (m_tab == Tab::Code)
    ShowPC();
  m_changed = true;
}

bool DebuggerView::ConsumeChanged()
{
  return std::exchange(m_changed, false);
}

void DebuggerView::ShowPC()
{
  ShowCode(Core::System::GetInstance().GetPPCState().pc);
}

void DebuggerView::ShowCode(u32 address)
{
  m_tab = Tab::Code;
  m_code_address = (address & ~3u) - static_cast<u32>(CODE_LINES / 2) * 4;
  m_code_selected = CODE_LINES / 2;
  m_changed = true;
}

void DebuggerView::ShowMemory(u32 address)
{
  m_tab = Tab::Memory;
  m_memory_address = address & ~(MEMORY_ROW_BYTES - 1);
  m_memory_selected = 0;
  m_memory_column = (address % MEMORY_ROW_BYTES) / MEMORY_WORD_BYTES;
  m_changed = true;
}

u32 DebuggerView::SelectedMemoryAddress() const
{
  return m_memory_address + static_cast<u32>(m_memory_selected) * MEMORY_ROW_BYTES +
         static_cast<u32>(m_memory_column) * MEMORY_WORD_BYTES;
}

u32 DebuggerView::SelectedCodeAddress() const
{
  return m_code_address + static_cast<u32>(m_code_selected) * 4;
}

void DebuggerView::MoveSelection(int delta)
{
  switch (m_tab)
  {
  case Tab::Code:
  {
    s64 index = static_cast<s64>(m_code_selected) + delta;
    if (index < 0)
    {
      m_code_address -= static_cast<u32>(-index) * 4;
      index = 0;
    }
    else if (index >= static_cast<s64>(CODE_LINES))
    {
      m_code_address += static_cast<u32>(index - static_cast<s64>(CODE_LINES) + 1) * 4;
      index = CODE_LINES - 1;
    }
    m_code_selected = static_cast<size_t>(index);
    break;
  }
  case Tab::Registers:
    m_register_selected = static_cast<size_t>(std::clamp<s64>(
        static_cast<s64>(m_register_selected) + delta * static_cast<s64>(REGISTER_COLUMNS), 0,
        static_cast<s64>(INTEGER_REGISTERS) - 1));
    break;
  case Tab::Memory:
  {
    s64 index = static_cast<s64>(m_memory_selected) + delta;
    if (index < 0)
    {
      m_memory_address -= static_cast<u32>(-index) * MEMORY_ROW_BYTES;
      index = 0;
    }
    else if (index >= static_cast<s64>(MEMORY_ROWS))
    {
      m_memory_address +=
          static_cast<u32>(index - static_cast<s64>(MEMORY_ROWS) + 1) * MEMORY_ROW_BYTES;
      index = MEMORY_ROWS - 1;
    }
    m_memory_selected = static_cast<size_t>(index);
    break;
  }
  case Tab::Assembler:
    break;
  default:
    if (!m_list_addresses.empty())
    {
      m_list_selected =
          static_cast<size_t>(std::clamp<s64>(static_cast<s64>(m_list_selected) + delta, 0,
                                              static_cast<s64>(m_list_addresses.size()) - 1));
    }
    break;
  }
  m_changed = true;
}

void DebuggerView::MoveColumn(int delta)
{
  switch (m_tab)
  {
  case Tab::Code:
    MoveSelection(delta * static_cast<int>(CODE_LINES));
    break;
  case Tab::Registers:
    m_register_selected = static_cast<size_t>(std::clamp<s64>(
        static_cast<s64>(m_register_selected) + delta, 0, static_cast<s64>(INTEGER_REGISTERS) - 1));
    break;
  case Tab::Memory:
  {
    const s64 column = static_cast<s64>(m_memory_column) + delta;
    if (column < 0)
    {
      m_memory_column = MEMORY_ROW_WORDS - 1;
      MoveSelection(-1);
    }
    else if (column >= static_cast<s64>(MEMORY_ROW_WORDS))
    {
      m_memory_column = 0;
      MoveSelection(1);
    }
    else
    {
      m_memory_column = static_cast<size_t>(column);
    }
    break;
  }
  default:
    // Long lists move a page at a time.
    if (IsListTab())
      MoveSelection(delta * LIST_PAGE);
    break;
  }
  m_changed = true;
}

void DebuggerView::Step()
{
  auto& system = Core::System::GetInstance();
  auto& cpu = system.GetCPU();
  if (!cpu.IsStepping())
    return;
  Common::Event sync_event;
  auto& power_pc = system.GetPowerPC();
  const PowerPC::CoreMode old_mode = power_pc.GetMode();
  power_pc.SetMode(PowerPC::CoreMode::Interpreter);
  cpu.StepOpcode(&sync_event);
  sync_event.WaitFor(std::chrono::milliseconds(20));
  power_pc.SetMode(old_mode);
  ShowPC();
}

void DebuggerView::StepOver()
{
  auto& system = Core::System::GetInstance();
  auto& cpu = system.GetCPU();
  if (!cpu.IsStepping())
    return;
  const UGeckoInstruction inst = [&] {
    const Core::CPUThreadGuard guard(system);
    return PowerPC::MMU::HostRead_Instruction(guard, system.GetPPCState().pc);
  }();
  if (inst.LK)
  {
    system.GetPowerPC().GetBreakPoints().SetTemporary(system.GetPPCState().pc + 4);
    cpu.SetStepping(false);
    m_changed = true;
    return;
  }
  Step();
}

void DebuggerView::StepOut()
{
  auto& system = Core::System::GetInstance();
  if (!system.GetCPU().IsStepping())
    return;

  using Clock = std::chrono::steady_clock;
  const Clock::time_point timeout = Clock::now() + std::chrono::seconds(5);
  auto& power_pc = system.GetPowerPC();
  auto& ppc_state = power_pc.GetPPCState();
  {
    const Core::CPUThreadGuard guard(system);
    const PowerPC::CoreMode old_mode = power_pc.GetMode();
    power_pc.SetMode(PowerPC::CoreMode::Interpreter);
    UGeckoInstruction inst = PowerPC::MMU::HostRead_Instruction(guard, ppc_state.pc);
    do
    {
      if (WillInstructionReturn(system, inst))
      {
        power_pc.SingleStep();
        break;
      }
      if (inst.LK)
      {
        const u32 next_pc = ppc_state.pc + 4;
        do
        {
          power_pc.SingleStep();
        } while (ppc_state.pc != next_pc && Clock::now() < timeout && !power_pc.CheckBreakPoints());
      }
      else
      {
        power_pc.SingleStep();
      }
      inst = PowerPC::MMU::HostRead_Instruction(guard, ppc_state.pc);
    } while (Clock::now() < timeout && !power_pc.CheckBreakPoints());
    power_pc.SetMode(old_mode);
  }

  if (power_pc.CheckBreakPoints())
    OSD::AddMessage("A breakpoint stopped the step out");
  else if (Clock::now() >= timeout)
    OSD::AddMessage("Step out timed out");
  ShowPC();
}

void DebuggerView::Skip()
{
  if (IsStepping())
    Core::System::GetInstance().GetPPCState().pc += 4;
  ShowPC();
}

void DebuggerView::ToggleBreakpointAtPC()
{
  auto& system = Core::System::GetInstance();
  system.GetPowerPC().GetBreakPoints().ToggleBreakPoint(system.GetPPCState().pc);
  m_changed = true;
}

void DebuggerView::AddMemoryBreakpoint()
{
  if (m_tab == Tab::Memory && IsStepping())
  {
    ToggleMemoryBreakpoint(SelectedMemoryAddress());
    return;
  }
  OpenHexEntry(EntryPurpose::AddMemoryBreakpoint, "Memory breakpoint at", SelectedMemoryAddress());
}

void DebuggerView::ToggleMemoryBreakpoint(u32 address)
{
  auto& memchecks = Core::System::GetInstance().GetPowerPC().GetMemChecks();
  const bool exists =
      std::ranges::any_of(memchecks.GetMemChecks(), [address](const TMemCheck& check) {
        return check.start_address == address;
      });
  if (exists)
    memchecks.Remove(address);
  else
    AddMemoryBreakpointAt(address);
  m_changed = true;
}

void DebuggerView::AddMemoryBreakpointAt(u32 address)
{
  TMemCheck check;
  check.start_address = address;
  check.end_address = address + MEMORY_WORD_BYTES - 1;
  check.is_ranged = true;
  check.is_break_on_read = true;
  check.is_break_on_write = true;
  check.break_on_hit = true;
  Core::System::GetInstance().GetPowerPC().GetMemChecks().Add(std::move(check));
  m_changed = true;
}

void DebuggerView::SetCondition(u32 address, std::optional<std::string> condition)
{
  auto& breakpoints = Core::System::GetInstance().GetPowerPC().GetBreakPoints();
  const TBreakPoint* const breakpoint = breakpoints.GetRegularBreakpoint(address);
  if (!breakpoint)
    return;

  std::optional<Expression> expression;
  if (condition)
  {
    expression = Expression::TryParse(*condition);
    if (!expression)
    {
      OSD::AddMessage("The condition is not valid");
      return;
    }
  }
  const bool break_on_hit = breakpoint->break_on_hit;
  const bool log_on_hit = breakpoint->log_on_hit;
  breakpoints.Remove(address);
  breakpoints.Add(address, break_on_hit, log_on_hit, std::move(expression));
  m_changed = true;
}

void DebuggerView::FindValue(u32 value)
{
  m_last_find = value;
  auto& memory = Core::System::GetInstance().GetMemory();
  const std::array<u8, 4> pattern{static_cast<u8>(value >> 24), static_cast<u8>(value >> 16),
                                  static_cast<u8>(value >> 8), static_cast<u8>(value)};

  struct Region
  {
    u32 base;
    const u8* data;
    u32 size;
  };
  std::vector<Region> regions{{0x80000000, memory.GetRAM(), memory.GetRamSizeReal()}};
  if (memory.GetEXRAM())
    regions.push_back({0x90000000, memory.GetEXRAM(), memory.GetExRamSizeReal()});

  // Searches after the selected word and wraps around to the start of memory.
  const u32 start = SelectedMemoryAddress() + 1;
  const auto current = std::ranges::find_if(regions, [start](const Region& region) {
    return start >= region.base && start - region.base < region.size;
  });
  const size_t first =
      current != regions.end() ? static_cast<size_t>(current - regions.begin()) : 0;
  for (size_t i = 0; i <= regions.size(); ++i)
  {
    const Region& region = regions[(first + i) % regions.size()];
    if (!region.data)
      continue;
    const u32 offset = i == 0 && current != regions.end() ? start - region.base : 0;
    const u32 end =
        i == regions.size() && current != regions.end() ? start - region.base : region.size;
    if (offset >= end)
      continue;
    const auto found =
        std::search(region.data + offset, region.data + end, pattern.begin(), pattern.end());
    if (found != region.data + end)
    {
      ShowMemory(region.base + static_cast<u32>(found - region.data));
      return;
    }
  }
  OSD::AddMessage(fmt::format("{:08X} was not found", value));
}

void DebuggerView::WriteRegister(size_t index, u32 value)
{
  auto& system = Core::System::GetInstance();
  if (!IsStepping())
    return;
  auto& ppc_state = system.GetPPCState();
  if (index < 32)
  {
    ppc_state.gpr[index] = value;
    return;
  }
  switch (index)
  {
  case 32:
    ppc_state.pc = value;
    break;
  case 33:
    LR(ppc_state) = value;
    break;
  case 34:
    CTR(ppc_state) = value;
    break;
  case 35:
    ppc_state.cr.Set(value);
    break;
  case 36:
    ppc_state.SetXER(UReg_XER(value));
    break;
  case 37:
    ppc_state.msr.Hex = value;
    system.GetPowerPC().MSRUpdated();
    break;
  case 38:
    ppc_state.spr[SPR_SRR0] = value;
    break;
  default:
    ppc_state.spr[SPR_SRR1] = value;
    break;
  }
}

void DebuggerView::SetInstruction(u32 address, u32 instruction)
{
  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  system.GetPowerPC().GetDebugInterface().SetPatch(guard, address, instruction);
  m_changed = true;
}

void DebuggerView::RestoreInstruction(u32 address)
{
  auto& system = Core::System::GetInstance();
  auto& debug_interface = system.GetPowerPC().GetDebugInterface();
  if (!debug_interface.HasEnabledPatch(address))
  {
    OSD::AddMessage(fmt::format("The instruction at {:08X} isn't patched", address));
    return;
  }
  const Core::CPUThreadGuard guard(system);
  debug_interface.UnsetPatch(guard, address);
  m_changed = true;
}

void DebuggerView::AssembleInstruction(u32 address, const std::string& text)
{
  const auto result = Common::GekkoAssembler::Assemble(text, address);
  if (Common::GekkoAssembler::IsFailure(result))
  {
    OSD::AddMessage(fmt::format("The instruction could not be assembled: {}",
                                Common::GekkoAssembler::GetFailure(result).message));
    return;
  }
  for (const CodeBlock& block : Common::GekkoAssembler::GetT(result))
  {
    if (block.block_address == address && block.instructions.size() == 4)
    {
      SetInstruction(address, BlockWords(block).front());
      return;
    }
  }
  OSD::AddMessage("Enter one instruction to assemble");
}

void DebuggerView::FollowBranch()
{
  auto& system = Core::System::GetInstance();
  const std::string text = [&] {
    const Core::CPUThreadGuard guard(system);
    return Disassemble(guard, SelectedCodeAddress());
  }();
  // The disassembly names the target of a branch.
  const size_t target = text.find("->0x");
  u32 address = 0;
  if (target == std::string::npos || !TryParse(text.substr(target + 2, 10), &address))
  {
    OSD::AddMessage("The instruction isn't a branch to an address");
    return;
  }
  ShowCode(address);
}

void DebuggerView::RenameSymbol(u32 address, const std::string& name)
{
  auto& symbols = Core::System::GetInstance().GetPowerPC().GetSymbolDB();
  const Common::Symbol* const symbol = symbols.GetSymbolFromAddr(address);
  const std::string trimmed(StripWhitespace(name));
  if (!symbol || trimmed.empty())
    return;
  symbols.RenameSymbol(*symbol, trimmed);
  Host_PPCSymbolsChanged();
  m_changed = true;
}

void DebuggerView::GenerateSymbols(size_t choice)
{
  if (choice == 2)
  {
    StartRSOSearch();
    return;
  }

  auto& system = Core::System::GetInstance();
  auto& symbols = system.GetPowerPC().GetSymbolDB();
  {
    const Core::CPUThreadGuard guard(system);
    PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR,
                              Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal(),
                              &symbols);
    if (choice == 1)
    {
      SignatureDB db(SignatureDB::HandlerType::DSY);
      if (db.Load(File::GetSysDirectory() + TOTALDB))
      {
        db.Apply(guard, &symbols);
        OSD::AddMessage(fmt::format("Generated symbol names from {}", TOTALDB));
      }
      else
      {
        OSD::AddMessage(fmt::format("{} was not found, so no names were generated", TOTALDB));
      }
    }
  }
  Host_PPCSymbolsChanged();
  m_changed = true;
}

void DebuggerView::StartRSOSearch()
{
  if (m_rso_search.valid())
    return;
  m_rso_search = std::async(std::launch::async, &DetectRSOModules);
  OSD::AddMessage("Looking for RSO modules");
}

void DebuggerView::ApplyRSOModule(u32 address)
{
  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  RSOChainView chain;
  if (!chain.Load(guard, address))
  {
    OSD::AddMessage(fmt::format("The RSO module at {:08X} could not be loaded", address));
    return;
  }
  chain.Apply(guard, &system.GetPowerPC().GetSymbolDB());
  Host_PPCSymbolsChanged();
  m_changed = true;
}

void DebuggerView::LoadSymbols(size_t choice)
{
  auto& system = Core::System::GetInstance();
  const auto load_map = [this](const std::string& path, bool bad) {
    auto& map_system = Core::System::GetInstance();
    if (!map_system.GetPowerPC().GetSymbolDB().LoadMap(Core::CPUThreadGuard{map_system}, path, bad))
    {
      OSD::AddMessage(fmt::format("The map file {} could not be loaded", path));
      return;
    }
    HLE::PatchFunctions(map_system);
    Host_PPCSymbolsChanged();
    OSD::AddMessage(fmt::format("Loaded symbols from {}", path));
    m_changed = true;
  };

  switch (choice)
  {
  case 0:
  {
    std::string existing;
    std::string writable;
    if (PPCSymbolDB::FindMapFile(&existing, &writable))
    {
      load_map(existing, false);
      return;
    }
    // A game without a map file is scanned for common functions instead.
    auto& symbols = system.GetPowerPC().GetSymbolDB();
    symbols.Clear();
    {
      const Core::CPUThreadGuard guard(system);
      PPCAnalyst::FindFunctions(guard, Memory::MEM1_BASE_ADDR + 0x1300000,
                                Memory::MEM1_BASE_ADDR + system.GetMemory().GetRamSizeReal(),
                                &symbols);
      SignatureDB db(SignatureDB::HandlerType::DSY);
      if (db.Load(File::GetSysDirectory() + TOTALDB))
        db.Apply(guard, &symbols);
    }
    HLE::PatchFunctions(system);
    Host_PPCSymbolsChanged();
    OSD::AddMessage(
        fmt::format("{} was not found, so common functions were found instead", writable));
    m_changed = true;
    return;
  }
  case 1:
  case 2:
    Pick(false, {L".map"}, L"dolphin-debugger-map",
         [load_map, bad = choice == 2](const std::string& path) { load_map(path, bad); });
    return;
  case 3:
    Pick(false, {L".dsy", L".csv", L".mega"}, L"dolphin-debugger-signature",
         [this](const std::string& path) {
           auto& signature_system = Core::System::GetInstance();
           SignatureDB db(path);
           db.Load(path);
           db.Apply(Core::CPUThreadGuard{signature_system},
                    &signature_system.GetPowerPC().GetSymbolDB());
           HLE::PatchFunctions(signature_system);
           Host_PPCSymbolsChanged();
           OSD::AddMessage(fmt::format("Applied {}", path));
           m_changed = true;
         });
    return;
  default:
    return;
  }
}

void DebuggerView::SaveSymbols(size_t choice)
{
  auto& system = Core::System::GetInstance();
  const auto& symbols = system.GetPowerPC().GetSymbolDB();
  std::string existing;
  std::string writable;
  PPCSymbolDB::FindMapFile(&existing, &writable);
  const auto save_map = [](const std::string& path) {
    File::CreateFullPath(path);
    OSD::AddMessage(Core::System::GetInstance().GetPowerPC().GetSymbolDB().SaveSymbolMap(path) ?
                        fmt::format("Saved the symbols to {}", path) :
                        fmt::format("The symbols could not be saved to {}", path));
  };

  switch (choice)
  {
  case 0:
    save_map(writable);
    return;
  case 1:
    Pick(true, {}, L"dolphin-debugger-map-folder", [save_map](const std::string& folder) {
      save_map(fmt::format("{}/{}.map", folder, GameID()));
    });
    return;
  case 2:
  {
    const std::string path = writable.substr(0, writable.find_last_of('.')) + "_code.map";
    File::CreateFullPath(path);
    OSD::AddMessage(symbols.SaveCodeMap(Core::CPUThreadGuard{system}, path) ?
                        fmt::format("Saved the code to {}", path) :
                        fmt::format("The code could not be saved to {}", path));
    return;
  }
  case 3:
    Pick(true, {}, L"dolphin-debugger-signature-folder", [](const std::string& folder) {
      const std::string path = fmt::format("{}/{}.dsy", folder, GameID());
      SignatureDB db(path);
      db.Populate(&Core::System::GetInstance().GetPowerPC().GetSymbolDB());
      OSD::AddMessage(db.Save(path) ? fmt::format("Saved the signatures to {}", path) :
                                      fmt::format("The signatures could not be saved to {}", path));
    });
    return;
  case 4:
    Pick(false, {L".dsy", L".csv", L".mega"}, L"dolphin-debugger-signature",
         [](const std::string& path) {
           SignatureDB db(path);
           db.Populate(&Core::System::GetInstance().GetPowerPC().GetSymbolDB());
           db.Load(path);
           OSD::AddMessage(db.Save(path) ?
                               fmt::format("Added the signatures to {}", path) :
                               fmt::format("The signatures could not be added to {}", path));
         });
    return;
  default:
    return;
  }
}

void DebuggerView::BranchWatchFile(size_t choice)
{
  const std::string default_path =
      fmt::format("{}{}.txt", File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX), GameID());
  const auto save = [](const std::string& path) {
    auto& system = Core::System::GetInstance();
    Core::BranchWatch& branch_watch = system.GetPowerPC().GetBranchWatch();
    if (!branch_watch.CanSave())
    {
      OSD::AddMessage("There is nothing to save");
      return;
    }
    File::CreateFullPath(path);
    File::IOFile file(path, "w");
    if (!file.IsOpen())
    {
      OSD::AddMessage(fmt::format("{} could not be written", path));
      return;
    }
    branch_watch.Save(Core::CPUThreadGuard{system}, file.GetHandle());
    OSD::AddMessage(fmt::format("Saved branch watch to {}", path));
  };
  const auto load = [this](const std::string& path) {
    auto& system = Core::System::GetInstance();
    File::IOFile file(path, "r");
    if (!file.IsOpen())
    {
      OSD::AddMessage(fmt::format("{} could not be read", path));
      return;
    }
    system.GetPowerPC().GetBranchWatch().Load(Core::CPUThreadGuard{system}, file.GetHandle());
    OSD::AddMessage(fmt::format("Loaded branch watch from {}", path));
    m_list_selected = 0;
    m_changed = true;
  };

  switch (choice)
  {
  case 0:
    save(default_path);
    return;
  case 1:
    Pick(true, {}, L"dolphin-debugger-branch-watch-folder",
         [save](const std::string& folder) { save(fmt::format("{}/{}.txt", folder, GameID())); });
    return;
  case 2:
    load(default_path);
    return;
  case 3:
    Pick(false, {L".txt"}, L"dolphin-debugger-branch-watch", load);
    return;
  default:
    return;
  }
}

void DebuggerView::AssembleSource()
{
  m_assembler_result.clear();
  const auto result = Common::GekkoAssembler::Assemble(m_assembler_source, m_assembler_base);
  if (Common::GekkoAssembler::IsFailure(result))
  {
    const Common::GekkoAssembler::AssemblerError& error =
        Common::GekkoAssembler::GetFailure(result);
    m_assembler_result.push_back(fmt::format("Error on line {}, column {}: {}", error.line + 1,
                                             error.col + 1, error.message));
    m_assembler_result.emplace_back(error.error_line);
    m_changed = true;
    return;
  }
  for (const CodeBlock& block : Common::GekkoAssembler::GetT(result))
  {
    const std::vector<u32> words = BlockWords(block);
    for (size_t i = 0; i < words.size(); ++i)
    {
      m_assembler_result.push_back(
          fmt::format("{:08X}  {:08X}", block.block_address + i * 4, words[i]));
    }
  }
  if (m_assembler_result.empty())
    m_assembler_result.push_back("There is nothing to assemble");
  m_changed = true;
}

void DebuggerView::InjectSource()
{
  const auto result = Common::GekkoAssembler::Assemble(m_assembler_source, m_assembler_base);
  if (Common::GekkoAssembler::IsFailure(result))
  {
    AssembleSource();
    return;
  }
  auto& system = Core::System::GetInstance();
  const Core::CPUThreadGuard guard(system);
  size_t bytes = 0;
  for (const CodeBlock& block : Common::GekkoAssembler::GetT(result))
  {
    if (block.instructions.empty() || !PowerPC::MMU::HostIsRAMAddress(guard, block.block_address))
      continue;
    system.GetPowerPC().GetDebugInterface().SetPatch(guard, block.block_address,
                                                     block.instructions);
    bytes += block.instructions.size();
  }
  OSD::AddMessage(fmt::format("Injected {} bytes", bytes));
  m_changed = true;
}

void DebuggerView::AddSourceAsCode(size_t choice)
{
  const auto result = Common::GekkoAssembler::Assemble(m_assembler_source, m_assembler_base);
  if (Common::GekkoAssembler::IsFailure(result))
  {
    AssembleSource();
    return;
  }
  const std::vector<CodeBlock>& blocks = Common::GekkoAssembler::GetT(result);
  const std::string name = fmt::format("Assembled at {:08X}", m_assembler_base);

  GameCheats cheats;
  cheats.Load(GameID(), SConfig::GetInstance().GetRevision());
  std::string error;
  if (choice == 2)
  {
    auto entries = ParseActionReplayCode(WriteCode(blocks), &error);
    if (entries)
    {
      ActionReplay::ARCode code;
      code.name = name;
      code.ops = std::move(*entries);
      code.enabled = true;
      code.user_defined = true;
      cheats.action_replay.push_back(std::move(code));
    }
  }
  else
  {
    auto lines = ParseGeckoCode(choice == 0 ? WriteCode(blocks) : InsertCode(blocks), &error);
    if (lines)
    {
      Gecko::GeckoCode code;
      code.name = name;
      code.codes = std::move(*lines);
      code.enabled = true;
      code.user_defined = true;
      cheats.gecko.push_back(std::move(code));
    }
  }
  if (!error.empty())
  {
    OSD::AddMessage(fmt::format("The code could not be made: {}", error));
    return;
  }
  cheats.Save();
  cheats.ApplyToRunningGame();
  OSD::AddMessage(Config::AreCheatsEnabled() ?
                      fmt::format("Added \"{}\" to this game's cheats", name) :
                      fmt::format("Added \"{}\". Enable cheats to use it", name));
}

void DebuggerView::AssemblerFile(size_t choice)
{
  if (choice == 0)
  {
    Pick(false, {L".s", L".asm", L".txt"}, L"dolphin-debugger-assembly",
         [this](const std::string& path) {
           std::string source;
           if (!File::ReadFileToString(path, source))
           {
             OSD::AddMessage(fmt::format("{} could not be read", path));
             return;
           }
           m_assembler_source = std::move(source);
           m_assembler_result.clear();
           m_changed = true;
         });
    return;
  }
  Pick(true, {}, L"dolphin-debugger-assembly-folder", [this](const std::string& folder) {
    const std::string path = fmt::format("{}/{}.s", folder, GameID());
    OSD::AddMessage(File::WriteStringToFile(path, m_assembler_source) ?
                        fmt::format("Saved the assembly to {}", path) :
                        fmt::format("{} could not be written", path));
  });
}

void DebuggerView::Pick(bool folder, std::vector<std::wstring> file_types, std::wstring token,
                        std::function<void(const std::string&)> picked)
{
  m_pick = PickRequest{folder, std::move(file_types), std::move(token), std::move(picked)};
}

void DebuggerView::OpenHexEntry(EntryPurpose purpose, std::string title, u32 value, u32 target)
{
  Entry entry;
  entry.purpose = purpose;
  entry.title = std::move(title);
  entry.target = target;
  for (size_t i = 0; i < HEX_DIGITS_PER_WORD; ++i)
  {
    entry.cells.push_back(HEX_DIGITS);
    entry.values.push_back((value >> ((HEX_DIGITS_PER_WORD - 1 - i) * 4)) & 0xF);
  }
  m_entry = std::move(entry);
  m_changed = true;
}

void DebuggerView::OpenConditionEntry(u32 address)
{
  OpenHexEntry(EntryPurpose::Condition, fmt::format("Break at {:08X} when", address), 0, address);
  m_entry->cells.insert(m_entry->cells.begin(), {CONDITION_REGISTERS, COMPARISONS});
  m_entry->values.insert(m_entry->values.begin(), {1, 0});
}

void DebuggerView::ConfirmEntry()
{
  const Entry entry = std::move(*m_entry);
  m_entry.reset();
  m_changed = true;

  const size_t first_digit = entry.purpose == EntryPurpose::Condition ? 2 : 0;
  u32 value = 0;
  for (size_t i = first_digit; i < entry.values.size(); ++i)
    value = (value << 4) | static_cast<u32>(entry.values[i]);

  auto& system = Core::System::GetInstance();
  switch (entry.purpose)
  {
  case EntryPurpose::GoToAddress:
    if (m_tab == Tab::Memory)
      ShowMemory(value);
    else
      ShowCode(value);
    break;
  case EntryPurpose::EditMemory:
    if (IsStepping())
    {
      const Core::CPUThreadGuard guard(system);
      if (!PowerPC::MMU::HostTryWrite<u32>(guard, value, entry.target))
        OSD::AddMessage(fmt::format("{:08X} could not be written", entry.target));
      system.GetJitInterface().InvalidateICache(entry.target, MEMORY_WORD_BYTES, true);
    }
    break;
  case EntryPurpose::EditRegister:
    WriteRegister(entry.target, value);
    break;
  case EntryPurpose::FindValue:
    FindValue(value);
    break;
  case EntryPurpose::AddWatch:
    system.GetPowerPC().GetDebugInterface().SetWatch(value);
    OSD::AddMessage(fmt::format("Watching {:08X}", value));
    break;
  case EntryPurpose::AddMemoryBreakpoint:
    AddMemoryBreakpointAt(value);
    OSD::AddMessage(fmt::format("Added a memory breakpoint at {:08X}", value));
    break;
  case EntryPurpose::Condition:
    if (entry.values[0] == 0)
    {
      SetCondition(entry.target, std::nullopt);
    }
    else
    {
      SetCondition(entry.target, fmt::format("{} {} {}", CONDITION_REGISTERS[entry.values[0]],
                                             COMPARISONS[entry.values[1]], value));
    }
    break;
  case EntryPurpose::AssemblerBase:
    m_assembler_base = value;
    m_assembler_result.clear();
    break;
  }
}

void DebuggerView::OpenChoice(ChoicePurpose purpose, std::string title,
                              std::vector<std::string> items, std::vector<u32> values)
{
  m_choice = Choice{purpose, std::move(title), std::move(items), std::move(values), 0};
  m_changed = true;
}

void DebuggerView::ConfirmChoice()
{
  const Choice choice = std::move(*m_choice);
  m_choice.reset();
  m_changed = true;
  switch (choice.purpose)
  {
  case ChoicePurpose::GenerateSymbols:
    GenerateSymbols(choice.selected);
    break;
  case ChoicePurpose::LoadSymbols:
    LoadSymbols(choice.selected);
    break;
  case ChoicePurpose::SaveSymbols:
    SaveSymbols(choice.selected);
    break;
  case ChoicePurpose::ClearSymbols:
    if (choice.selected == 0)
    {
      Core::System::GetInstance().GetPowerPC().GetSymbolDB().Clear();
      Host_PPCSymbolsChanged();
      m_list_selected = 0;
    }
    break;
  case ChoicePurpose::RsoModule:
    if (choice.selected < choice.values.size())
      ApplyRSOModule(choice.values[choice.selected]);
    break;
  case ChoicePurpose::BranchWatchFile:
    BranchWatchFile(choice.selected);
    break;
  case ChoicePurpose::AddAsCode:
    AddSourceAsCode(choice.selected);
    break;
  case ChoicePurpose::AssemblerFile:
    AssemblerFile(choice.selected);
    break;
  }
}

void DebuggerView::OpenText(TextPurpose purpose, std::string title, std::string text, u32 target)
{
  m_text = TextEdit{purpose, std::move(title), target};
  GameTextInput::Begin(std::move(text), purpose == TextPurpose::EditSource,
                       Common::UWP::IsXboxConsole());
  m_changed = true;
}

void DebuggerView::HandleTextInput(const Frontend::FrameInput& input)
{
  m_changed = true;
  switch (GameTextInput::TakeResult())
  {
  case GameTextInput::Result::Submitted:
  {
    std::string text = GameTextInput::GetText();
    GameTextInput::End();
    ConfirmText(std::move(text));
    return;
  }
  case GameTextInput::Result::Cancelled:
    GameTextInput::End();
    m_text.reset();
    return;
  case GameTextInput::Result::None:
    break;
  }

  // The on-screen keyboard has the controller while it is shown.
  if (GameTextInput::IsKeyboardVisible())
    return;
  if (input.accept)
    GameTextInput::ShowKeyboard();
  else if (input.menu)
    GameTextInput::Submit();
  else if (input.back && GameTextInput::IsMultiline())
    GameTextInput::Submit();
  else if (input.back)
    GameTextInput::Cancel();
  else if (input.left || input.right)
    GameTextInput::MoveCaret(input.left ? -1 : 1);
}

void DebuggerView::ConfirmText(std::string text)
{
  const TextEdit edit = std::move(*m_text);
  m_text.reset();
  m_changed = true;
  switch (edit.purpose)
  {
  case TextPurpose::AssembleInstruction:
    if (!StripWhitespace(text).empty())
      AssembleInstruction(edit.target, text);
    break;
  case TextPurpose::RenameSymbol:
    RenameSymbol(edit.target, text);
    break;
  case TextPurpose::FindSymbol:
    m_symbol_filter = std::string(StripWhitespace(text));
    m_list_selected = 0;
    break;
  case TextPurpose::EditSource:
    m_assembler_source = std::move(text);
    m_assembler_result.clear();
    break;
  }
}

DebuggerView::Effect DebuggerView::Continue()
{
  Close();
  Core::SetState(Core::System::GetInstance(), Core::State::Running);
  return Effect::Close;
}

void DebuggerView::RunCommand(Command command)
{
  auto& system = Core::System::GetInstance();
  auto& power_pc = system.GetPowerPC();
  switch (command)
  {
  case Command::Step:
    Step();
    break;
  case Command::StepOver:
    StepOver();
    break;
  case Command::StepOut:
    StepOut();
    break;
  case Command::Skip:
    Skip();
    break;
  case Command::GoToPC:
    if (m_tab == Tab::Threads && m_list_selected < m_list_addresses.size())
      ShowCode(m_list_addresses[m_list_selected]);
    else
      ShowPC();
    break;
  case Command::GoToAddress:
    OpenHexEntry(EntryPurpose::GoToAddress, "Go to address",
                 m_tab == Tab::Memory ? SelectedMemoryAddress() : SelectedCodeAddress());
    break;
  case Command::FindValue:
    OpenHexEntry(EntryPurpose::FindValue, "Find value", m_last_find);
    break;
  case Command::AddWatch:
    OpenHexEntry(EntryPurpose::AddWatch, "Watch address", SelectedMemoryAddress());
    break;
  case Command::AddMemoryBreakpoint:
    OpenHexEntry(EntryPurpose::AddMemoryBreakpoint, "Memory breakpoint at",
                 SelectedMemoryAddress());
    break;
  case Command::SetCondition:
    if (m_list_selected < m_list_code_breakpoints.size() &&
        m_list_code_breakpoints[m_list_selected])
    {
      OpenConditionEntry(m_list_addresses[m_list_selected]);
    }
    else
    {
      OSD::AddMessage("Choose a code breakpoint to set its condition");
    }
    break;
  case Command::AssembleInstruction:
    if (IsStepping())
    {
      const u32 address = SelectedCodeAddress();
      std::string text;
      {
        const Core::CPUThreadGuard guard(system);
        text = Disassemble(guard, address);
      }
      OpenText(TextPurpose::AssembleInstruction, fmt::format("Assemble at {:08X}", address),
               std::move(text), address);
    }
    break;
  case Command::ReplaceWithNop:
    if (IsStepping())
      SetInstruction(SelectedCodeAddress(), NOP);
    break;
  case Command::RestoreInstruction:
    if (IsStepping())
      RestoreInstruction(SelectedCodeAddress());
    break;
  case Command::RenameSymbol:
  {
    const u32 address =
        m_tab == Tab::Symbols ?
            (m_list_selected < m_list_addresses.size() ? m_list_addresses[m_list_selected] : 0) :
            SelectedCodeAddress();
    const Common::Symbol* const symbol = power_pc.GetSymbolDB().GetSymbolFromAddr(address);
    if (symbol)
      OpenText(TextPurpose::RenameSymbol, "Rename the symbol", symbol->name, symbol->address);
    else
      OSD::AddMessage("There is no symbol to rename here");
    break;
  }
  case Command::FollowBranch:
    if (IsStepping())
      FollowBranch();
    break;
  case Command::FindSymbol:
    OpenText(TextPurpose::FindSymbol, "Find symbols whose names contain", m_symbol_filter);
    break;
  case Command::GenerateSymbols:
    OpenChoice(ChoicePurpose::GenerateSymbols, "Generate symbols",
               {"Scan memory for functions", "Use the signature database", "Find RSO modules"});
    break;
  case Command::LoadSymbols:
    OpenChoice(ChoicePurpose::LoadSymbols, "Load symbols",
               {"Load the game's map", "Load another map", "Load a map with bad symbols",
                "Apply a signature file"});
    break;
  case Command::SaveSymbols:
    OpenChoice(ChoicePurpose::SaveSymbols, "Save symbols",
               {"Save the game's map", "Save the map to a folder", "Save the code map",
                "Create a signature file", "Add to a signature file"});
    break;
  case Command::ClearSymbols:
    OpenChoice(ChoicePurpose::ClearSymbols, "Clear the list of symbol names?",
               {"Clear the symbols", "Keep them"});
    break;
  case Command::StartBranchWatch:
  {
    Core::BranchWatch& branch_watch = power_pc.GetBranchWatch();
    branch_watch.SetRecordingActive(Core::CPUThreadGuard{system},
                                    !branch_watch.GetRecordingActive());
    break;
  }
  case Command::CodePathTaken:
    power_pc.GetBranchWatch().IsolateHasExecuted(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    break;
  case Command::CodePathNotTaken:
    power_pc.GetBranchWatch().IsolateNotExecuted(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    break;
  case Command::BranchOverwritten:
    power_pc.GetBranchWatch().IsolateWasOverwritten(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    break;
  case Command::BranchNotOverwritten:
    power_pc.GetBranchWatch().IsolateNotOverwritten(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    break;
  case Command::WipeRecentHits:
    power_pc.GetBranchWatch().UpdateHitsSnapshot();
    break;
  case Command::ClearBranchWatch:
    power_pc.GetBranchWatch().Clear(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    break;
  case Command::BranchWatchFile:
    OpenChoice(ChoicePurpose::BranchWatchFile, "Branch watch",
               {"Save", "Save to a folder", "Load", "Load a file"});
    break;
  case Command::ClearJitCache:
    system.GetJitInterface().ClearCache(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    OSD::AddMessage("The JIT cache was cleared");
    break;
  case Command::JitProfiling:
    Config::SetBaseOrCurrent(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING,
                             !Config::Get(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING));
    // Blocks count their runs once they are compiled again.
    system.GetJitInterface().ClearCache(Core::CPUThreadGuard{system});
    m_list_selected = 0;
    break;
  case Command::WipeProfileData:
    system.GetJitInterface().WipeBlockProfilingData(Core::CPUThreadGuard{system});
    break;
  case Command::EditSource:
    OpenText(TextPurpose::EditSource, "Assembly", m_assembler_source);
    break;
  case Command::BaseAddress:
    OpenHexEntry(EntryPurpose::AssemblerBase, "Base address", m_assembler_base);
    break;
  case Command::Assemble:
    AssembleSource();
    break;
  case Command::Inject:
    if (IsStepping())
      InjectSource();
    break;
  case Command::AddAsCode:
    OpenChoice(ChoicePurpose::AddAsCode, "Add the assembled code as",
               {"A Gecko code that writes it",
                "A Gecko code that runs it in place of the "
                "instruction at the base address",
                "An Action Replay code that writes it"});
    break;
  case Command::AssemblerFile:
    OpenChoice(ChoicePurpose::AssemblerFile, "Assembly file", {"Open a file", "Save to a folder"});
    break;
  case Command::Continue:
    break;
  }
  m_changed = true;
}

void DebuggerView::Activate()
{
  auto& system = Core::System::GetInstance();
  switch (m_tab)
  {
  case Tab::Code:
    system.GetPowerPC().GetBreakPoints().ToggleBreakPoint(SelectedCodeAddress());
    break;
  case Tab::Registers:
    if (IsStepping())
    {
      OpenHexEntry(EntryPurpose::EditRegister,
                   fmt::format("Edit {}", RegisterName(m_register_selected)),
                   ReadRegister(system.GetPPCState(), m_register_selected),
                   static_cast<u32>(m_register_selected));
    }
    break;
  case Tab::Memory:
    if (IsStepping())
    {
      const u32 address = SelectedMemoryAddress();
      const auto value = [&] {
        const Core::CPUThreadGuard guard(system);
        return PowerPC::MMU::HostTryRead<u32>(guard, address);
      }();
      if (value)
        OpenHexEntry(EntryPurpose::EditMemory, fmt::format("Edit {:08X}", address), value->value,
                     address);
    }
    break;
  case Tab::Watches:
    if (m_list_selected < m_list_addresses.size())
      ShowMemory(m_list_addresses[m_list_selected]);
    break;
  case Tab::Network:
    break;
  case Tab::Assembler:
    OpenText(TextPurpose::EditSource, "Assembly", m_assembler_source);
    break;
  default:
    if (m_list_selected < m_list_addresses.size())
      ShowCode(m_list_addresses[m_list_selected]);
    break;
  }
  m_changed = true;
}

void DebuggerView::Secondary()
{
  auto& system = Core::System::GetInstance();
  auto& power_pc = system.GetPowerPC();
  switch (m_tab)
  {
  case Tab::Code:
    if (IsStepping())
      power_pc.GetDebugInterface().RunTo(SelectedCodeAddress());
    break;
  case Tab::Memory:
    if (IsStepping())
      ToggleMemoryBreakpoint(SelectedMemoryAddress());
    break;
  case Tab::Breakpoints:
    if (m_list_selected < m_list_code_breakpoints.size())
    {
      const u32 address = m_list_addresses[m_list_selected];
      if (m_list_code_breakpoints[m_list_selected])
        power_pc.GetBreakPoints().Remove(address);
      else
        power_pc.GetMemChecks().Remove(address);
    }
    break;
  case Tab::Watches:
    if (m_list_selected < power_pc.GetDebugInterface().GetWatches().size())
      power_pc.GetDebugInterface().RemoveWatch(m_list_selected);
    break;
  case Tab::Symbols:
    if (m_list_selected < m_list_addresses.size())
      power_pc.GetBreakPoints().ToggleBreakPoint(m_list_addresses[m_list_selected]);
    break;
  case Tab::Threads:
    if (m_list_selected < m_list_second_addresses.size())
      ShowMemory(m_list_second_addresses[m_list_selected]);
    break;
  case Tab::BranchWatch:
    if (m_list_selected < m_list_second_addresses.size())
      ShowCode(m_list_second_addresses[m_list_selected]);
    break;
  default:
    break;
  }
  m_changed = true;
}

DebuggerView::Effect DebuggerView::HandleInput(const Frontend::FrameInput& input)
{
  if (m_rso_search.valid() &&
      m_rso_search.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    const std::vector<std::pair<u32, std::string>> modules = m_rso_search.get();
    if (modules.empty())
    {
      OSD::AddMessage("No RSO modules were found");
    }
    else
    {
      std::vector<std::string> items;
      std::vector<u32> addresses;
      for (const auto& [address, name] : modules)
      {
        items.push_back(fmt::format("{:08X}  {}", address, name));
        addresses.push_back(address);
      }
      OpenChoice(ChoicePurpose::RsoModule, "Choose an RSO module", std::move(items),
                 std::move(addresses));
    }
  }

  if (m_text)
  {
    HandleTextInput(input);
    return Effect::None;
  }

  if (m_choice)
  {
    Choice& choice = *m_choice;
    const size_t count = choice.items.size();
    if (count != 0 && (input.up || input.down))
      choice.selected = (choice.selected + (input.down ? 1 : count - 1)) % count;
    if (input.accept && count != 0)
      ConfirmChoice();
    else if (input.back)
      m_choice.reset();
    m_changed |= input.up || input.down || input.accept || input.back;
    return Effect::None;
  }

  if (m_entry)
  {
    Entry& entry = *m_entry;
    const size_t count = entry.cells.size();
    if (input.left)
      entry.cursor = (entry.cursor + count - 1) % count;
    if (input.right)
      entry.cursor = (entry.cursor + 1) % count;
    if (input.up || input.down)
    {
      const size_t options = entry.cells[entry.cursor].size();
      size_t& value = entry.values[entry.cursor];
      value = (value + (input.up ? 1 : options - 1)) % options;
    }
    if (input.accept)
      ConfirmEntry();
    else if (input.back)
      m_entry.reset();
    m_changed |= input.left || input.right || input.up || input.down || input.accept || input.back;
    return Effect::None;
  }

  if (input.back)
    return Continue();
  if (input.menu)
    return Continue();
  if (input.leftShoulder || input.rightShoulder)
  {
    const int count = static_cast<int>(TAB_NAMES.size());
    m_tab =
        static_cast<Tab>((static_cast<int>(m_tab) + (input.rightShoulder ? 1 : count - 1)) % count);
    m_list_selected = 0;
    m_list_addresses.clear();
    m_command = 0;
    m_changed = true;
  }
  if (input.alternate)
  {
    m_commands_focused = !m_commands_focused;
    m_changed = true;
  }
  if (input.view)
    Step();

  if (m_commands_focused)
  {
    const std::vector<Command> commands = GetCommands();
    const size_t count = commands.size();
    if (input.left)
      m_command = (m_command + count - 1) % count;
    if (input.right)
      m_command = (m_command + 1) % count;
    if (input.down)
      m_commands_focused = false;
    m_changed |= input.left || input.right || input.down;
    if (input.accept)
    {
      const Command command = commands[m_command];
      if (command == Command::Continue)
        return Continue();
      RunCommand(command);
    }
    return Effect::None;
  }

  if (input.up)
    MoveSelection(-1);
  if (input.down)
    MoveSelection(1);
  if (input.left || input.right)
    MoveColumn(input.left ? -1 : 1);
  if (input.accept)
    Activate();
  if (input.context)
    Secondary();
  return Effect::None;
}

DebuggerSnapshot DebuggerView::BuildSnapshot()
{
  DebuggerSnapshot snapshot;
  snapshot.tab = static_cast<int>(m_tab);
  snapshot.stepping = IsStepping();
  snapshot.commands_focused = m_commands_focused;
  snapshot.command = m_command;
  for (const Command command : GetCommands())
    snapshot.commands.push_back(GetCommandLabel(command));

  if (m_text)
  {
    snapshot.text_title = m_text->title;
    snapshot.text = GameTextInput::GetText();
    snapshot.text_caret = GameTextInput::GetCaret();
    snapshot.text_multiline = GameTextInput::IsMultiline();
    snapshot.hint = GameTextInput::IsKeyboardVisible() ?
                        "Type with the on-screen keyboard" :
                    snapshot.text_multiline ?
                        "A Keyboard   Left/Right Move   Menu or B Done   Esc Done" :
                        "A Keyboard   Left/Right Move   Menu Done   B Cancel   Enter Done";
  }
  else if (m_choice)
  {
    snapshot.choice_title = m_choice->title;
    snapshot.choices = m_choice->items;
    snapshot.choice_selected = m_choice->selected;
    snapshot.hint = "Up/Down Choose   A Confirm   B Cancel";
  }
  else if (m_entry)
  {
    snapshot.entry_title = m_entry->title;
    for (size_t i = 0; i < m_entry->cells.size(); ++i)
      snapshot.entry_cells.push_back(m_entry->cells[i][m_entry->values[i]]);
    snapshot.entry_cursor = m_entry->cursor;
    snapshot.hint = "Left/Right Choose   Up/Down Change   A Confirm   B Cancel";
  }
  else if (m_commands_focused)
  {
    snapshot.hint = "Left/Right Choose   A Run   Y Back to the view   B Resume the game";
  }
  else
  {
    static constexpr std::array<const char*, 12> hints{
        "A Breakpoint   X Run to here   Left/Right Page   View Step   Y Commands",
        "A Edit   View Step   Y Commands",
        "A Edit   X Memory breakpoint   View Step   Y Commands",
        "A Show   X Remove   View Step   Y Commands",
        "A Show   X Remove   View Step   Y Commands",
        "A Show   View Step   Y Commands",
        "A Show   X Breakpoint   Left/Right Page   Y Commands",
        "A Show the PC   X Show the stack   Y Commands",
        "A Show the origin   X Show the destination   Left/Right Page   Y Commands",
        "A Show the code   Left/Right Page   Y Commands",
        "Y Commands",
        "A Edit   Y Commands",
    };
    snapshot.hint =
        fmt::format("{}   LB/RB Tabs   B Resume the game", hints[static_cast<size_t>(m_tab)]);
  }

  auto& system = Core::System::GetInstance();
  if (m_tab == Tab::Assembler)
  {
    snapshot.header = fmt::format("Base address {:08X}", m_assembler_base);
    std::vector<std::string> source = SplitString(m_assembler_source, '\n');
    if (m_assembler_source.empty())
      snapshot.text_lines.push_back("Choose Edit to write assembly, such as li r3, 1.");
    for (size_t i = 0; i < std::min(source.size(), ASSEMBLER_SOURCE_LINES); ++i)
      snapshot.text_lines.push_back(std::move(source[i]));
    if (source.size() > ASSEMBLER_SOURCE_LINES)
    {
      snapshot.text_lines.push_back(
          fmt::format("and {} more lines", source.size() - ASSEMBLER_SOURCE_LINES));
    }
    if (!m_assembler_result.empty())
    {
      snapshot.text_lines.emplace_back();
      snapshot.text_lines.insert(snapshot.text_lines.end(), m_assembler_result.begin(),
                                 m_assembler_result.end());
    }
  }

  if (!snapshot.stepping)
  {
    snapshot.status = "Running";
    if (m_tab == Tab::BranchWatch && system.GetPowerPC().GetBranchWatch().GetRecordingActive())
      snapshot.header = "Branch watch is recording. Pause the game to narrow the candidates.";
    return snapshot;
  }

  auto& power_pc = system.GetPowerPC();
  const auto& ppc_state = system.GetPPCState();
  const PPCSymbolDB& symbols = power_pc.GetSymbolDB();
  const Core::CPUThreadGuard guard(system);
  const Common::Symbol* const pc_symbol = symbols.GetSymbolFromAddr(ppc_state.pc);
  snapshot.status = pc_symbol ?
                        fmt::format("Paused at {:08X} in {}", ppc_state.pc, pc_symbol->name) :
                        fmt::format("Paused at {:08X}", ppc_state.pc);

  const auto symbol_name = [&symbols](u32 address) -> std::string {
    const Common::Symbol* const symbol = symbols.GetSymbolFromAddr(address);
    return symbol ? symbol->name : std::string();
  };

  std::vector<DebuggerSnapshot::ListEntry> list;
  std::string empty_text;
  switch (m_tab)
  {
  case Tab::Code:
  {
    const BreakPoints& breakpoints = power_pc.GetBreakPoints();
    const auto& debug_interface = power_pc.GetDebugInterface();
    for (size_t i = 0; i < CODE_LINES; ++i)
    {
      DebuggerSnapshot::CodeLine line;
      line.address = m_code_address + static_cast<u32>(i) * 4;
      const auto instruction = PowerPC::MMU::HostTryReadInstruction(guard, line.address);
      if (instruction)
      {
        const std::string text =
            Common::GekkoDisassembler::Disassemble(instruction->value, line.address);
        const size_t split = text.find('\t');
        line.mnemonic = text.substr(0, split);
        if (split != std::string::npos)
          line.operands = text.substr(split + 1);
      }
      else
      {
        line.mnemonic = "--";
      }
      if (const Common::Symbol* symbol = symbols.GetSymbolFromAddr(line.address);
          symbol && symbol->address == line.address)
      {
        line.symbol = symbol->name;
      }
      line.breakpoint = breakpoints.IsAddressBreakPoint(line.address);
      line.pc = line.address == ppc_state.pc;
      line.patched = debug_interface.HasEnabledPatch(line.address);
      snapshot.code.push_back(std::move(line));
    }
    snapshot.code_selected = m_code_selected;
    break;
  }
  case Tab::Registers:
  {
    for (size_t i = 0; i < INTEGER_REGISTERS; ++i)
      snapshot.registers.push_back(
          {RegisterName(i), fmt::format("{:08X}", ReadRegister(ppc_state, i))});
    for (int i = 0; i < 32; ++i)
    {
      snapshot.registers.push_back(
          {fmt::format("f{}", i), fmt::format("{:.6g} / {:.6g}", ppc_state.ps[i].PS0AsDouble(),
                                              ppc_state.ps[i].PS1AsDouble())});
    }
    snapshot.register_selected = m_register_selected;
    break;
  }
  case Tab::Memory:
  {
    snapshot.memory_address = m_memory_address;
    snapshot.memory_selected = m_memory_selected;
    snapshot.memory_column = m_memory_column;
    const auto& memchecks = power_pc.GetMemChecks();
    for (u32 offset = 0; offset < MEMORY_ROWS * MEMORY_ROW_BYTES; ++offset)
    {
      const u32 address = m_memory_address + offset;
      const auto value = PowerPC::MMU::HostTryRead<u8>(guard, address);
      snapshot.memory.push_back(value ? std::optional<u8>(value->value) : std::nullopt);
      snapshot.memory_breakpoints.push_back(memchecks.OverlapsMemcheck(address, 1));
    }
    break;
  }
  case Tab::Breakpoints:
  {
    m_list_addresses.clear();
    m_list_code_breakpoints.clear();
    for (const TBreakPoint& breakpoint : power_pc.GetBreakPoints().GetBreakPoints())
    {
      const std::string name = symbol_name(breakpoint.address);
      std::string value = breakpoint.is_enabled ? "Code" : "Code (off)";
      if (breakpoint.condition)
        value += fmt::format(" if {}", breakpoint.condition->GetText());
      list.push_back({name.empty() ? fmt::format("{:08X}", breakpoint.address) :
                                     fmt::format("{:08X}  {}", breakpoint.address, name),
                      std::move(value)});
      m_list_addresses.push_back(breakpoint.address);
      m_list_code_breakpoints.push_back(true);
    }
    for (const TMemCheck& check : power_pc.GetMemChecks().GetMemChecks())
    {
      const char* const kind = check.is_break_on_read && check.is_break_on_write ? "Read/write" :
                               check.is_break_on_read                            ? "Read" :
                                                                                   "Write";
      list.push_back({check.is_ranged ?
                          fmt::format("{:08X}-{:08X}", check.start_address, check.end_address) :
                          fmt::format("{:08X}", check.start_address),
                      fmt::format("Memory {}", kind)});
      m_list_addresses.push_back(check.start_address);
      m_list_code_breakpoints.push_back(false);
    }
    empty_text = "There are no breakpoints. Press A on a code line to add one.";
    break;
  }
  case Tab::Watches:
  {
    m_list_addresses.clear();
    for (const Common::Debug::Watch& watch : power_pc.GetDebugInterface().GetWatches())
    {
      const auto value = PowerPC::MMU::HostTryRead<u32>(guard, watch.address);
      list.push_back(
          {watch.name.empty() ? fmt::format("{:08X}", watch.address) :
                                fmt::format("{:08X}  {}", watch.address, watch.name),
           value ? fmt::format("{:08X}  {}", value->value, value->value) : std::string("--")});
      m_list_addresses.push_back(watch.address);
    }
    empty_text = "There are no watches. Choose Watch in the memory view to add one.";
    break;
  }
  case Tab::CallStack:
  {
    m_list_addresses.clear();
    std::vector<Dolphin_Debugger::CallstackEntry> entries;
    if (Dolphin_Debugger::GetCallstack(guard, entries))
    {
      for (const auto& entry : entries)
      {
        list.push_back({entry.Name, fmt::format("{:08X}", entry.vAddress)});
        m_list_addresses.push_back(entry.vAddress);
      }
    }
    empty_text = "The call stack is not available.";
    break;
  }
  case Tab::Symbols:
  {
    m_list_addresses.clear();
    symbols.ForEachSymbol([&](const Common::Symbol& symbol) {
      if (!ContainsIgnoringCase(symbol.name, m_symbol_filter))
        return;
      list.push_back({fmt::format("{:08X}  {}", symbol.address, symbol.name),
                      fmt::format("{} bytes", symbol.size)});
      m_list_addresses.push_back(symbol.address);
    });
    if (!m_symbol_filter.empty())
      snapshot.header = fmt::format("{} symbols contain \"{}\"", list.size(), m_symbol_filter);
    empty_text = m_symbol_filter.empty() ?
                     "There are no symbols. Choose Generate or Load to add them." :
                     "No symbol contains the text.";
    break;
  }
  case Tab::Threads:
  {
    m_list_addresses.clear();
    m_list_second_addresses.clear();
    for (const auto& thread : power_pc.GetDebugInterface().GetThreads(guard))
    {
      if (!thread->IsValid(guard))
        continue;
      const Common::Debug::PartialContext context = thread->GetContext(guard);
      const u32 pc = context.srr0.value_or(0);
      const u32 stack = context.gpr ? context.gpr->at(1) : thread->GetStackStart();
      const std::string name = symbol_name(pc);
      list.push_back(
          {fmt::format("{:08X}  {}  priority {}{}", thread->GetAddress(),
                       ThreadStateName(thread->GetState()), thread->GetEffectivePriority(),
                       thread->IsSuspended() ? "  suspended" : ""),
           name.empty() ? fmt::format("PC {:08X}", pc) : fmt::format("PC {:08X}  {}", pc, name)});
      m_list_addresses.push_back(pc);
      m_list_second_addresses.push_back(stack);
    }
    empty_text = "No threads were found. Games that don't use the Nintendo OS have none.";
    break;
  }
  case Tab::BranchWatch:
  {
    m_list_addresses.clear();
    m_list_second_addresses.clear();
    const Core::BranchWatch& branch_watch = power_pc.GetBranchWatch();
    if (branch_watch.GetRecordingPhase() == Core::BranchWatch::Phase::Blacklist)
    {
      const size_t candidates = branch_watch.GetCollectionSize();
      const size_t excluded = branch_watch.GetBlacklistSize();
      snapshot.header =
          excluded == 0 ?
              fmt::format("{}Candidates: {}",
                          branch_watch.GetRecordingActive() ? "Recording. " : "", candidates) :
              fmt::format("{}Candidates: {}   Excluded: {}   Remaining: {}",
                          branch_watch.GetRecordingActive() ? "Recording. " : "", candidates,
                          excluded, candidates - excluded);
      empty_text = "Start branch watch and play. Choose Code path not taken to exclude the "
                   "branches of what didn't happen, then Code path was taken after it happens.";
      break;
    }
    for (const auto& value : branch_watch.GetSelection())
    {
      const Core::BranchWatchCollectionKey& key = value.collection_ptr->first;
      const Core::BranchWatchCollectionValue& hits = value.collection_ptr->second;
      std::string instruction =
          Common::GekkoDisassembler::Disassemble(key.original_inst.hex, key.origin_addr);
      std::ranges::replace(instruction, '\t', ' ');
      const std::string name = symbol_name(key.origin_addr);
      list.push_back({fmt::format("{:08X} -> {:08X}  {}{}", key.origin_addr, key.destin_addr,
                                  instruction, name.empty() ? "" : "  " + name),
                      fmt::format("{} recent  {} total", hits.total_hits - hits.hits_snapshot,
                                  hits.total_hits)});
      m_list_addresses.push_back(key.origin_addr);
      m_list_second_addresses.push_back(key.destin_addr);
    }
    snapshot.header =
        fmt::format("{}Candidates: {}", branch_watch.GetRecordingActive() ? "Recording. " : "",
                    branch_watch.GetSelection().size());
    empty_text = "Zero candidates remain. Choose Clear to start over.";
    break;
  }
  case Tab::JIT:
  {
    struct Block
    {
      u32 address;
      u32 size;
      size_t runs;
      u64 cycles;
    };
    std::vector<Block> blocks;
    system.GetJitInterface().RunOnBlocks(guard, [&blocks](const JitBlock& block) {
      blocks.push_back({block.effectiveAddress, block.originalSize,
                        block.profile_data ? block.profile_data->run_count : 0,
                        block.profile_data ? block.profile_data->cycles_spent : 0});
    });
    const bool profiling = Config::Get(Config::MAIN_DEBUG_JIT_ENABLE_PROFILING);
    if (profiling)
      std::ranges::sort(blocks, std::greater<>{}, &Block::runs);
    else
      std::ranges::sort(blocks, {}, &Block::address);
    m_list_addresses.clear();
    for (const Block& block : blocks)
    {
      const std::string name = symbol_name(block.address);
      list.push_back({name.empty() ? fmt::format("{:08X}", block.address) :
                                     fmt::format("{:08X}  {}", block.address, name),
                      profiling ? fmt::format("{} runs  {} cycles", block.runs, block.cycles) :
                                  fmt::format("{} instructions", block.size)});
      m_list_addresses.push_back(block.address);
    }
    snapshot.header =
        fmt::format("{} blocks{}", blocks.size(),
                    profiling ? ", by runs" : ". Start profiling to count the runs of each block.");
    empty_text = "There are no compiled blocks. The interpreter doesn't compile any.";
    break;
  }
  case Tab::Network:
  {
    m_list_addresses.clear();
    for (DebuggerNetwork::Socket& socket : DebuggerNetwork::List(system))
    {
      list.push_back({std::move(socket.label), std::move(socket.value)});
      m_list_addresses.push_back(0);
    }
    empty_text = system.IsWii() || system.IsTriforce() ?
                     "There are no open sockets." :
                     "Sockets are shown for Wii and Triforce games.";
    break;
  }
  case Tab::Assembler:
    break;
  }

  if (IsListTab())
  {
    if (!m_list_addresses.empty())
      m_list_selected = std::min(m_list_selected, m_list_addresses.size() - 1);
    else
      m_list_selected = 0;
    snapshot.list_total = list.size();
    snapshot.list_selected = m_list_selected;
    snapshot.list_first = m_list_selected > LIST_WINDOW / 2 ? m_list_selected - LIST_WINDOW / 2 : 0;
    const size_t last = std::min(list.size(), snapshot.list_first + LIST_WINDOW);
    for (size_t i = snapshot.list_first; i < last; ++i)
      snapshot.list.push_back(std::move(list[i]));
    if (list.empty())
      snapshot.text_lines.push_back(std::move(empty_text));
  }
  return snapshot;
}

void DebuggerView::Draw(const DebuggerSnapshot& snapshot, const Frontend::ShellTheme& theme,
                        ImFont* font)
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  if (!viewport)
    return;
  ImGui::GetBackgroundDrawList()->AddRectFilled(
      viewport->Pos, {viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y},
      IM_COL32(0, 0, 0, 150));
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!ImGui::Begin("DolphinDebugger", nullptr, flags))
  {
    ImGui::End();
    return;
  }
  if (font)
    ImGui::PushFont(font);

  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float margin = std::max(24.0f, viewport->WorkSize.x * 0.03f);
  const ImVec2 panel_min{viewport->WorkPos.x + margin, viewport->WorkPos.y + margin};
  const ImVec2 panel_max{viewport->WorkPos.x + viewport->WorkSize.x - margin,
                         viewport->WorkPos.y + viewport->WorkSize.y - margin};
  Frontend::DrawShellPanelPx(draw, panel_min, panel_max, theme, 24.0f, 0.94f);

  const float line = ImGui::GetTextLineHeight();
  const float left = panel_min.x + 28.0f;
  const float right = panel_max.x - 28.0f;
  const ImU32 primary = Color(theme.textPrimary);
  const ImU32 secondary = Color(theme.textSecondary);
  const ImU32 accent = Color(theme.cursorNormal);
  const ImU32 breakpoint_color = IM_COL32(220, 50, 60, 255);
  const ImU32 patch_color = IM_COL32(240, 180, 40, 255);

  float y = panel_min.y + 22.0f;
  draw->AddText({left, y}, primary, "Debugger");
  const float status_width = ImGui::CalcTextSize(snapshot.status.c_str()).x;
  draw->AddText({right - status_width, y}, snapshot.stepping ? accent : secondary,
                snapshot.status.c_str());
  y += line + 18.0f;

  // A row of items scrolls so that the chosen one stays in view.
  const auto scroll_offset = [left, right](const std::vector<float>& widths, size_t chosen,
                                           float gap) {
    float end = 0.0f;
    for (size_t i = 0; i <= chosen && i < widths.size(); ++i)
      end += widths[i] + (i < chosen ? gap : 0.0f);
    return std::max(0.0f, end - (right - left));
  };

  {
    std::vector<float> widths;
    for (const char* const name : TAB_NAMES)
      widths.push_back(ImGui::CalcTextSize(name).x);
    float x = left - scroll_offset(widths, static_cast<size_t>(snapshot.tab), 32.0f);
    draw->PushClipRect({left - 4.0f, y - 4.0f}, {right + 4.0f, y + line + 10.0f}, true);
    for (size_t i = 0; i < TAB_NAMES.size(); ++i)
    {
      const bool selected = static_cast<int>(i) == snapshot.tab;
      draw->AddText({x, y}, selected ? primary : secondary, TAB_NAMES[i]);
      if (selected)
        draw->AddRectFilled({x, y + line + 4.0f}, {x + widths[i], y + line + 7.0f}, accent, 2.0f);
      x += widths[i] + 32.0f;
    }
    draw->PopClipRect();
  }
  y += line + 22.0f;

  const float command_height = line + 16.0f;
  {
    std::vector<float> widths;
    for (const std::string& command : snapshot.commands)
      widths.push_back(ImGui::CalcTextSize(command.c_str()).x + 28.0f);
    float x = left - scroll_offset(widths, snapshot.command, 10.0f);
    draw->PushClipRect({left - 6.0f, y - 6.0f}, {right + 6.0f, y + command_height + 6.0f}, true);
    for (size_t i = 0; i < snapshot.commands.size(); ++i)
    {
      const ImVec2 min{x, y};
      const ImVec2 max{x + widths[i], y + command_height};
      const bool focused = snapshot.commands_focused && i == snapshot.command;
      Frontend::DrawShellPanelPx(draw, min, max, theme, 12.0f, focused ? 0.86f : 0.52f);
      Frontend::DrawReplicaFocus(draw, min, max, 12.0f, focused, false);
      draw->AddText({x + 14.0f, y + 8.0f}, snapshot.stepping || i == 0 ? primary : secondary,
                    snapshot.commands[i].c_str());
      x += widths[i] + 10.0f;
    }
    draw->PopClipRect();
  }
  y += command_height + 18.0f;

  float content_top = y;
  const float content_bottom = panel_max.y - line - 34.0f;
  const float row_height = line + 8.0f;
  if (!snapshot.header.empty())
  {
    Frontend::DrawClippedText(draw, {left, content_top}, {right, content_top + line}, accent,
                              snapshot.header.c_str());
    content_top += row_height + 4.0f;
  }
  const auto row_background = [&](float row_y, bool selected, bool highlighted) {
    if (!selected && !highlighted)
      return;
    const ImVec2 min{left - 8.0f, row_y - 3.0f};
    const ImVec2 max{right + 8.0f, row_y + row_height - 3.0f};
    if (highlighted)
      draw->AddRectFilled(min, max, Frontend::ColorWithAlpha(theme.cursorNormal, 0.14f), 8.0f);
    if (selected && !snapshot.commands_focused)
      Frontend::DrawReplicaFocus(draw, min, max, 8.0f, true, false);
  };
  const auto cell_focus = [&](ImVec2 min, ImVec2 max) {
    if (snapshot.commands_focused)
      return;
    Frontend::DrawReplicaFocus(draw, min, max, 6.0f, true, false);
  };
  const auto draw_text_lines = [&](float top) {
    for (size_t i = 0; i < snapshot.text_lines.size(); ++i)
    {
      const float row_y = top + static_cast<float>(i) * row_height;
      if (row_y + line > content_bottom)
        break;
      Frontend::DrawClippedText(draw, {left, row_y}, {right, row_y + line},
                                snapshot.tab == 11 ? primary : secondary,
                                snapshot.text_lines[i].c_str());
    }
  };

  if (snapshot.tab == 11)
  {
    draw_text_lines(content_top);
  }
  else if (!snapshot.stepping)
  {
    draw->AddText({left, content_top}, secondary,
                  "The game is running. Choose Continue or press B to return to it.");
  }
  else if (snapshot.tab == 0)
  {
    const float column_address = left + 26.0f;
    const float column_mnemonic = column_address + ImGui::CalcTextSize("00000000").x + 32.0f;
    const float column_operands = column_mnemonic + ImGui::CalcTextSize("stwux.").x + 40.0f;
    const float column_symbol = left + (right - left) * 0.62f;
    const size_t visible = std::min(
        snapshot.code.size(), static_cast<size_t>((content_bottom - content_top) / row_height));
    const size_t first =
        snapshot.code_selected >= visible ? snapshot.code_selected - visible + 1 : 0;
    for (size_t i = first; i < std::min(snapshot.code.size(), first + visible); ++i)
    {
      const DebuggerSnapshot::CodeLine& code = snapshot.code[i];
      const float row_y = content_top + static_cast<float>(i - first) * row_height;
      row_background(row_y, i == snapshot.code_selected, code.pc);
      const float middle = row_y + line * 0.5f;
      if (code.breakpoint)
        draw->AddCircleFilled({left + 6.0f, middle}, 6.0f, breakpoint_color);
      if (code.pc)
      {
        draw->AddTriangleFilled({left + 12.0f, middle - 6.0f}, {left + 20.0f, middle},
                                {left + 12.0f, middle + 6.0f}, accent);
      }
      draw->AddText({column_address, row_y}, secondary,
                    fmt::format("{:08X}", code.address).c_str());
      draw->AddText({column_mnemonic, row_y}, code.patched ? patch_color : primary,
                    code.mnemonic.c_str());
      Frontend::DrawClippedText(draw, {column_operands, row_y},
                                {column_symbol - 16.0f, row_y + line},
                                code.patched ? patch_color : primary, code.operands.c_str());
      if (!code.symbol.empty())
      {
        Frontend::DrawClippedText(draw, {column_symbol, row_y}, {right, row_y + line}, accent,
                                  code.symbol.c_str());
      }
    }
  }
  else if (snapshot.tab == 1)
  {
    const float column_width = (right - left) / static_cast<float>(REGISTER_COLUMNS);
    for (size_t i = 0; i < std::min(INTEGER_REGISTERS, snapshot.registers.size()); ++i)
    {
      const float cell_x = left + static_cast<float>(i % REGISTER_COLUMNS) * column_width;
      const float cell_y = content_top + static_cast<float>(i / REGISTER_COLUMNS) * row_height;
      if (i == snapshot.register_selected)
      {
        cell_focus({cell_x - 8.0f, cell_y - 3.0f},
                   {cell_x + column_width - 16.0f, cell_y + row_height - 3.0f});
      }
      draw->AddText({cell_x, cell_y}, secondary, snapshot.registers[i].label.c_str());
      draw->AddText({cell_x + 56.0f, cell_y}, primary, snapshot.registers[i].value.c_str());
    }
    const float float_top = content_top + 10.0f * row_height + 12.0f;
    const float float_column = (right - left) / 2.0f;
    for (size_t i = INTEGER_REGISTERS; i < snapshot.registers.size(); ++i)
    {
      const size_t index = i - INTEGER_REGISTERS;
      const float cell_x = left + static_cast<float>(index % 2) * float_column;
      const float cell_y = float_top + static_cast<float>(index / 2) * row_height;
      if (cell_y + line > content_bottom)
        break;
      draw->AddText({cell_x, cell_y}, secondary, snapshot.registers[i].label.c_str());
      Frontend::DrawClippedText(draw, {cell_x + 56.0f, cell_y},
                                {cell_x + float_column - 16.0f, cell_y + line}, primary,
                                snapshot.registers[i].value.c_str());
    }
  }
  else if (snapshot.tab == 2)
  {
    const float byte_width = ImGui::CalcTextSize("00").x + 10.0f;
    const float column_bytes = left + ImGui::CalcTextSize("00000000").x + 28.0f;
    const float column_text = column_bytes + byte_width * MEMORY_ROW_BYTES + 24.0f;
    for (size_t row = 0; row < MEMORY_ROWS; ++row)
    {
      const float row_y = content_top + static_cast<float>(row) * row_height;
      if (row_y + line > content_bottom)
        break;
      if (row == snapshot.memory_selected)
      {
        const float word_x = column_bytes + byte_width * static_cast<float>(snapshot.memory_column *
                                                                            MEMORY_WORD_BYTES);
        cell_focus({word_x - 5.0f, row_y - 3.0f},
                   {word_x + byte_width * MEMORY_WORD_BYTES - 5.0f, row_y + row_height - 3.0f});
      }
      draw->AddText(
          {left, row_y}, secondary,
          fmt::format("{:08X}", snapshot.memory_address + static_cast<u32>(row) * MEMORY_ROW_BYTES)
              .c_str());
      std::string text;
      for (u32 column = 0; column < MEMORY_ROW_BYTES; ++column)
      {
        const size_t index = row * MEMORY_ROW_BYTES + column;
        const std::optional<u8> value =
            index < snapshot.memory.size() ? snapshot.memory[index] : std::nullopt;
        const bool watched =
            index < snapshot.memory_breakpoints.size() && snapshot.memory_breakpoints[index];
        draw->AddText({column_bytes + byte_width * static_cast<float>(column), row_y},
                      watched ? breakpoint_color : primary,
                      value ? fmt::format("{:02X}", *value).c_str() : "--");
        text.push_back(value && *value >= 0x20 && *value < 0x7F ? static_cast<char>(*value) : '.');
      }
      draw->AddText({column_text, row_y}, secondary, text.c_str());
    }
  }
  else
  {
    if (snapshot.list_total == 0)
      draw_text_lines(content_top);
    const size_t visible = static_cast<size_t>((content_bottom - content_top) / row_height);
    const size_t first =
        snapshot.list_selected >= visible ? snapshot.list_selected - visible + 1 : 0;
    for (size_t i = first; i < std::min(snapshot.list_total, first + visible); ++i)
    {
      if (i < snapshot.list_first || i - snapshot.list_first >= snapshot.list.size())
        continue;
      const DebuggerSnapshot::ListEntry& entry = snapshot.list[i - snapshot.list_first];
      const float row_y = content_top + static_cast<float>(i - first) * row_height;
      row_background(row_y, i == snapshot.list_selected, false);
      const float value_width = ImGui::CalcTextSize(entry.value.c_str()).x;
      Frontend::DrawClippedText(draw, {left, row_y}, {right - value_width - 24.0f, row_y + line},
                                primary, entry.label.c_str());
      draw->AddText({right - value_width, row_y}, secondary, entry.value.c_str());
    }
  }

  draw->AddText({left, panel_max.y - line - 20.0f}, secondary, snapshot.hint.c_str());

  const auto modal = [&](float width, float height) {
    const ImVec2 min{viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5f,
                     viewport->WorkPos.y + (viewport->WorkSize.y - height) * 0.5f};
    const ImVec2 max{min.x + width, min.y + height};
    Frontend::DrawShellPanelPx(draw, min, max, theme, 20.0f, 0.98f);
    return min;
  };

  if (!snapshot.entry_title.empty())
  {
    std::vector<float> widths;
    float cells_width = 0.0f;
    for (const std::string& cell : snapshot.entry_cells)
    {
      widths.push_back(std::max(ImGui::CalcTextSize("0").x, ImGui::CalcTextSize(cell.c_str()).x) +
                       18.0f);
      cells_width += widths.back();
    }
    const float width = std::max(
        {420.0f, cells_width + 48.0f, ImGui::CalcTextSize(snapshot.entry_title.c_str()).x + 48.0f});
    const ImVec2 min = modal(width, line * 2.0f + 96.0f);
    draw->AddText({min.x + 24.0f, min.y + 20.0f}, primary, snapshot.entry_title.c_str());
    float cell_x = min.x + (width - cells_width) * 0.5f;
    const float cells_y = min.y + line + 48.0f;
    for (size_t i = 0; i < snapshot.entry_cells.size(); ++i)
    {
      const ImVec2 cell_min{cell_x, cells_y - 6.0f};
      const ImVec2 cell_max{cell_x + widths[i] - 4.0f, cells_y + line + 6.0f};
      const bool selected = i == snapshot.entry_cursor;
      Frontend::DrawShellPanelPx(draw, cell_min, cell_max, theme, 8.0f, selected ? 0.9f : 0.5f);
      Frontend::DrawReplicaFocus(draw, cell_min, cell_max, 8.0f, selected, false);
      const float text_width = ImGui::CalcTextSize(snapshot.entry_cells[i].c_str()).x;
      draw->AddText({cell_min.x + (widths[i] - 4.0f - text_width) * 0.5f, cells_y}, primary,
                    snapshot.entry_cells[i].c_str());
      cell_x += widths[i];
    }
  }

  if (!snapshot.choice_title.empty())
  {
    float width = ImGui::CalcTextSize(snapshot.choice_title.c_str()).x + 48.0f;
    for (const std::string& choice : snapshot.choices)
      width = std::max(width, ImGui::CalcTextSize(choice.c_str()).x + 64.0f);
    width = std::min(std::max(width, 460.0f), panel_max.x - panel_min.x - 48.0f);
    const size_t shown = std::min<size_t>(snapshot.choices.size(), 12);
    const ImVec2 min = modal(width, line + 56.0f + static_cast<float>(shown) * row_height);
    draw->AddText({min.x + 24.0f, min.y + 20.0f}, primary, snapshot.choice_title.c_str());
    const size_t first =
        snapshot.choice_selected >= shown ? snapshot.choice_selected - shown + 1 : 0;
    for (size_t i = first; i < std::min(snapshot.choices.size(), first + shown); ++i)
    {
      const float row_y = min.y + line + 40.0f + static_cast<float>(i - first) * row_height;
      const ImVec2 row_min{min.x + 16.0f, row_y - 3.0f};
      const ImVec2 row_max{min.x + width - 16.0f, row_y + row_height - 3.0f};
      if (i == snapshot.choice_selected)
        Frontend::DrawReplicaFocus(draw, row_min, row_max, 8.0f, true, false);
      Frontend::DrawClippedText(draw, {min.x + 28.0f, row_y}, {min.x + width - 28.0f, row_y + line},
                                primary, snapshot.choices[i].c_str());
    }
  }

  if (!snapshot.text_title.empty())
  {
    const float width = std::min(panel_max.x - panel_min.x - 48.0f, 1100.0f);
    std::vector<std::string_view> lines;
    size_t caret_line = 0;
    size_t caret_column = 0;
    {
      size_t start = 0;
      while (true)
      {
        const size_t end = snapshot.text.find('\n', start);
        const size_t stop = end == std::string::npos ? snapshot.text.size() : end;
        if (snapshot.text_caret >= start && snapshot.text_caret <= stop)
        {
          caret_line = lines.size();
          caret_column = snapshot.text_caret - start;
        }
        lines.push_back(std::string_view(snapshot.text).substr(start, stop - start));
        if (end == std::string::npos)
          break;
        start = end + 1;
      }
    }
    const size_t shown = snapshot.text_multiline ? std::min<size_t>(14, lines.size() + 1) : 1;
    const ImVec2 min = modal(width, line + 64.0f + static_cast<float>(shown) * row_height);
    draw->AddText({min.x + 24.0f, min.y + 20.0f}, primary, snapshot.text_title.c_str());
    const size_t first = caret_line >= shown ? caret_line - shown + 1 : 0;
    const float text_left = min.x + 28.0f;
    const float text_right = min.x + width - 28.0f;
    for (size_t i = first; i < std::min(lines.size(), first + shown); ++i)
    {
      const float row_y = min.y + line + 44.0f + static_cast<float>(i - first) * row_height;
      const std::string text(lines[i]);
      // A long line scrolls so that the caret stays in view.
      float offset = 0.0f;
      float caret_x = 0.0f;
      if (i == caret_line)
      {
        caret_x = ImGui::CalcTextSize(text.substr(0, caret_column).c_str()).x;
        offset = std::max(0.0f, caret_x - (text_right - text_left) + 8.0f);
      }
      draw->PushClipRect({text_left, row_y - 4.0f}, {text_right, row_y + line + 4.0f}, true);
      draw->AddText({text_left - offset, row_y}, primary, text.c_str());
      if (i == caret_line)
      {
        const float x = text_left - offset + caret_x;
        draw->AddRectFilled({x, row_y}, {x + 2.0f, row_y + line}, accent);
      }
      draw->PopClipRect();
    }
  }

  if (font)
    ImGui::PopFont();
  ImGui::End();
}
}  // namespace DolphinUWP

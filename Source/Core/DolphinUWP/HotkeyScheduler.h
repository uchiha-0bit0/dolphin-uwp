// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"

namespace DolphinUWP
{
class HotkeyScheduler
{
public:
  enum class Action
  {
    OpenFile,
    RefreshLibrary,
    TogglePause,
    Stop,
    Reset,
    Screenshot,
    ToggleFullscreen,
    UnlockCursor,
    EjectDisc,
    ChangeDisc,
    StartRecording,
    PlayRecording,
    ExportRecording,
    ToggleReadOnly,
    ConnectWiiRemote,
    LoadStateSlot,
    SaveStateSlot,
    LoadLastState,
    LoadStateFile,
    SaveStateFile,
    SelectStateSlot,
    SaveSelectedState,
    LoadSelectedState,
    IncrementStateSlot,
    DecrementStateSlot,
    SaveOldestState,
    UndoLoadState,
    UndoSaveState,
    OpenSkylanderPortal,
    OpenInfinityBase,
    ActivateChat,
    RequestGolfControl,
    DebugStep,
    DebugStepOver,
    DebugStepOut,
    DebugSkip,
    DebugShowPC,
    DebugToggleBreakpoint,
    DebugAddBreakpoint,
    DebugAddMemoryBreakpoint,
    OpenAchievements,
    GBALoadROM,
    GBAUnloadROM,
  };

  struct Request
  {
    Action action;
    int argument = 0;
  };

  HotkeyScheduler();
  ~HotkeyScheduler();

  HotkeyScheduler(const HotkeyScheduler&) = delete;
  HotkeyScheduler& operator=(const HotkeyScheduler&) = delete;

  void Start();
  void Stop();

  // Hotkeys apply while a game is shown without an overlay on top. Without a game, only the
  // library hotkeys apply.
  void SetEnabled(bool enabled);

  std::vector<Request> TakeRequests();

private:
  void Run();
  void Queue(Action action, int argument = 0);
  void HandleFrameStep();

  std::thread m_thread;
  std::atomic<bool> m_stop_requested = false;
  std::mutex m_mutex;
  std::vector<Request> m_requests;

  int m_frame_step_count = 0;
  int m_frame_step_delay = 1;
  int m_frame_step_delay_count = 0;
  bool m_frame_step_hold = false;
};
}  // namespace DolphinUWP

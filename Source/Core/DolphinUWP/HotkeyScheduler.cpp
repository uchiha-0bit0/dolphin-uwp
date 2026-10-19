// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/HotkeyScheduler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <fmt/format.h>

#include "AudioCommon/AudioCommon.h"
#include "Common/Config/Config.h"
#include "Common/Thread.h"
#include "Core/AchievementManager.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Core.h"
#include "Core/FreeLookManager.h"
#include "Core/HotkeyManager.h"
#include "Core/State.h"
#include "Core/System.h"
#include "DolphinUWP/GBAControls.h"
#include "DolphinUWP/GBAScreens.h"
#include "DolphinUWP/UWPSettings.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputProfile.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

namespace DolphinUWP
{
namespace
{
constexpr const char* DUBOIS_ALGORITHM_SHADER = "dubois";

bool IsHotkey(int id, bool held = false)
{
  return HotkeyManagerEmu::IsPressed(id, held);
}

void ShowVolume()
{
  OSD::AddMessage(
      fmt::format("Volume: {}", Config::Get(Config::MAIN_AUDIO_MUTED) ?
                                    "Muted" :
                                    fmt::format("{}%", Config::Get(Config::MAIN_AUDIO_VOLUME))));
}

void ShowEmulationSpeed()
{
  const float speed = Config::Get(Config::MAIN_EMULATION_SPEED);
  OSD::AddMessage(speed <= 0 ? "Speed Limit: Unlimited" :
                               fmt::format("Speed Limit: {}%", std::lround(speed * 100.f)));
}

void ShowEFBScale(int scale)
{
  if (scale == EFB_SCALE_AUTO_INTEGRAL)
    OSD::AddMessage("Internal Resolution: Auto (integral)");
  else if (scale == 1)
    OSD::AddMessage("Internal Resolution: Native");
  else
    OSD::AddMessage(fmt::format("Internal Resolution: {}x", scale));
}

void ShowXFBCopies()
{
  OSD::AddMessage(fmt::format(
      "Copy XFB: {}{}", Config::Get(Config::GFX_HACK_IMMEDIATE_XFB) ? " (Immediate)" : "",
      Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM) ? "to Texture" : "to RAM"));
}

void ToggleStereoMode(StereoMode mode)
{
  if (Config::Get(Config::GFX_STEREO_MODE) == mode)
  {
    Config::SetCurrent(Config::GFX_STEREO_MODE, StereoMode::Off);
    if (mode == StereoMode::Anaglyph)
      Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, "");
    return;
  }

  if (mode == StereoMode::Anaglyph)
    Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, DUBOIS_ALGORITHM_SHADER);
  else if (Config::Get(Config::GFX_ENHANCE_POST_SHADER) == DUBOIS_ALGORITHM_SHADER)
    Config::SetCurrent(Config::GFX_ENHANCE_POST_SHADER, "");
  Config::SetCurrent(Config::GFX_STEREO_MODE, mode);
}

void ToggleAspectRatio()
{
  const int aspect_ratio = (static_cast<int>(Config::Get(Config::GFX_ASPECT_RATIO)) + 1) & 3;
  Config::SetCurrent(Config::GFX_ASPECT_RATIO, static_cast<AspectMode>(aspect_ratio));
  switch (static_cast<AspectMode>(aspect_ratio))
  {
  case AspectMode::Stretch:
    OSD::AddMessage("Stretch");
    break;
  case AspectMode::ForceStandard:
    OSD::AddMessage("Force 4:3");
    break;
  case AspectMode::ForceWide:
    OSD::AddMessage("Force 16:9");
    break;
  default:
    OSD::AddMessage("Auto");
    break;
  }
}

void HandleGraphicsHotkeys()
{
  const int efb_scale = Config::Get(Config::GFX_EFB_SCALE);
  if (IsHotkey(HK_INCREASE_IR))
  {
    Config::SetCurrent(Config::GFX_EFB_SCALE, efb_scale + 1);
    ShowEFBScale(efb_scale + 1);
  }
  if (IsHotkey(HK_DECREASE_IR) && efb_scale > EFB_SCALE_AUTO_INTEGRAL)
  {
    Config::SetCurrent(Config::GFX_EFB_SCALE, efb_scale - 1);
    ShowEFBScale(efb_scale - 1);
  }

  if (IsHotkey(HK_TOGGLE_CROP_TO_ASPECT_RATIO))
  {
    Config::SetCurrent(Config::GFX_CROP_TO_ASPECT_RATIO,
                       !Config::Get(Config::GFX_CROP_TO_ASPECT_RATIO));
  }
  if (IsHotkey(HK_TOGGLE_CROP_CUSTOM))
    Config::SetCurrent(Config::GFX_CROP_CUSTOM, !Config::Get(Config::GFX_CROP_CUSTOM));
  if (IsHotkey(HK_TOGGLE_AR))
    ToggleAspectRatio();

  if (IsHotkey(HK_TOGGLE_SKIP_EFB_ACCESS))
  {
    const bool skip = !Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE);
    Config::SetCurrent(Config::GFX_HACK_EFB_ACCESS_ENABLE, skip);
    OSD::AddMessage(fmt::format("{} EFB Access from CPU", skip ? "Skip" : "Don't skip"));
  }
  if (IsHotkey(HK_TOGGLE_EFBCOPIES))
  {
    const bool to_texture = !Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
    Config::SetCurrent(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM, to_texture);
    OSD::AddMessage(fmt::format("Copy EFB: {}", to_texture ? "to Texture" : "to RAM"));
  }
  if (IsHotkey(HK_TOGGLE_XFBCOPIES))
  {
    Config::SetCurrent(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM,
                       !Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM));
    ShowXFBCopies();
  }
  if (IsHotkey(HK_TOGGLE_IMMEDIATE_XFB))
  {
    Config::SetCurrent(Config::GFX_HACK_IMMEDIATE_XFB,
                       !Config::Get(Config::GFX_HACK_IMMEDIATE_XFB));
    ShowXFBCopies();
  }
  if (IsHotkey(HK_TOGGLE_FOG))
  {
    const bool disabled = !Config::Get(Config::GFX_DISABLE_FOG);
    Config::SetCurrent(Config::GFX_DISABLE_FOG, disabled);
    OSD::AddMessage(fmt::format("Fog: {}", disabled ? "Disabled" : "Enabled"));
  }
  if (IsHotkey(HK_TOGGLE_DUMPTEXTURES))
  {
    const bool dumping = !Config::Get(Config::GFX_DUMP_TEXTURES);
    Config::SetCurrent(Config::GFX_DUMP_TEXTURES, dumping);
    OSD::AddMessage(fmt::format("Texture Dumping {}",
                                dumping ? "enabled. This will reduce performance." : "disabled."));
  }
  if (IsHotkey(HK_TOGGLE_TEXTURES))
    Config::SetCurrent(Config::GFX_HIRES_TEXTURES, !Config::Get(Config::GFX_HIRES_TEXTURES));

  if (IsHotkey(HK_TOGGLE_STEREO_SIDE_BY_SIDE))
    ToggleStereoMode(StereoMode::SideBySide);
  if (IsHotkey(HK_TOGGLE_STEREO_TOP_AND_BOTTOM))
    ToggleStereoMode(StereoMode::TopAndBottom);
  if (IsHotkey(HK_TOGGLE_STEREO_ANAGLYPH))
    ToggleStereoMode(StereoMode::Anaglyph);
}

void HandleThrottleHotkey(Core::System& system)
{
  const bool held = IsHotkey(HK_TOGGLE_THROTTLE, true);
  Core::SetIsThrottlerTempDisabled(held);

  if (held && !Config::Get(Config::MAIN_AUDIO_MUTED) &&
      Config::Get(Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT))
  {
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, true);
    AudioCommon::UpdateSoundStream(system);
  }
  else if (!held && Config::Get(Config::MAIN_AUDIO_MUTED) &&
           Config::GetActiveLayerForConfig(Config::MAIN_AUDIO_MUTED) ==
               Config::LayerType::CurrentRun)
  {
    Config::DeleteKey(Config::LayerType::CurrentRun, Config::MAIN_AUDIO_MUTED);
    AudioCommon::UpdateSoundStream(system);
  }
}

void HandleWiiHotkeys()
{
  if (IsHotkey(HK_TOGGLE_SD_CARD))
  {
    const bool inserted = !Config::Get(Config::MAIN_WII_SD_CARD);
    Config::SetBaseOrCurrent(Config::MAIN_WII_SD_CARD, inserted);
    OSD::AddMessage(inserted ? "SD card inserted" : "SD card removed");
  }

  if (IsHotkey(HK_TOGGLE_USB_KEYBOARD))
  {
    const bool connected = !Config::Get(Config::MAIN_WII_KEYBOARD);
    Config::SetBaseOrCurrent(Config::MAIN_WII_KEYBOARD, connected);
    OSD::AddMessage(connected ? "USB keyboard connected" : "USB keyboard disconnected");
  }

  if (IsHotkey(HK_TOGGLE_WII_SPEAK_MUTE))
  {
    const bool muted = !Config::Get(Config::MAIN_WII_SPEAK_MUTED);
    Config::SetBaseOrCurrent(Config::MAIN_WII_SPEAK_MUTED, muted);
    OSD::AddMessage(muted ? "Wii Speak muted" : "Wii Speak unmuted");
  }
}

// The GBA screens aren't windows, so these act on every running GBA.
void HandleGBAHotkeys()
{
  const bool reset = IsHotkey(HK_GBA_RESET);
  const bool volume_down = IsHotkey(HK_GBA_VOLUME_DOWN);
  const bool volume_up = IsHotkey(HK_GBA_VOLUME_UP);
  const bool mute = IsHotkey(HK_GBA_TOGGLE_MUTE);
  for (size_t port = 0; port < DolphinUWP::GBAScreens::MAX_SCREENS; ++port)
  {
    const std::optional<DolphinUWP::GBAScreens::Status> status =
        DolphinUWP::GBAScreens::GetStatus(port);
    if (!status)
      continue;
    if (reset)
      DolphinUWP::GBAScreens::Reset(port);
    if (volume_down)
      DolphinUWP::GBAScreens::SetVolume(port, status->volume - 10);
    if (volume_up)
      DolphinUWP::GBAScreens::SetVolume(port, status->volume + 10);
    if (mute)
      DolphinUWP::GBAScreens::SetMuted(port, !status->muted);
  }

  constexpr std::array sizes{HK_GBA_1X, HK_GBA_2X, HK_GBA_3X, HK_GBA_4X};
  for (size_t i = 0; i < sizes.size(); ++i)
  {
    if (IsHotkey(sizes[i]))
      Config::SetCurrent(Config::UWP_GBA_SCALE, static_cast<int>(i + 1));
  }

  constexpr std::array switches{HK_GBA_SWITCH_CONTROLS_1, HK_GBA_SWITCH_CONTROLS_2,
                                HK_GBA_SWITCH_CONTROLS_3, HK_GBA_SWITCH_CONTROLS_4};
  for (size_t player = 0; player < switches.size(); ++player)
  {
    if (IsHotkey(switches[player]))
      DolphinUWP::GBAControls::SwitchPlayer(player);
  }
}

void HandleProfileHotkeys(InputProfile::ProfileCycler& cycler)
{
  constexpr std::array previous{HK_PREV_WIIMOTE_PROFILE_1, HK_PREV_WIIMOTE_PROFILE_2,
                                HK_PREV_WIIMOTE_PROFILE_3, HK_PREV_WIIMOTE_PROFILE_4};
  constexpr std::array next{HK_NEXT_WIIMOTE_PROFILE_1, HK_NEXT_WIIMOTE_PROFILE_2,
                            HK_NEXT_WIIMOTE_PROFILE_3, HK_NEXT_WIIMOTE_PROFILE_4};
  constexpr std::array previous_game{HK_PREV_GAME_WIIMOTE_PROFILE_1, HK_PREV_GAME_WIIMOTE_PROFILE_2,
                                     HK_PREV_GAME_WIIMOTE_PROFILE_3,
                                     HK_PREV_GAME_WIIMOTE_PROFILE_4};
  constexpr std::array next_game{HK_NEXT_GAME_WIIMOTE_PROFILE_1, HK_NEXT_GAME_WIIMOTE_PROFILE_2,
                                 HK_NEXT_GAME_WIIMOTE_PROFILE_3, HK_NEXT_GAME_WIIMOTE_PROFILE_4};

  for (int i = 0; i < 4; ++i)
  {
    if (IsHotkey(previous[i]))
      cycler.PreviousWiimoteProfile(i);
    else if (IsHotkey(next[i]))
      cycler.NextWiimoteProfile(i);

    if (IsHotkey(previous_game[i]))
      cycler.PreviousWiimoteProfileForGame(i);
    else if (IsHotkey(next_game[i]))
      cycler.NextWiimoteProfileForGame(i);
  }
}

void HandleStereoDepthHotkeys()
{
  const float depth = Config::Get(Config::GFX_STEREO_DEPTH);
  if (IsHotkey(HK_DECREASE_DEPTH, true))
    Config::SetCurrent(Config::GFX_STEREO_DEPTH, std::max(depth - 1, 0.0f));
  if (IsHotkey(HK_INCREASE_DEPTH, true))
    Config::SetCurrent(Config::GFX_STEREO_DEPTH,
                       std::min(depth + 1, Config::GFX_STEREO_DEPTH_MAXIMUM));

  const float convergence = Config::Get(Config::GFX_STEREO_CONVERGENCE);
  if (IsHotkey(HK_DECREASE_CONVERGENCE, true))
    Config::SetCurrent(Config::GFX_STEREO_CONVERGENCE, std::max(convergence - 5, 0.0f));
  if (IsHotkey(HK_INCREASE_CONVERGENCE, true))
  {
    Config::SetCurrent(Config::GFX_STEREO_CONVERGENCE,
                       std::min(convergence + 5, Config::GFX_STEREO_CONVERGENCE_MAXIMUM));
  }
}
}  // namespace

HotkeyScheduler::HotkeyScheduler()
{
  HotkeyManagerEmu::Enable(false);
}

HotkeyScheduler::~HotkeyScheduler()
{
  Stop();
}

void HotkeyScheduler::Start()
{
  if (m_thread.joinable())
    return;
  m_stop_requested = false;
  m_thread = std::thread(&HotkeyScheduler::Run, this);
}

void HotkeyScheduler::Stop()
{
  m_stop_requested = true;
  if (m_thread.joinable())
    m_thread.join();
}

void HotkeyScheduler::SetEnabled(bool enabled)
{
  if (HotkeyManagerEmu::IsEnabled() == enabled)
    return;
  HotkeyManagerEmu::Enable(enabled);
  if (!enabled)
    Core::SetIsThrottlerTempDisabled(false);
}

std::vector<HotkeyScheduler::Request> HotkeyScheduler::TakeRequests()
{
  std::lock_guard lock(m_mutex);
  return std::exchange(m_requests, {});
}

void HotkeyScheduler::Queue(Action action, int argument)
{
  std::lock_guard lock(m_mutex);
  m_requests.push_back({action, argument});
}

void HotkeyScheduler::HandleFrameStep()
{
  constexpr int MAX_FRAME_STEP_DELAY = 60;
  constexpr int FRAME_STEP_DELAY = 30;

  if (IsHotkey(HK_FRAME_ADVANCE_INCREASE_SPEED))
  {
    m_frame_step_delay = std::max(m_frame_step_delay - 1, 0);
    return;
  }
  if (IsHotkey(HK_FRAME_ADVANCE_DECREASE_SPEED))
  {
    m_frame_step_delay = std::min(m_frame_step_delay + 1, MAX_FRAME_STEP_DELAY);
    return;
  }
  if (IsHotkey(HK_FRAME_ADVANCE_RESET_SPEED))
  {
    m_frame_step_delay = 1;
    return;
  }

  if (!IsHotkey(HK_FRAME_ADVANCE, true))
  {
    m_frame_step_count = 0;
    m_frame_step_hold = false;
    m_frame_step_delay_count = 0;
    return;
  }

  if (m_frame_step_delay_count < m_frame_step_delay && m_frame_step_hold)
    ++m_frame_step_delay_count;

  if ((m_frame_step_count == 0 || m_frame_step_count == FRAME_STEP_DELAY) && !m_frame_step_hold)
  {
    Core::QueueHostJob([](Core::System& system) { Core::DoFrameStep(system); });
    m_frame_step_hold = true;
  }

  if (m_frame_step_count < FRAME_STEP_DELAY)
  {
    ++m_frame_step_count;
    m_frame_step_hold = false;
  }

  if (m_frame_step_count == FRAME_STEP_DELAY && m_frame_step_hold &&
      m_frame_step_delay_count >= m_frame_step_delay)
  {
    m_frame_step_hold = false;
    m_frame_step_delay_count = 0;
  }
}

void HotkeyScheduler::Run()
{
  Common::SetCurrentThreadName("HotkeyScheduler");
  InputProfile::ProfileCycler profile_cycler;

  while (!m_stop_requested)
  {
    Common::SleepCurrentThread(5);

    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::FreeLook);
    g_controller_interface.UpdateInput();
    FreeLook::UpdateInput();

    g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
    g_controller_interface.UpdateInput();

    if (!HotkeyManagerEmu::IsEnabled())
      continue;

    Core::System& system = Core::System::GetInstance();
    const Core::State state = Core::GetState(system);
    if (state == Core::State::Stopping)
      continue;

    Core::UpdateInputGate(Config::Get(Config::MAIN_FOCUSED_HOTKEYS));
    HotkeyManagerEmu::GetStatus(false);
    ControlReference::SetInputGate(true);
    HotkeyManagerEmu::GetStatus(true);

    if (IsHotkey(HK_OPEN))
      Queue(Action::OpenFile);
    if (IsHotkey(HK_REFRESH_LIST))
      Queue(Action::RefreshLibrary);

    if (state == Core::State::Uninitialized)
    {
      if (IsHotkey(HK_PLAY_RECORDING))
        Queue(Action::PlayRecording);
      continue;
    }

    if (IsHotkey(HK_START_RECORDING))
      Queue(Action::StartRecording);
    if (IsHotkey(HK_EXIT))
      Queue(Action::Stop);
    if (IsHotkey(HK_EJECT_DISC))
      Queue(Action::EjectDisc);
    if (IsHotkey(HK_CHANGE_DISC))
      Queue(Action::ChangeDisc);
    if (IsHotkey(HK_FULLSCREEN))
      Queue(Action::ToggleFullscreen);
    if (IsHotkey(HK_PLAY_PAUSE))
      Queue(Action::TogglePause);
    if (IsHotkey(HK_STOP))
      Queue(Action::Stop);
    if (IsHotkey(HK_RESET))
      Queue(Action::Reset);
    HandleFrameStep();
    if (IsHotkey(HK_SCREENSHOT))
      Queue(Action::Screenshot);
    if (IsHotkey(HK_UNLOCK_CURSOR))
      Queue(Action::UnlockCursor);
    if (IsHotkey(HK_CENTER_MOUSE, true))
      g_controller_interface.SetMouseCenteringRequested(true);
    if (IsHotkey(HK_ACTIVATE_CHAT))
      Queue(Action::ActivateChat);
    if (IsHotkey(HK_REQUEST_GOLF_CONTROL))
      Queue(Action::RequestGolfControl);
    if (IsHotkey(HK_EXPORT_RECORDING))
      Queue(Action::ExportRecording);
    if (IsHotkey(HK_READ_ONLY_MODE))
      Queue(Action::ToggleReadOnly);

    if (Config::IsDebuggingEnabled())
    {
      if (IsHotkey(HK_STEP))
        Queue(Action::DebugStep);
      if (IsHotkey(HK_STEP_OVER))
        Queue(Action::DebugStepOver);
      if (IsHotkey(HK_STEP_OUT))
        Queue(Action::DebugStepOut);
      if (IsHotkey(HK_SKIP) || IsHotkey(HK_SET_PC))
        Queue(Action::DebugSkip);
      if (IsHotkey(HK_SHOW_PC))
        Queue(Action::DebugShowPC);
      if (IsHotkey(HK_BP_TOGGLE))
        Queue(Action::DebugToggleBreakpoint);
      if (IsHotkey(HK_BP_ADD))
        Queue(Action::DebugAddBreakpoint);
      if (IsHotkey(HK_MBP_ADD))
        Queue(Action::DebugAddMemoryBreakpoint);
    }

    if (system.IsWii())
    {
      constexpr std::array connect{HK_WIIMOTE1_CONNECT, HK_WIIMOTE2_CONNECT, HK_WIIMOTE3_CONNECT,
                                   HK_WIIMOTE4_CONNECT, HK_BALANCEBOARD_CONNECT};
      for (int i = 0; i < static_cast<int>(connect.size()); ++i)
      {
        if (IsHotkey(connect[i]))
          Queue(Action::ConnectWiiRemote, i);
      }
      HandleWiiHotkeys();
    }
    HandleProfileHotkeys(profile_cycler);
    HandleGBAHotkeys();
    if (IsHotkey(HK_GBA_LOAD))
      Queue(Action::GBALoadROM);
    if (IsHotkey(HK_GBA_UNLOAD))
      Queue(Action::GBAUnloadROM);
#ifdef USE_RETRO_ACHIEVEMENTS
    if (IsHotkey(HK_OPEN_ACHIEVEMENTS))
      Queue(Action::OpenAchievements);
#endif

    if (IsHotkey(HK_VOLUME_DOWN))
    {
      AudioCommon::DecreaseVolume(system, 3);
      ShowVolume();
    }
    if (IsHotkey(HK_VOLUME_UP))
    {
      AudioCommon::IncreaseVolume(system, 3);
      ShowVolume();
    }
    if (IsHotkey(HK_VOLUME_TOGGLE_MUTE))
    {
      AudioCommon::ToggleMuteVolume(system);
      ShowVolume();
    }

    HandleGraphicsHotkeys();
    HandleThrottleHotkey(system);

    if (IsHotkey(HK_DECREASE_EMULATION_SPEED))
    {
      double speed = Config::Get(Config::MAIN_EMULATION_SPEED) - 0.1;
      if (speed > 0)
      {
        speed = (speed >= 0.95 && speed <= 1.05) ? 1.0 : speed;
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, static_cast<float>(speed));
      }
      ShowEmulationSpeed();
    }
    if (IsHotkey(HK_INCREASE_EMULATION_SPEED))
    {
      double speed = Config::Get(Config::MAIN_EMULATION_SPEED) + 0.1;
      speed = (speed >= 0.95 && speed <= 1.05) ? 1.0 : speed;
      Config::SetCurrent(Config::MAIN_EMULATION_SPEED, static_cast<float>(speed));
      ShowEmulationSpeed();
    }

    if (IsHotkey(HK_SKYLANDERS_PORTAL))
      Queue(Action::OpenSkylanderPortal);
    if (IsHotkey(HK_INFINITY_BASE))
      Queue(Action::OpenInfinityBase);

    if (IsHotkey(HK_SAVE_STATE_SLOT_SELECTED))
      Queue(Action::SaveSelectedState);
    if (IsHotkey(HK_LOAD_STATE_SLOT_SELECTED))
      Queue(Action::LoadSelectedState);
    if (IsHotkey(HK_INCREMENT_SELECTED_STATE_SLOT))
      Queue(Action::IncrementStateSlot);
    if (IsHotkey(HK_DECREMENT_SELECTED_STATE_SLOT))
      Queue(Action::DecrementStateSlot);

    HandleStereoDepthHotkeys();

    if (IsHotkey(HK_FREELOOK_TOGGLE))
    {
      const bool enabled = !Config::Get(Config::FREE_LOOK_ENABLED);
      Config::SetCurrent(Config::FREE_LOOK_ENABLED, enabled);
      if (AchievementManager::GetInstance().IsHardcoreModeActive())
        OSD::AddMessage("Free Look is Disabled in Hardcore Mode");
      else
        OSD::AddMessage(fmt::format("Free Look: {}", enabled ? "Enabled" : "Disabled"));
    }

    for (u32 i = 0; i < State::NUM_STATES; ++i)
    {
      const int slot = static_cast<int>(i + 1);
      if (IsHotkey(HK_LOAD_STATE_SLOT_1 + i))
        Queue(Action::LoadStateSlot, slot);
      if (IsHotkey(HK_SAVE_STATE_SLOT_1 + i))
        Queue(Action::SaveStateSlot, slot);
      if (IsHotkey(HK_LOAD_LAST_STATE_1 + i))
        Queue(Action::LoadLastState, slot);
      if (IsHotkey(HK_SELECT_STATE_SLOT_1 + i))
        Queue(Action::SelectStateSlot, slot);
    }
    if (IsHotkey(HK_SAVE_FIRST_STATE))
      Queue(Action::SaveOldestState);
    if (IsHotkey(HK_UNDO_LOAD_STATE))
      Queue(Action::UndoLoadState);
    if (IsHotkey(HK_UNDO_SAVE_STATE))
      Queue(Action::UndoSaveState);
    if (IsHotkey(HK_LOAD_STATE_FILE))
      Queue(Action::LoadStateFile);
    if (IsHotkey(HK_SAVE_STATE_FILE))
      Queue(Action::SaveStateFile);
  }
}
}  // namespace DolphinUWP

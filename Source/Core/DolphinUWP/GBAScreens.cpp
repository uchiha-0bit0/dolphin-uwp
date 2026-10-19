// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GBAScreens.h"

#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <span>
#include <utility>

#ifdef HAS_LIBMGBA

#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GBACore.h"
#include "Core/HW/GBAPad.h"
#include "Core/Host.h"
#include "Core/System.h"

namespace DolphinUWP::GBAScreens
{
namespace
{
// The emulated GBA leaves the alpha channel unset.
constexpr u32 OPAQUE_ALPHA = 0xFF000000;

struct Screen
{
  bool active = false;
  u32 width = 0;
  u32 height = 0;
  u64 frame = 0;
  std::vector<u32> pixels;
  std::weak_ptr<HW::GBA::Core> core;
  Status status;
};

std::mutex s_mutex;
std::array<Screen, MAX_SCREENS> s_screens;

void ApplyVolume(size_t index, const Status& status)
{
  SoundStream* const sound_stream = Core::System::GetInstance().GetSoundStream();
  if (!sound_stream)
    return;
  const u32 volume = status.muted ? 0 : static_cast<u32>(status.volume * 256 / 100);
  sound_stream->GetMixer()->SetGBAVolume(index, volume, volume);
}

std::weak_ptr<HW::GBA::Core> GetCore(size_t index)
{
  if (index >= MAX_SCREENS)
    return {};
  std::lock_guard lock(s_mutex);
  return s_screens[index].active ? s_screens[index].core : std::weak_ptr<HW::GBA::Core>();
}

void RunOnCore(size_t index, std::function<void(HW::GBA::Core&)> function)
{
  std::weak_ptr<HW::GBA::Core> core = GetCore(index);
  if (core.expired())
    return;
  Core::RunOnCPUThread(Core::System::GetInstance(),
                       [core = std::move(core), function = std::move(function)] {
                         if (const auto core_ptr = core.lock())
                           function(*core_ptr);
                       });
}

class Host final : public GBAHostInterface
{
public:
  explicit Host(std::weak_ptr<HW::GBA::Core> core) : m_core(std::move(core))
  {
    const auto core_ptr = m_core.lock();
    if (!core_ptr)
      return;
    const HW::GBA::CoreInfo info = core_ptr->GetCoreInfo();
    if (info.device_number < 0 || static_cast<size_t>(info.device_number) >= MAX_SCREENS)
      return;
    m_index = static_cast<size_t>(info.device_number);
    Status status;
    {
      std::lock_guard lock(s_mutex);
      Screen& screen = s_screens[m_index];
      screen = {.active = true, .width = info.width, .height = info.height, .core = m_core};
      screen.status.has_rom = info.has_rom;
      screen.status.has_ereader = info.has_ereader;
      screen.status.title = info.game_title;
      status = screen.status;
    }
    ApplyVolume(m_index, status);
  }

  ~Host() override
  {
    if (m_index >= MAX_SCREENS)
      return;
    std::lock_guard lock(s_mutex);
    s_screens[m_index] = {};
  }

  void GameChanged() override
  {
    const auto core_ptr = m_core.lock();
    if (!core_ptr || m_index >= MAX_SCREENS)
      return;
    const HW::GBA::CoreInfo info = core_ptr->GetCoreInfo();
    std::lock_guard lock(s_mutex);
    Screen& screen = s_screens[m_index];
    screen.width = info.width;
    screen.height = info.height;
    screen.status.has_rom = info.has_rom;
    screen.status.has_ereader = info.has_ereader;
    screen.status.title = info.game_title;
  }

  void FrameEnded(std::span<const u32> video_buffer) override
  {
    if (m_index >= MAX_SCREENS)
      return;
    std::lock_guard lock(s_mutex);
    Screen& screen = s_screens[m_index];
    if (video_buffer.size() != static_cast<size_t>(screen.width) * screen.height)
      return;
    screen.pixels.resize(video_buffer.size());
    for (size_t i = 0; i < video_buffer.size(); ++i)
      screen.pixels[i] = video_buffer[i] | OPAQUE_ALPHA;
    ++screen.frame;
  }

private:
  std::weak_ptr<HW::GBA::Core> m_core;
  size_t m_index = MAX_SCREENS;
};
}  // namespace

std::unique_ptr<GBAHostInterface> CreateHost(std::weak_ptr<HW::GBA::Core> core)
{
  return std::make_unique<Host>(std::move(core));
}

bool CopyScreen(size_t index, u64* frame, u32* width, u32* height, std::vector<u32>* pixels)
{
  std::lock_guard lock(s_mutex);
  if (index >= MAX_SCREENS || !s_screens[index].active || s_screens[index].pixels.empty())
    return false;
  const Screen& screen = s_screens[index];
  *width = screen.width;
  *height = screen.height;
  if (screen.frame != *frame)
  {
    *frame = screen.frame;
    *pixels = screen.pixels;
  }
  return true;
}

std::optional<Status> GetStatus(size_t index)
{
  if (index >= MAX_SCREENS)
    return std::nullopt;
  std::lock_guard lock(s_mutex);
  if (!s_screens[index].active)
    return std::nullopt;
  return s_screens[index].status;
}

bool IsRunning(size_t index)
{
  return GetStatus(index).has_value();
}

void Reset(size_t index)
{
  if (IsRunning(index))
    Pad::SetGBAReset(static_cast<int>(index), true);
}

void SetConnected(size_t index, bool connected)
{
  {
    std::lock_guard lock(s_mutex);
    if (index >= MAX_SCREENS || !s_screens[index].active)
      return;
    s_screens[index].status.connected = connected;
  }
  RunOnCore(index, [connected](HW::GBA::Core& core) { core.SetForceDisconnect(!connected); });
}

void LoadROM(size_t index, std::string path)
{
  // The core restarts on the CPU thread and reports its title once it runs. Until then the file
  // name stands in for it.
  {
    std::lock_guard lock(s_mutex);
    if (index >= MAX_SCREENS || !s_screens[index].active)
      return;
    Status& status = s_screens[index].status;
    status.has_rom = !path.empty();
    status.has_ereader = false;
    const size_t start = path.find_last_of("\\/") + 1;
    const size_t dot = path.find_last_of('.');
    status.title = path.substr(start, dot != std::string::npos && dot > start ? dot - start :
                                                                                std::string::npos);
  }
  RunOnCore(index, [path = std::move(path)](HW::GBA::Core& core) {
    // The core opens ROMs itself, which works only inside the app's folders.
    std::string rom_path = path;
    const std::string roms = File::GetUserPath(D_GBAUSER_IDX) + "ROMs/";
    if (!rom_path.empty() && !rom_path.starts_with(File::GetUserPath(D_USER_IDX)))
    {
      const std::string name = rom_path.substr(rom_path.find_last_of("\\/") + 1);
      if (!File::CreateFullPath(roms) || !File::Copy(rom_path, roms + name))
        return;
      rom_path = roms + name;
    }

    const auto& info = Config::MAIN_GBA_ROM_PATHS[core.GetCoreInfo().device_number];
    auto& core_timing = Core::System::GetInstance().GetCoreTiming();
    core.Stop();
    Config::SetCurrent(info, rom_path);
    if (core.Start(core_timing.GetTicks()))
      return;
    Config::SetCurrent(info, Config::GetBase(info));
    core.Start(core_timing.GetTicks());
  });
}

void QueueEReaderCard(size_t index, std::string path)
{
  RunOnCore(index, [path = std::move(path)](HW::GBA::Core& core) { core.EReaderQueueCard(path); });
}

void ImportState(size_t index, std::string path)
{
  RunOnCore(index, [path = std::move(path)](HW::GBA::Core& core) { core.ImportState(path); });
}

void ExportState(size_t index, std::string path)
{
  RunOnCore(index, [path = std::move(path)](HW::GBA::Core& core) { core.ExportState(path); });
}

void ImportSave(size_t index, std::string path)
{
  RunOnCore(index, [path = std::move(path)](HW::GBA::Core& core) { core.ImportSave(path); });
}

void ExportSave(size_t index, std::string path)
{
  RunOnCore(index, [path = std::move(path)](HW::GBA::Core& core) { core.ExportSave(path); });
}

void SetVolume(size_t index, int volume)
{
  Status status;
  {
    std::lock_guard lock(s_mutex);
    if (index >= MAX_SCREENS || !s_screens[index].active)
      return;
    s_screens[index].status.volume = std::clamp(volume, 0, 100);
    s_screens[index].status.muted = false;
    status = s_screens[index].status;
  }
  ApplyVolume(index, status);
}

void SetMuted(size_t index, bool muted)
{
  Status status;
  {
    std::lock_guard lock(s_mutex);
    if (index >= MAX_SCREENS || !s_screens[index].active)
      return;
    s_screens[index].status.muted = muted;
    status = s_screens[index].status;
  }
  ApplyVolume(index, status);
}
}  // namespace DolphinUWP::GBAScreens

#else

#include "Core/Host.h"

namespace DolphinUWP::GBAScreens
{
std::unique_ptr<GBAHostInterface> CreateHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

bool CopyScreen(size_t, u64*, u32*, u32*, std::vector<u32>*)
{
  return false;
}

std::optional<Status> GetStatus(size_t)
{
  return std::nullopt;
}

bool IsRunning(size_t)
{
  return false;
}

void Reset(size_t)
{
}
void SetConnected(size_t, bool)
{
}
void LoadROM(size_t, std::string)
{
}
void QueueEReaderCard(size_t, std::string)
{
}
void ImportState(size_t, std::string)
{
}
void ExportState(size_t, std::string)
{
}
void ImportSave(size_t, std::string)
{
}
void ExportSave(size_t, std::string)
{
}
void SetVolume(size_t, int)
{
}
void SetMuted(size_t, bool)
{
}
}  // namespace DolphinUWP::GBAScreens

#endif

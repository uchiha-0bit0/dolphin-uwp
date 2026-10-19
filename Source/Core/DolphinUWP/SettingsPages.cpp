// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/SettingsPages.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <winrt/Windows.ApplicationModel.h>

#include <UwpImGuiFrontend/Widgets.h>

#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Enums.h"
#include "AudioCommon/XAudio2Stream.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Common/UWP/Device.h"
#include "Common/Version.h"
#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Config/UISettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/DVD/AMMediaboard.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/PowerPC/PowerPC.h"
#include "DiscIO/Enums.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/SettingChoices.h"
#include "DolphinUWP/ShellWidgets.h"
#include "DolphinUWP/Translation.h"
#include "DolphinUWP/UWPSettings.h"
#include "VideoBackends/D3DCommon/D3DCommon.h"
#include "VideoCommon/PostProcessing.h"
#include "VideoCommon/VideoConfig.h"

namespace DolphinUWP
{
namespace
{
using UwpImGuiFrontend::ChoiceItem;
using UwpImGuiFrontend::SectionTitle;

constexpr std::array<Choice<SerialInterface::SIDevices>, 9> PORT_DEVICES{{
    {SerialInterface::SIDEVICE_NONE, "None"},
    {SerialInterface::SIDEVICE_GC_CONTROLLER, "Standard Controller"},
    {SerialInterface::SIDEVICE_GC_STEERING, "Steering Wheel"},
    {SerialInterface::SIDEVICE_DANCEMAT, "Dance Mat"},
    {SerialInterface::SIDEVICE_GC_TARUKONGA, "DK Bongos"},
    {SerialInterface::SIDEVICE_GC_GBA_EMULATED, "GBA (Integrated)"},
    {SerialInterface::SIDEVICE_GC_GBA, "GBA (TCP)"},
    {SerialInterface::SIDEVICE_GC_KEYBOARD, "Keyboard Controller"},
    {SerialInterface::SIDEVICE_AM_BASEBOARD, "Triforce Baseboard"},
}};

constexpr std::array<Choice<ExpansionInterface::EXIDeviceType>, 7> SLOT_DEVICES{{
    {ExpansionInterface::EXIDeviceType::None, "None"},
    {ExpansionInterface::EXIDeviceType::Dummy, "Dummy"},
    {ExpansionInterface::EXIDeviceType::MemoryCard, "Memory Card"},
    {ExpansionInterface::EXIDeviceType::MemoryCardFolder, "GCI Folder"},
    {ExpansionInterface::EXIDeviceType::Gecko, "USB Gecko"},
    {ExpansionInterface::EXIDeviceType::AGP, "Advance Game Port"},
    {ExpansionInterface::EXIDeviceType::Microphone, "Microphone"},
}};

constexpr std::array<Choice<ExpansionInterface::EXIDeviceType>, 8> SERIAL_PORT_DEVICES{{
    {ExpansionInterface::EXIDeviceType::None, "None"},
    {ExpansionInterface::EXIDeviceType::Dummy, "Dummy"},
    {ExpansionInterface::EXIDeviceType::EthernetBuiltIn, "Broadband Adapter (HLE)"},
    {ExpansionInterface::EXIDeviceType::Ethernet, "Broadband Adapter (TAP)"},
    {ExpansionInterface::EXIDeviceType::EthernetXLink, "Broadband Adapter (XLink Kai)"},
    {ExpansionInterface::EXIDeviceType::EthernetTapServer, "Broadband Adapter (tapserver)"},
    {ExpansionInterface::EXIDeviceType::ModemTapServer, "Modem Adapter (tapserver)"},
    {ExpansionInterface::EXIDeviceType::Baseboard, "AM Baseboard"},
}};

constexpr std::array<Choice<FreeLook::ControlType>, 3> FREE_LOOK_CONTROL_TYPES{{
    {FreeLook::ControlType::SixAxis, "Six Axis"},
    {FreeLook::ControlType::FPS, "First Person"},
    {FreeLook::ControlType::Orbital, "Orbital"},
}};

constexpr std::array<Choice<int>, 6> GAMECUBE_LANGUAGES{{
    {0, "English"},
    {1, "German"},
    {2, "French"},
    {3, "Spanish"},
    {4, "Italian"},
    {5, "Dutch"},
}};

constexpr std::array<Choice<u32>, 10> WII_LANGUAGES{{
    {0, "Japanese"},
    {1, "English"},
    {2, "German"},
    {3, "French"},
    {4, "Spanish"},
    {5, "Italian"},
    {6, "Dutch"},
    {7, "Simplified Chinese"},
    {8, "Traditional Chinese"},
    {9, "Korean"},
}};

constexpr std::array<Choice<u32>, 3> WII_SOUND_MODES{{
    {0, "Mono"},
    {1, "Stereo"},
    {2, "Surround"},
}};

constexpr std::array<Choice<DiscIO::Region>, 4> FALLBACK_REGIONS{{
    {DiscIO::Region::NTSC_J, "NTSC-J"},
    {DiscIO::Region::NTSC_U, "NTSC-U"},
    {DiscIO::Region::PAL, "PAL"},
    {DiscIO::Region::NTSC_K, "NTSC-K"},
}};

constexpr std::array<Choice<Config::ShowCursor>, 3> CURSOR_VISIBILITY{{
    {Config::ShowCursor::OnMovement, "While it moves"},
    {Config::ShowCursor::Never, "Never"},
    {Config::ShowCursor::Constantly, "Always"},
}};

constexpr std::array<Choice<FrameDumpResolutionType>, 3> FRAME_DUMP_RESOLUTIONS{{
    {FrameDumpResolutionType::WindowResolution, "Window resolution"},
    {FrameDumpResolutionType::XFBAspectRatioCorrectedResolution,
     "Aspect ratio corrected internal resolution"},
    {FrameDumpResolutionType::XFBRawResolution, "Raw internal resolution"},
}};

constexpr std::array<Choice<bool>, 2> NETWORK_DUMP_FORMATS{{
    {true, "PCAP"},
    {false, "Raw"},
}};

constexpr std::array<Choice<WiimoteSource>, 2> WIIMOTE_SOURCES{{
    {WiimoteSource::None, "None"},
    {WiimoteSource::Emulated, "Emulated Wii Remote"},
}};

constexpr std::array<Choice<u32>, 2> SENSOR_BAR_POSITIONS{{
    {0, "Bottom"},
    {1, "Top"},
}};

constexpr std::array<Choice<PowerPC::CPUCore>, 3> CPU_CORES{{
    {PowerPC::CPUCore::JIT64, "JIT Recompiler (recommended)"},
    {PowerPC::CPUCore::CachedInterpreter, "Cached Interpreter (slower)"},
    {PowerPC::CPUCore::Interpreter, "Interpreter (slowest)"},
}};

constexpr std::array<Choice<const char*>, 2> VIDEO_BACKENDS{{
    {"D3D", "Direct3D 11"},
    {"D3D12", "Direct3D 12"},
}};

constexpr std::array<Choice<u32>, 4> MSAA_MODES{{
    {1, "None"},
    {2, "2x"},
    {4, "4x"},
    {8, "8x"},
}};

constexpr std::array<Choice<AnisotropicFilteringMode>, 6> ANISOTROPY_MODES{{
    {AnisotropicFilteringMode::Default, "Default"},
    {AnisotropicFilteringMode::Force1x, "1x"},
    {AnisotropicFilteringMode::Force2x, "2x"},
    {AnisotropicFilteringMode::Force4x, "4x"},
    {AnisotropicFilteringMode::Force8x, "8x"},
    {AnisotropicFilteringMode::Force16x, "16x"},
}};

constexpr std::array<Choice<TextureFilteringMode>, 3> TEXTURE_FILTERING_MODES{{
    {TextureFilteringMode::Default, "Default"},
    {TextureFilteringMode::Nearest, "Force Nearest"},
    {TextureFilteringMode::Linear, "Force Linear"},
}};

constexpr std::array<Choice<OutputResamplingMode>, 7> RESAMPLING_MODES{{
    {OutputResamplingMode::Default, "Default"},
    {OutputResamplingMode::Bilinear, "Bilinear"},
    {OutputResamplingMode::BSpline, "B-Spline"},
    {OutputResamplingMode::MitchellNetravali, "Mitchell-Netravali"},
    {OutputResamplingMode::CatmullRom, "Catmull-Rom"},
    {OutputResamplingMode::SharpBilinear, "Sharp Bilinear"},
    {OutputResamplingMode::AreaSampling, "Area Sampling"},
}};

constexpr std::array<Choice<StereoMode>, 5> STEREO_MODES{{
    {StereoMode::Off, "Off"},
    {StereoMode::SideBySide, "Side-by-Side"},
    {StereoMode::TopAndBottom, "Top-and-Bottom"},
    {StereoMode::Anaglyph, "Anaglyph"},
    {StereoMode::Passive, "Passive"},
}};

constexpr std::array<Choice<ColorCorrectionRegion>, 3> GAME_COLOR_SPACES{{
    {ColorCorrectionRegion::SMPTE_NTSCM, "NTSC-M (SMPTE 170M)"},
    {ColorCorrectionRegion::SYSTEMJ_NTSCJ, "NTSC-J (ARIB TR-B9)"},
    {ColorCorrectionRegion::EBU_PAL, "PAL (EBU)"},
}};

constexpr std::array<Choice<AudioCommon::DPL2Quality>, 4> DPL2_QUALITIES{{
    {AudioCommon::DPL2Quality::Lowest, "Lowest (latency about 10 ms)"},
    {AudioCommon::DPL2Quality::Low, "Low (latency about 20 ms)"},
    {AudioCommon::DPL2Quality::High, "High (latency about 40 ms)"},
    {AudioCommon::DPL2Quality::Highest, "Highest (latency about 80 ms)"},
}};

constexpr std::array<Choice<Config::GBALayout>, 6> GBA_LAYOUTS{{
    {Config::GBALayout::Bottom, "Along the bottom"},
    {Config::GBALayout::Top, "Along the top"},
    {Config::GBALayout::Left, "Down the left side"},
    {Config::GBALayout::Right, "Down the right side"},
    {Config::GBALayout::Corners, "In the corners"},
    {Config::GBALayout::Hidden, "Hidden"},
}};

constexpr std::array<Choice<int>, 5> GBA_SCALES{{
    {0, "Automatic"},
    {1, "1x"},
    {2, "2x"},
    {3, "3x"},
    {4, "4x"},
}};

constexpr u64 MIB = 1024 * 1024;
constexpr std::array<Choice<u64>, 11> SD_CARD_SIZES{{
    {0, "Automatic"},
    {64 * MIB, "64 MiB"},
    {128 * MIB, "128 MiB"},
    {256 * MIB, "256 MiB"},
    {512 * MIB, "512 MiB"},
    {1024 * MIB, "1 GiB"},
    {2048 * MIB, "2 GiB"},
    {4096 * MIB, "4 GiB (SDHC)"},
    {8192 * MIB, "8 GiB (SDHC)"},
    {16384 * MIB, "16 GiB (SDHC)"},
    {32768 * MIB, "32 GiB (SDHC)"},
}};

bool Toggle(const char* label, const Config::Info<bool>& info, bool invert = false)
{
  bool value = Config::Get(info) != invert;
  if (!UwpImGuiFrontend::ShellCheckbox(label, &value))
    return false;
  Config::SetBaseOrCurrent(info, value != invert);
  return true;
}

template <typename T>
bool Select(const char* label, const Config::Info<T>& info, std::span<const Choice<T>> choices)
{
  const T current = Config::Get(info);
  size_t selected = 0;
  std::vector<ChoiceItem> items;
  items.reserve(choices.size());
  for (size_t i = 0; i < choices.size(); ++i)
  {
    if (choices[i].value == current)
      selected = i;
    items.push_back({std::to_string(i), choices[i].label});
  }
  if (!UwpImGuiFrontend::ShellCombo(label, &selected, items))
    return false;
  Config::SetBaseOrCurrent(info, choices[selected].value);
  return true;
}

template <typename T, size_t N>
bool Select(const char* label, const Config::Info<T>& info, const std::array<Choice<T>, N>& choices)
{
  return Select(label, info, std::span<const Choice<T>>(choices));
}

bool IntegerSlider(const char* label, const Config::Info<int>& info, int minimum, int maximum,
                   const char* format = "%d")
{
  int value = Config::Get(info);
  if (!UwpImGuiFrontend::ShellSliderInt(label, &value, minimum, maximum, format))
    return false;
  Config::SetBaseOrCurrent(info, value);
  return true;
}

template <typename T>
bool UnsignedSlider(const char* label, const Config::Info<T>& info, int minimum, int maximum)
{
  int value = static_cast<int>(Config::Get(info));
  if (!UwpImGuiFrontend::ShellSliderInt(label, &value, minimum, maximum))
    return false;
  Config::SetBaseOrCurrent(info, static_cast<T>(value));
  return true;
}

bool FloatSlider(const char* label, const Config::Info<float>& info, float minimum, float maximum,
                 const char* format = "%.2f")
{
  float value = Config::Get(info);
  if (!UwpImGuiFrontend::ShellSliderFloat(label, &value, minimum, maximum, format))
    return false;
  Config::SetBaseOrCurrent(info, value);
  return true;
}

bool Percentage(const char* label, const Config::Info<float>& info, int minimum, int maximum,
                bool zero_is_unlimited = false)
{
  int value = static_cast<int>(std::lround(Config::Get(info) * 100.0f));
  const char* format = zero_is_unlimited && value == 0 ? "Unlimited" : "%d%%";
  if (!UwpImGuiFrontend::ShellSliderInt(label, &value, minimum, maximum, format))
    return false;
  Config::SetBaseOrCurrent(info, static_cast<float>(value) / 100.0f);
  return true;
}

bool Megabytes(const char* label, const Config::Info<u32>& info, int minimum, int maximum)
{
  constexpr u32 MEGABYTE = 0x100000;
  int value = static_cast<int>(Config::Get(info) / MEGABYTE);
  if (!UwpImGuiFrontend::ShellSliderInt(label, &value, minimum, maximum, "%d MB"))
    return false;
  Config::SetBaseOrCurrent(info, static_cast<u32>(value) * MEGABYTE);
  return true;
}

std::string FileName(const std::string& path)
{
  return path.empty() ? std::string("None") : PathToString(StringToPath(path).filename());
}
}  // namespace

SettingsPages::SettingsPages()
{
  m_login_hook = AchievementManager::GetInstance().login_event.Register(
      [this](int result) { m_login_result = result; });
}

SettingsPages::~SettingsPages() = default;

std::vector<UwpImGuiFrontend::SettingsExtensionPage> SettingsPages::BuildSettingsPages()
{
  return {
      {"dolphin-general", "General", true, [this] { DrawGeneral(); }},
      {"dolphin-osd", "On-Screen Display", true, [this] { DrawOnScreenDisplay(); }},
      {"dolphin-gamecube", "GameCube", true, [this] { DrawGameCube(); }},
      {"dolphin-wii", "Wii", true, [this] { DrawWii(); }},
      {"dolphin-audio", "Audio", true, [this] { DrawAudio(); }},
      {"dolphin-achievements", "Achievements", true, [this] { DrawAchievements(); }},
      {"dolphin-paths", "Paths", true, [this] { DrawPaths(); }},
      {"dolphin-advanced", "Advanced", true, [this] { DrawAdvanced(); }},
      {"dolphin-about", "About", true, [this] { DrawAbout(); }},
  };
}

UwpImGuiFrontend::GraphicsViewModel SettingsPages::BuildGraphicsModel()
{
  UwpImGuiFrontend::GraphicsViewModel model;
  model.pages = {
      {"general", "General", true, [this] { DrawGraphicsGeneral(); }},
      {"enhancements", "Enhancements", true, [this] { DrawEnhancements(); }},
      {"stereoscopy", "Stereoscopy", true, [this] { DrawStereoscopy(); }},
      {"hacks", "Hacks", true, [this] { DrawHacks(); }},
      {"advanced", "Advanced", true, [this] { DrawGraphicsAdvanced(); }},
  };
  return model;
}

void SettingsPages::Flush()
{
  m_text_buffers.clear();
  Save();
}

void SettingsPages::Update()
{
  constexpr auto SAVE_DELAY = std::chrono::seconds(1);
  const bool pending = m_dirty || (m_post_processing && m_post_processing->IsDirty());
  if (!pending)
  {
    m_dirty_since.reset();
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!m_dirty_since)
    m_dirty_since = now;
  else if (now - *m_dirty_since >= SAVE_DELAY)
    Save();
}

void SettingsPages::Save()
{
  m_dirty_since.reset();
  if (m_post_processing && m_post_processing->IsDirty())
  {
    m_post_processing->SaveOptionsConfiguration();
    m_post_processing->SetDirty(false);
  }
  if (!m_dirty)
    return;
  m_dirty = false;
  Config::Save();
}

bool SettingsPages::TextSetting(const char* label, const Config::Info<std::string>& info,
                                UwpImGuiFrontend::TextInputScope scope)
{
  auto [iter, inserted] = m_text_buffers.try_emplace(label);
  auto& buffer = iter->second;
  if (inserted)
  {
    const std::string value = Config::Get(info);
    buffer.fill('\0');
    value.copy(buffer.data(), buffer.size() - 1);
  }

  if (!UwpImGuiFrontend::NativeInputText(label, buffer.data(), buffer.size(), 0, scope))
    return false;
  Config::SetBaseOrCurrent(info, std::string(buffer.data()));
  return true;
}

bool SettingsPages::ActionButton(const char* label, std::string_view action)
{
  if (!UwpImGuiFrontend::ShellButton(label))
    return false;
  if (m_action)
    m_action(action);
  return true;
}

void SettingsPages::DrawAdapterSettings()
{
  using ExpansionInterface::EXIDeviceType;
  switch (Config::Get(Config::MAIN_SERIAL_PORT_1))
  {
  case EXIDeviceType::EthernetBuiltIn:
    m_dirty |= TextSetting("DNS server", Config::MAIN_BBA_BUILTIN_DNS,
                           UwpImGuiFrontend::TextInputScope::Url);
    ImGui::TextDisabled("Use 8.8.8.8 for normal DNS, or enter the address of a custom server.");
    break;
  case EXIDeviceType::Ethernet:
    m_dirty |= TextSetting("MAC address", Config::MAIN_BBA_MAC);
    ImGui::TextDisabled("Enter an address like aa:bb:cc:dd:ee:ff, or leave it empty to generate "
                        "one.");
    break;
  case EXIDeviceType::EthernetXLink:
    m_dirty |= TextSetting("XLink Kai client address", Config::MAIN_BBA_XLINK_IP,
                           UwpImGuiFrontend::TextInputScope::Url);
    m_dirty |= Toggle("Show XLink Kai chat messages", Config::MAIN_BBA_XLINK_CHAT_OSD);
    break;
  case EXIDeviceType::EthernetTapServer:
    m_dirty |= TextSetting("tapserver destination", Config::MAIN_BBA_TAPSERVER_DESTINATION,
                           UwpImGuiFrontend::TextInputScope::Url);
    ImGui::TextDisabled("Enter the address:port of the tapserver instance to connect to.");
    break;
  case EXIDeviceType::ModemTapServer:
    m_dirty |= TextSetting("tapserver destination", Config::MAIN_MODEM_TAPSERVER_DESTINATION,
                           UwpImGuiFrontend::TextInputScope::Url);
    ImGui::TextDisabled("Enter the address:port of the tapserver instance to connect to.");
    break;
  default:
    break;
  }
}

void SettingsPages::DrawGeneral()
{
  SectionTitle("Emulation");
  m_dirty |= Toggle("Enable dual core", Config::MAIN_CPU_THREAD);
  m_dirty |= Percentage("Emulation speed", Config::MAIN_EMULATION_SPEED, 0, 200, true);
  m_dirty |= Toggle("Enable cheats", Config::MAIN_ENABLE_CHEATS);
  m_dirty |= Toggle("Speed up disc transfer rate", Config::MAIN_FAST_DISC_SPEED);
  m_dirty |= Toggle("Load the whole game into memory", Config::MAIN_LOAD_GAME_INTO_MEMORY);
  m_dirty |= Toggle("Change discs automatically", Config::MAIN_AUTO_DISC_CHANGE);
  m_dirty |= Toggle("Allow mismatched region settings", Config::MAIN_OVERRIDE_REGION_SETTINGS);
  m_dirty |= Select("Fallback region", Config::MAIN_FALLBACK_REGION, FALLBACK_REGIONS);
  ShellWidgets::HintText("The fallback region is used for titles whose region can't be "
                         "determined.");

  SectionTitle("Interface");
  DrawLanguage();
  m_dirty |= Toggle("Pause when Dolphin is in the background", Config::MAIN_PAUSE_ON_FOCUS_LOST);
  m_dirty |= Toggle("Confirm before stopping a game", Config::MAIN_CONFIRM_ON_STOP);
  m_dirty |= Toggle("Use panic handlers", Config::MAIN_USE_PANIC_HANDLERS);
  m_dirty |= Toggle("Enable play time tracking", Config::MAIN_TIME_TRACKING);
  if (Toggle("Use the built-in database of game names", Config::MAIN_USE_BUILT_IN_TITLE_DATABASE))
  {
    m_dirty = true;
    if (m_action)
      m_action("library-rebuild");
  }

  if (!Common::UWP::IsXboxConsole())
  {
    SectionTitle("Window");
    m_dirty |= Toggle("Show the game's title in the window title", Config::MAIN_SHOW_ACTIVE_TITLE);
    m_dirty |= Toggle("Keep the game window on top", Config::MAIN_KEEP_WINDOW_ON_TOP);
    ShellWidgets::HintText("A game whose window is kept on top plays in a compact window that "
                           "stays above other windows.");

    SectionTitle("Mouse");
    m_dirty |= Toggle("Lock the mouse cursor to the game", Config::MAIN_LOCK_CURSOR);
    ShellWidgets::HintText("The Unlock Cursor hotkey or the quick menu releases the cursor. Click "
                           "the game to lock it again.");
    m_dirty |= Select("Show the cursor over the game", Config::MAIN_SHOW_CURSOR, CURSOR_VISIBILITY);
  }

  SectionTitle("Hotkeys");
  m_dirty |= Toggle("Hotkeys require window focus", Config::MAIN_FOCUSED_HOTKEYS);
  ActionButton("Configure hotkeys", "hotkeys");
}

void SettingsPages::DrawLanguage()
{
  const std::string current = Config::Get(Config::MAIN_INTERFACE_LANGUAGE);
  std::vector<ChoiceItem> items{{"", "System language"}};
  size_t selected = 0;
  for (const auto& [code, name] : Translation::GetLanguages())
  {
    if (code == current)
      selected = items.size();
    items.push_back({code, name});
  }
  if (!UwpImGuiFrontend::ShellCombo("Language", &selected, items))
    return;
  Config::SetBaseOrCurrent(Config::MAIN_INTERFACE_LANGUAGE, items[selected].id);
  Translation::Apply();
  m_dirty = true;
}

void SettingsPages::DrawOnScreenDisplay()
{
  SectionTitle("Messages");
  m_dirty |= Toggle("Show on-screen messages", Config::MAIN_OSD_MESSAGES);
  m_dirty |= IntegerSlider("Font size", Config::MAIN_OSD_FONT_SIZE, 12, 40);

  SectionTitle("Performance statistics");
  m_dirty |= Toggle("Show FPS", Config::GFX_SHOW_FPS);
  m_dirty |= Toggle("Show frame times", Config::GFX_SHOW_FTIMES);
  m_dirty |= Toggle("Show VPS", Config::GFX_SHOW_VPS);
  m_dirty |= Toggle("Show VBlank times", Config::GFX_SHOW_VTIMES);
  m_dirty |= Toggle("Show speed", Config::GFX_SHOW_SPEED);
  m_dirty |= Toggle("Show speed colors", Config::GFX_SHOW_SPEED_COLORS);
  m_dirty |= Toggle("Show performance graphs", Config::GFX_SHOW_GRAPHS);
  int window = Config::Get(Config::GFX_PERF_SAMP_WINDOW);
  if (UwpImGuiFrontend::ShellIntegerStepper("Performance sample window", &window, 0, 10000, 100,
                                            "%d ms"))
  {
    Config::SetBaseOrCurrent(Config::GFX_PERF_SAMP_WINDOW, window);
    m_dirty = true;
  }

  SectionTitle("NetPlay");
  m_dirty |= Toggle("Show NetPlay ping", Config::GFX_SHOW_NETPLAY_PING);
  m_dirty |= Toggle("Show NetPlay chat", Config::GFX_SHOW_NETPLAY_MESSAGES);

  SectionTitle("Debugging");
  m_dirty |= Toggle("Show statistics", Config::GFX_OVERLAY_STATS);
  m_dirty |= Toggle("Show projection statistics", Config::GFX_OVERLAY_PROJ_STATS);
  m_dirty |= Toggle("Show XFB resolution", Config::GFX_SHOW_INTERNAL_RESOLUTION);
}

void SettingsPages::DrawGameCube()
{
  SectionTitle("Controller ports");
  bool tcp_gba = false;
  for (int port = 0; port < 4; ++port)
  {
    m_dirty |= Select(fmt::format("Port {}", port + 1).c_str(), Config::GetInfoForSIDevice(port),
                      PORT_DEVICES);
    tcp_gba |= Config::Get(Config::GetInfoForSIDevice(port)) == SerialInterface::SIDEVICE_GC_GBA;
  }
  ShellWidgets::HintText("Map a Keyboard Controller with the GameCube Keyboard controller type, "
                         "and a Triforce Baseboard with the GameCube Controller type.");
  if (tcp_gba)
  {
    ShellWidgets::HintText("A GBA (TCP) port waits for a GBA emulator on another device, such as "
                           "mGBA, to connect to this device.");
  }

  if (m_draw_storage)
  {
    SectionTitle("Memory cards");
    m_draw_storage("gc");
    ShellWidgets::HintText("Memory cards, GCI folders and the IPL are kept here. Files already in "
                           "the old location are not moved.");
  }

  SectionTitle("Game Boy Advance");
  const bool bios = File::Exists(File::GetUserPath(F_GBABIOS_IDX));
  if (ShellWidgets::ValueRow("gba-bios", "BIOS", bios ? "Installed" : "Not installed") && m_action)
    m_action("gba-bios");
  for (size_t port = 0; port < Config::MAIN_GBA_ROM_PATHS.size(); ++port)
  {
    const std::string action = fmt::format("gba-rom-{}", port);
    const std::string label = port == Config::GBPLAYER_GBA_INDEX ?
                                  std::string("Game Boy Player ROM") :
                                  fmt::format("GBA {} ROM", port + 1);
    if (ShellWidgets::ValueRow(action.c_str(), label,
                               FileName(Config::Get(Config::MAIN_GBA_ROM_PATHS[port]))) &&
        m_action)
    {
      m_action(action);
    }
  }
  ActionButton("Remove the GBA ROMs", "gba-roms-clear");
  m_dirty |= Toggle("Save in the same folder as the ROM", Config::MAIN_GBA_SAVES_IN_ROM_PATH);
  if (!Config::Get(Config::MAIN_GBA_SAVES_IN_ROM_PATH) && m_draw_storage)
  {
    m_draw_storage("gba-saves");
    ShellWidgets::HintText("Saves already in the old location are not moved.");
  }
  ShellWidgets::HintText("Set a controller port to GBA (Integrated) to connect a Game Boy Advance. "
                         "Games that send their own program over the link cable only need the "
                         "BIOS.");
  m_dirty |= Select("Screen position", Config::UWP_GBA_LAYOUT, GBA_LAYOUTS);
  m_dirty |= Select("Screen size", Config::UWP_GBA_SCALE, GBA_SCALES);
  m_dirty |= Toggle("Frame around the screens", Config::UWP_GBA_FRAME);
  m_dirty |= Toggle("Interframe blending", Config::UWP_GBA_INTERFRAME_BLENDING);
  m_dirty |= Toggle("Show screens that phones show", Config::UWP_GBA_SHOW_STREAMED);
  ShellWidgets::HintText("A phone running RetroDSU shows the GBA of its player on its secondary "
                         "screen, with that GBA's sound. The player is the one its controller is "
                         "set to in RetroDSU. Every phone can show the game screen.");

  SectionTitle("Memory cards");
  m_dirty |= Select("Slot A", Config::MAIN_SLOT_A, SLOT_DEVICES);
  m_dirty |= Select("Slot B", Config::MAIN_SLOT_B, SLOT_DEVICES);
  for (const ExpansionInterface::Slot slot :
       {ExpansionInterface::Slot::A, ExpansionInterface::Slot::B})
  {
    if (Config::Get(Config::GetInfoForEXIDevice(slot)) != ExpansionInterface::EXIDeviceType::AGP)
      continue;
    const bool a = slot == ExpansionInterface::Slot::A;
    if (ShellWidgets::ValueRow(a ? "agp-rom-a" : "agp-rom-b",
                               a ? "Slot A GBA cartridge" : "Slot B GBA cartridge",
                               FileName(Config::Get(Config::GetInfoForAGPCartPath(slot)))) &&
        m_action)
    {
      m_action(a ? "agp-rom-a" : "agp-rom-b");
    }
  }
  ActionButton("Manage memory cards", "memory-cards");

  SectionTitle("Network adapter");
  using ExpansionInterface::EXIDeviceType;
  std::vector<Choice<EXIDeviceType>> serial_devices(SERIAL_PORT_DEVICES.begin(),
                                                    SERIAL_PORT_DEVICES.end());
  // An Xbox can't install the driver of a TAP adapter.
  if (Common::UWP::IsXboxConsole() &&
      Config::Get(Config::MAIN_SERIAL_PORT_1) != EXIDeviceType::Ethernet)
  {
    std::erase_if(serial_devices, [](const Choice<EXIDeviceType>& device) {
      return device.value == EXIDeviceType::Ethernet;
    });
  }
  m_dirty |= Select("Serial port 1", Config::MAIN_SERIAL_PORT_1,
                    std::span<const Choice<EXIDeviceType>>(serial_devices));
  DrawAdapterSettings();

  SectionTitle("System");
  m_dirty |= Toggle("Skip main menu", Config::MAIN_SKIP_IPL);
  m_dirty |= Select("System language", Config::MAIN_GC_LANGUAGE, GAMECUBE_LANGUAGES);

  DrawTriforce();
}

void SettingsPages::DrawTriforce()
{
  SectionTitle("Triforce");
  ShellWidgets::HintText("Triforce games attach the baseboard on their own. Map Coin, Service and "
                         "Test in the GameCube controller mapping. Place the Triforce system menu "
                         "in the GC/DEV folder as IPL.bin to start it from Tools.");

  SectionTitle("IP address redirections");
  const std::string redirections = Config::Get(Config::MAIN_TRIFORCE_IP_REDIRECTIONS);
  std::vector<std::string> entries;
  for (const auto entry : redirections | std::views::split(','))
  {
    const std::string_view text(entry.begin(), entry.end());
    if (!text.empty())
      entries.emplace_back(text);
  }

  std::optional<size_t> removed;
  for (size_t i = 0; i < entries.size(); ++i)
  {
    const auto parsed = AMMediaboard::ParseIPRedirection(entries[i]);
    if (!parsed)
      continue;
    ImGui::PushID(static_cast<int>(i));
    if (ShellWidgets::ValueRow(
            "redirection", fmt::format("{} to {}", parsed->original, parsed->replacement),
            parsed->description.empty() ? std::string_view("Remove") : parsed->description))
    {
      removed = i;
    }
    ImGui::PopID();
  }
  if (entries.empty())
    ShellWidgets::HintText("There are no redirections.");
  else
    ShellWidgets::HintText("Select a redirection to remove it.");
  if (removed)
  {
    entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*removed));
    Config::SetBaseOrCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS,
                             fmt::format("{}", fmt::join(entries, ",")));
    m_dirty = true;
  }

  const auto buffer = [this](const char* key) -> std::array<char, 256>& {
    auto [iter, inserted] = m_text_buffers.try_emplace(key);
    if (inserted)
      iter->second.fill('\0');
    return iter->second;
  };
  auto& original = buffer("triforce-original");
  auto& replacement = buffer("triforce-replacement");
  auto& description = buffer("triforce-description");
  (void)UwpImGuiFrontend::NativeInputText("Emulated address", original.data(), original.size(), 0,
                                          UwpImGuiFrontend::TextInputScope::Url);
  (void)UwpImGuiFrontend::NativeInputText("Real address", replacement.data(), replacement.size(), 0,
                                          UwpImGuiFrontend::TextInputScope::Url);
  (void)UwpImGuiFrontend::NativeInputText("Description", description.data(), description.size());
  if (UwpImGuiFrontend::ShellButton("Add the redirection"))
  {
    std::string entry =
        fmt::format("{}={}", StripWhitespace(original.data()), StripWhitespace(replacement.data()));
    if (const std::string_view text = StripWhitespace(description.data()); !text.empty())
      entry += fmt::format(" {}", text);
    if (AMMediaboard::ParseIPRedirection(entry))
    {
      entries.push_back(std::move(entry));
      Config::SetBaseOrCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS,
                               fmt::format("{}", fmt::join(entries, ",")));
      original.fill('\0');
      replacement.fill('\0');
      description.fill('\0');
      m_triforce_error.clear();
      m_dirty = true;
    }
    else
    {
      m_triforce_error = "Enter addresses like 192.168.29.150:5000-5008 or 0.0.0.0/0.";
    }
  }
  if (!m_triforce_error.empty())
    ShellWidgets::HintText(m_triforce_error);
  if (UwpImGuiFrontend::ShellButton("Restore the default redirections"))
  {
    Config::SetBaseOrCurrent(Config::MAIN_TRIFORCE_IP_REDIRECTIONS, Config::DefaultState{});
    m_dirty = true;
  }
}

void SettingsPages::DrawWii()
{
  SectionTitle("Wii Remotes");
  for (int index = 0; index < 4; ++index)
  {
    m_dirty |= Select(fmt::format("Wii Remote {}", index + 1).c_str(),
                      Config::GetInfoForWiimoteSource(index), WIIMOTE_SOURCES);
  }
  m_dirty |=
      Select("Sensor bar position", Config::SYSCONF_SENSOR_BAR_POSITION, SENSOR_BAR_POSITIONS);
  m_dirty |= UnsignedSlider("IR sensitivity", Config::SYSCONF_SENSOR_BAR_SENSITIVITY, 1, 5);
  m_dirty |= UnsignedSlider("Speaker volume", Config::SYSCONF_SPEAKER_VOLUME, 0, 127);
  m_dirty |= Toggle("Enable rumble", Config::SYSCONF_WIIMOTE_MOTOR);
  m_dirty |= Toggle("Enable speaker data", Config::MAIN_WIIMOTE_ENABLE_SPEAKER);

  SectionTitle("System");
  m_dirty |= Toggle("Widescreen (16:9)", Config::SYSCONF_WIDESCREEN);
  m_dirty |= Toggle("Progressive scan", Config::SYSCONF_PROGRESSIVE_SCAN);
  m_dirty |= Toggle("PAL60 mode", Config::SYSCONF_PAL60);
  m_dirty |= Toggle("Screen saver", Config::SYSCONF_SCREENSAVER);
  m_dirty |= Select("Sound", Config::SYSCONF_SOUND_MODE, WII_SOUND_MODES);
  m_dirty |= Select("System language", Config::SYSCONF_LANGUAGE, WII_LANGUAGES);

  SectionTitle("Online");
  m_dirty |= Toggle("Enable WiiConnect24 via WiiLink", Config::MAIN_WII_WIILINK_ENABLE);

  SectionTitle("Devices");
  m_dirty |= Toggle("Connect USB keyboard", Config::MAIN_WII_KEYBOARD);
  m_dirty |= Toggle("Connect Wii Speak", Config::MAIN_EMULATE_WII_SPEAK);
  int logitech_mics = static_cast<int>(std::ranges::count_if(
      Config::MAIN_EMULATE_LOGITECH_MIC, [](const auto& info) { return Config::Get(info); }));
  if (UwpImGuiFrontend::ShellIntegerStepper("Logitech USB microphones", &logitech_mics, 0,
                                            static_cast<int>(Config::EMULATED_LOGITECH_MIC_COUNT)))
  {
    for (size_t i = 0; i < Config::EMULATED_LOGITECH_MIC_COUNT; ++i)
    {
      Config::SetBaseOrCurrent(Config::MAIN_EMULATE_LOGITECH_MIC[i],
                               static_cast<int>(i) < logitech_mics);
    }
    m_dirty = true;
  }
  ShellWidgets::HintText("Wii Speak and the microphones use the default microphone. Windows asks "
                         "for permission the first time a game uses it.");

  SectionTitle("SD card");
  m_dirty |= Toggle("Insert SD card", Config::MAIN_WII_SD_CARD);
  m_dirty |= Toggle("Allow writes to SD card", Config::MAIN_ALLOW_SD_WRITES);
  m_dirty |= Toggle("Automatically sync with folder", Config::MAIN_WII_SD_CARD_ENABLE_FOLDER_SYNC);
  m_dirty |= Select("SD card file size", Config::MAIN_WII_SD_CARD_FILESIZE, SD_CARD_SIZES);
  if (m_draw_storage)
  {
    m_draw_storage("sd-image");
    m_draw_storage("sd-folder");
    ShellWidgets::HintText("Files already in the old location are not moved.");
  }
  ActionButton("Pack the SD card now", "sd-pack");
  ActionButton("Unpack the SD card now", "sd-unpack");
}

void SettingsPages::DrawAudio()
{
  SectionTitle("Output");
  int volume = Config::Get(Config::MAIN_AUDIO_VOLUME);
  if (UwpImGuiFrontend::ShellSliderInt("Volume", &volume, 0, 100, "%d%%"))
  {
    Config::SetBaseOrCurrent(Config::MAIN_AUDIO_VOLUME, volume);
    m_dirty = true;
  }
  m_dirty |= Toggle("Mute", Config::MAIN_AUDIO_MUTED);
  int buffer = Config::Get(Config::MAIN_AUDIO_BUFFER_SIZE);
  if (UwpImGuiFrontend::ShellSliderInt("Buffer size", &buffer, 16, 512, "%d ms"))
  {
    Config::SetBaseOrCurrent(Config::MAIN_AUDIO_BUFFER_SIZE, buffer);
    m_dirty = true;
  }
  m_dirty |= Toggle("Fill audio gaps", Config::MAIN_AUDIO_FILL_GAPS);
  m_dirty |= Toggle("Preserve pitch when changing speed", Config::MAIN_AUDIO_PRESERVE_PITCH);
  m_dirty |= Toggle("Mute when the speed limit is disabled",
                    Config::MAIN_AUDIO_MUTE_ON_DISABLED_SPEED_LIMIT);

  SectionTitle("DSP emulation");
  const std::array<ChoiceItem, 3> engines{{
      {"hle", "HLE (recommended)"},
      {"lle-recompiler", "LLE Recompiler (slow)"},
      {"lle-interpreter", "LLE Interpreter (slowest)"},
  }};
  size_t engine =
      Config::Get(Config::MAIN_DSP_HLE) ? 0 : (Config::Get(Config::MAIN_DSP_JIT) ? 1 : 2);
  if (UwpImGuiFrontend::ShellCombo("DSP engine", &engine, engines))
  {
    Config::SetBaseOrCurrent(Config::MAIN_DSP_HLE, engine == 0);
    Config::SetBaseOrCurrent(Config::MAIN_DSP_JIT, engine == 1);
    m_dirty = true;
  }

  SectionTitle("Surround");
  if (Config::Get(Config::MAIN_DSP_HLE))
  {
    ShellWidgets::HintText("The Dolby Pro Logic II decoder needs an LLE DSP engine.");
  }
  else
  {
    m_dirty |= Toggle("Dolby Pro Logic II decoder", Config::MAIN_DPL2_DECODER);
    if (Config::Get(Config::MAIN_DPL2_DECODER))
      m_dirty |= Select("Decoding quality", Config::MAIN_DPL2_QUALITY, DPL2_QUALITIES);
  }

  DrawAudioRouting();
}

void SettingsPages::DrawAudioRouting()
{
  // Windows lists the output devices in the background when the page is first shown.
  if (!m_output_devices_requested)
  {
    m_output_devices_requested = true;
    m_output_device_list = std::async(std::launch::async, &XAudio2Stream::ListOutputDevices);
  }
  if (m_output_device_list.valid() &&
      m_output_device_list.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
  {
    m_output_devices = m_output_device_list.get();
  }

  SectionTitle("Wii Remote speakers");
  m_dirty |=
      Toggle("Play the speakers on other devices", Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED);
  if (Config::Get(Config::MAIN_WIIMOTE_AUDIO_ROUTING_ENABLED))
  {
    for (size_t i = 0; i < Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED.size(); ++i)
    {
      m_dirty |= Toggle(fmt::format("Wii Remote {}", i + 1).c_str(),
                        Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]);
      if (Config::Get(Config::MAIN_WIIMOTE_AUDIO_OUTPUT_ENABLED[i]))
      {
        m_dirty |= OutputDevice(fmt::format("Wii Remote {} device", i + 1).c_str(),
                                Config::MAIN_WIIMOTE_AUDIO_OUTPUT_DEVICE[i]);
      }
    }
    if (!Config::Get(Config::MAIN_WIIMOTE_ENABLE_SPEAKER))
      ShellWidgets::HintText("Turn on speaker data in the Wii settings to hear the speakers.");
  }
  ShellWidgets::HintText("A speaker that plays on another device, such as the headset of a "
                         "controller, isn't heard with the game. Changes apply when a game "
                         "starts.");

  SectionTitle("Game Boy Advance sound");
  m_dirty |= Toggle("Play the GBAs on other devices", Config::MAIN_GBA_AUDIO_ROUTING_ENABLED);
  if (Config::Get(Config::MAIN_GBA_AUDIO_ROUTING_ENABLED))
  {
    for (size_t i = 0; i < Config::MAIN_GBA_AUDIO_OUTPUT_ENABLED.size(); ++i)
    {
      m_dirty |=
          Toggle(fmt::format("GBA {}", i + 1).c_str(), Config::MAIN_GBA_AUDIO_OUTPUT_ENABLED[i]);
      if (Config::Get(Config::MAIN_GBA_AUDIO_OUTPUT_ENABLED[i]))
      {
        m_dirty |= OutputDevice(fmt::format("GBA {} device", i + 1).c_str(),
                                Config::MAIN_GBA_AUDIO_OUTPUT_DEVICE[i]);
      }
    }
  }
  ShellWidgets::HintText("A GBA that a phone shows plays on the phone instead. Changes apply when "
                         "a game starts.");
}

bool SettingsPages::OutputDevice(const char* label, const Config::Info<std::string>& info)
{
  const std::string current = Config::Get(info);
  std::vector<ChoiceItem> items{{"", "Default device"}};
  size_t selected = 0;
  for (const auto& [id, name] : m_output_devices)
  {
    if (id == current)
      selected = items.size();
    items.push_back({id, name});
  }
  if (!current.empty() && selected == 0)
  {
    selected = items.size();
    items.push_back(
        {current, m_output_device_list.valid() ? "Finding the device" : "Unavailable device"});
  }
  if (!UwpImGuiFrontend::ShellCombo(label, &selected, items))
    return false;
  Config::SetBaseOrCurrent(info, items[selected].id);
  return true;
}

void SettingsPages::DrawPaths()
{
  if (m_draw_storage)
  {
    m_draw_storage("games");
    SectionTitle("Folders");
    for (const std::string_view id : {"load", "resource-packs", "dump", "nand", "wfs", "cache"})
      m_draw_storage(id);
    ShellWidgets::HintText("Development files and USB storage keep each folder in a Dolphin "
                           "folder, such as D:\\DevelopmentFiles\\Dolphin\\Load. Files already "
                           "in the old location are not moved.");
    ActionButton("Use the default locations", "path-reset");
  }

  SectionTitle("Default disc");
  const std::string default_iso = Config::Get(Config::MAIN_DEFAULT_ISO);
  if (ShellWidgets::ValueRow("default-iso", "Disc", FileName(default_iso)) && m_action)
    m_action("default-iso");
  if (!default_iso.empty() && UwpImGuiFrontend::ShellButton("Remove the default disc"))
  {
    Config::SetBaseOrCurrent(Config::MAIN_DEFAULT_ISO, std::string());
    m_dirty = true;
  }
  ShellWidgets::HintText("The default disc is in the drive when the GameCube main menu or a "
                         "program file starts.");
}

void SettingsPages::DrawAdvanced()
{
  SectionTitle("CPU");
  m_dirty |= Select("CPU emulation engine", Config::MAIN_CPU_CORE, CPU_CORES);
  m_dirty |= Toggle("Enable MMU", Config::MAIN_MMU);
  m_dirty |= Toggle("Enable fastmem", Config::MAIN_FASTMEM);
  m_dirty |= Toggle("Enable write-back cache (slow)", Config::MAIN_ACCURATE_CPU_CACHE);
  m_dirty |= Toggle("Synchronize GPU thread", Config::MAIN_SYNC_GPU);
  m_dirty |= Toggle("Pause on panic", Config::MAIN_PAUSE_ON_PANIC);

  SectionTitle("Debugging");
  m_dirty |= Toggle("Enable debugging", Config::MAIN_ENABLE_DEBUGGING);
  if (Config::Get(Config::MAIN_ENABLE_DEBUGGING))
  {
    ShellWidgets::HintText("The in-game debugger opens from the quick menu. These switches turn "
                           "off parts of the JIT, as the JIT menu of DolphinQt's debugger does.");
    m_dirty |= Toggle("Disable page table fastmem", Config::MAIN_PAGE_TABLE_FASTMEM, true);
    m_dirty |= Toggle("Disable fastmem arena", Config::MAIN_FASTMEM_ARENA, true);
    m_dirty |= Toggle("Disable large entry points map", Config::MAIN_LARGE_ENTRY_POINTS_MAP, true);
    m_dirty |= Toggle("Enable JIT block profiling", Config::MAIN_DEBUG_JIT_ENABLE_PROFILING);
    m_dirty |= Toggle("JIT off (JIT core)", Config::MAIN_DEBUG_JIT_OFF);
    m_dirty |= Toggle("JIT LoadStore off", Config::MAIN_DEBUG_JIT_LOAD_STORE_OFF);
    m_dirty |= Toggle("JIT LoadStore lbzx off", Config::MAIN_DEBUG_JIT_LOAD_STORE_LBZX_OFF);
    m_dirty |= Toggle("JIT LoadStore lXz off", Config::MAIN_DEBUG_JIT_LOAD_STORE_LXZ_OFF);
    m_dirty |= Toggle("JIT LoadStore lwz off", Config::MAIN_DEBUG_JIT_LOAD_STORE_LWZ_OFF);
    m_dirty |= Toggle("JIT LoadStore Floating off", Config::MAIN_DEBUG_JIT_LOAD_STORE_FLOATING_OFF);
    m_dirty |= Toggle("JIT LoadStore Paired off", Config::MAIN_DEBUG_JIT_LOAD_STORE_PAIRED_OFF);
    m_dirty |= Toggle("JIT FloatingPoint off", Config::MAIN_DEBUG_JIT_FLOATING_POINT_OFF);
    m_dirty |= Toggle("JIT Integer off", Config::MAIN_DEBUG_JIT_INTEGER_OFF);
    m_dirty |= Toggle("JIT Paired off", Config::MAIN_DEBUG_JIT_PAIRED_OFF);
    m_dirty |= Toggle("JIT SystemRegisters off", Config::MAIN_DEBUG_JIT_SYSTEM_REGISTERS_OFF);
    m_dirty |= Toggle("JIT Branch off", Config::MAIN_DEBUG_JIT_BRANCH_OFF);
    m_dirty |= Toggle("JIT Register Cache off", Config::MAIN_DEBUG_JIT_REGISTER_CACHE_OFF);
  }

  SectionTitle("Clock override");
  m_dirty |= Toggle("Override emulated CPU clock", Config::MAIN_OVERCLOCK_ENABLE);
  m_dirty |= Percentage("Emulated CPU clock", Config::MAIN_OVERCLOCK, 10, 400);
  m_dirty |= Toggle("Override VBI frequency", Config::MAIN_VI_OVERCLOCK_ENABLE);
  m_dirty |= Percentage("VBI frequency", Config::MAIN_VI_OVERCLOCK, 1, 500);
  ShellWidgets::HintText("The VBI frequency also changes the emulated CPU clock to keep them in "
                         "step. Values other than 100% break many games.");

  SectionTitle("Memory override");
  m_dirty |= Toggle("Override emulated memory size", Config::MAIN_RAM_OVERRIDE_ENABLE);
  if (Config::Get(Config::MAIN_RAM_OVERRIDE_ENABLE))
  {
    m_dirty |= Megabytes("MEM1", Config::MAIN_MEM1_SIZE, 24, 64);
    m_dirty |= Megabytes("MEM2", Config::MAIN_MEM2_SIZE, 64, 256);
  }
  ShellWidgets::HintText("Sizes other than the console's break many games.");

  DrawCustomRTC();

  SectionTitle("Timing");
  m_dirty |= Toggle("Precision frame timing", Config::MAIN_PRECISION_FRAME_TIMING);
  m_dirty |= Toggle("Rush frame presentation", Config::MAIN_RUSH_FRAME_PRESENTATION);
  m_dirty |= Toggle("Smooth early presentation", Config::MAIN_SMOOTH_EARLY_PRESENTATION);
  m_dirty |= Toggle("Correct time drift", Config::MAIN_CORRECT_TIME_DRIFT);

  SectionTitle("Network dumps");
  m_dirty |= Toggle("Dump SSL reads", Config::MAIN_NETWORK_SSL_DUMP_READ);
  m_dirty |= Toggle("Dump SSL writes", Config::MAIN_NETWORK_SSL_DUMP_WRITE);
  m_dirty |= Toggle("Dump root CA certificates", Config::MAIN_NETWORK_SSL_DUMP_ROOT_CA);
  m_dirty |= Toggle("Dump peer certificates", Config::MAIN_NETWORK_SSL_DUMP_PEER_CERT);
  m_dirty |= Toggle("Verify SSL certificates", Config::MAIN_NETWORK_SSL_VERIFY_CERTIFICATES);
  m_dirty |= Toggle("Dump Broadband Adapter traffic", Config::MAIN_NETWORK_DUMP_BBA);
  m_dirty |= Select("Dump format", Config::MAIN_NETWORK_DUMP_AS_PCAP, NETWORK_DUMP_FORMATS);
  ShellWidgets::HintText("Dumps are saved in the Network folder of the dumps location.");

  SectionTitle("Reset");
  ActionButton("Reset all settings", "settings-reset");
}

void SettingsPages::DrawCustomRTC()
{
  using namespace std::chrono;

  SectionTitle("Custom RTC");
  m_dirty |= Toggle("Enable custom RTC", Config::MAIN_CUSTOM_RTC_ENABLE);
  if (!Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE))
  {
    ShellWidgets::HintText("A custom real time clock runs separately from the system time.");
    return;
  }

  const sys_seconds time{seconds{Config::Get(Config::MAIN_CUSTOM_RTC_VALUE)}};
  const sys_days day = floor<days>(time);
  const year_month_day date{day};
  const hh_mm_ss clock{time - day};
  int year_value = static_cast<int>(date.year());
  int month_value = static_cast<int>(static_cast<unsigned>(date.month()));
  int day_value = static_cast<int>(static_cast<unsigned>(date.day()));
  int hour = static_cast<int>(clock.hours().count());
  int minute = static_cast<int>(clock.minutes().count());
  int second = static_cast<int>(clock.seconds().count());

  bool changed = false;
  changed |= UwpImGuiFrontend::ShellIntegerStepper("Year", &year_value, 2000, 2099);
  changed |= UwpImGuiFrontend::ShellIntegerStepper("Month", &month_value, 1, 12);
  const unsigned last_day = static_cast<unsigned>(
      year_month_day_last{year{year_value} / month{static_cast<unsigned>(month_value)} / last}
          .day());
  day_value = std::min(day_value, static_cast<int>(last_day));
  changed |=
      UwpImGuiFrontend::ShellIntegerStepper("Day", &day_value, 1, static_cast<int>(last_day));
  changed |= UwpImGuiFrontend::ShellIntegerStepper("Hour", &hour, 0, 23);
  changed |= UwpImGuiFrontend::ShellIntegerStepper("Minute", &minute, 0, 59);
  changed |= UwpImGuiFrontend::ShellIntegerStepper("Second", &second, 0, 59);
  ShellWidgets::HintText("The time is in UTC.");
  if (!changed)
    return;

  const sys_seconds value = sys_days{year{year_value} / month{static_cast<unsigned>(month_value)} /
                                     static_cast<unsigned>(day_value)} +
                            hours{hour} + minutes{minute} + seconds{second};
  Config::SetBaseOrCurrent(Config::MAIN_CUSTOM_RTC_VALUE,
                           static_cast<u32>(value.time_since_epoch().count()));
  m_dirty = true;
}

void SettingsPages::DrawAbout()
{
  SectionTitle("Dolphin");
  ShellWidgets::ValueRow("version", "Version", Common::GetScmDescStr());
  ShellWidgets::ValueRow("branch", "Branch", Common::GetScmBranchStr());
  ShellWidgets::ValueRow("revision", "Revision", Common::GetScmRevGitStr());
  try
  {
    const auto version = winrt::Windows::ApplicationModel::Package::Current().Id().Version();
    ShellWidgets::ValueRow(
        "package", "App package",
        fmt::format("{}.{}.{}.{}", version.Major, version.Minor, version.Build, version.Revision));
  }
  catch (const winrt::hresult_error&)
  {
  }
  ShellWidgets::ValueRow("device", "Device", Common::UWP::IsXboxConsole() ? "Xbox" : "Windows");

  SectionTitle("License");
  ShellWidgets::HintText("Dolphin is a free and open-source GameCube and Wii emulator, licensed "
                         "under the GNU General Public License version 2 or later. The licenses of "
                         "the third-party components are included with the app.");
  ShellWidgets::HintText("This software should not be used to play games you do not legally own.");
}

void SettingsPages::DrawAchievements()
{
  AchievementManager& achievements = AchievementManager::GetInstance();
  SectionTitle("RetroAchievements");
  if (Toggle("Enable RetroAchievements", Config::RA_ENABLED))
  {
    Config::Save();
    if (Config::Get(Config::RA_ENABLED))
      achievements.Init(nullptr);
    else
      achievements.Shutdown();
  }
  if (!Config::Get(Config::RA_ENABLED))
  {
    ShellWidgets::HintText("Earn achievements in supported games with a free account from "
                           "retroachievements.org.");
    return;
  }

  SectionTitle("Account");
  if (achievements.HasAPIToken())
  {
    // The token arrives after the sign-in started, so it is saved as soon as it is here.
    if (std::exchange(m_login_pending, false))
      Config::Save();
    ShellWidgets::ValueRow("ra-user", "Signed in as", Config::Get(Config::RA_USERNAME), false);
    if (UwpImGuiFrontend::ShellButton("Sign out"))
    {
      PromptService::GetInstance().Confirm("RetroAchievements", "Sign out of RetroAchievements?",
                                           [](bool confirmed) {
                                             if (!confirmed)
                                               return;
                                             AchievementManager::GetInstance().Logout();
                                             Config::Save();
                                           });
    }
  }
  else
  {
    auto [username_entry, username_inserted] = m_text_buffers.try_emplace("ra-username");
    auto& username = username_entry->second;
    if (username_inserted)
    {
      username.fill('\0');
      const std::string saved = Config::Get(Config::RA_USERNAME);
      saved.copy(username.data(), username.size() - 1);
    }
    auto [password_entry, password_inserted] = m_text_buffers.try_emplace("ra-password");
    auto& password = password_entry->second;
    if (password_inserted)
      password.fill('\0');

    (void)UwpImGuiFrontend::NativeInputText("Username", username.data(), username.size(), 0,
                                            UwpImGuiFrontend::TextInputScope::UserName);
    (void)UwpImGuiFrontend::NativeInputText("Password", password.data(), password.size(),
                                            ImGuiInputTextFlags_Password,
                                            UwpImGuiFrontend::TextInputScope::Password);
    if (UwpImGuiFrontend::ShellButton("Sign in") && username[0] != '\0' && password[0] != '\0')
    {
      Config::SetBaseOrCurrent(Config::RA_USERNAME, std::string(StripWhitespace(username.data())));
      m_login_result = RC_OK;
      m_login_pending = true;
      achievements.Login(password.data());
      password.fill('\0');
      Config::Save();
    }
    if (m_login_pending && m_login_result == RC_INVALID_CREDENTIALS)
      ShellWidgets::HintText("Signing in failed. The username or password is wrong.");
    else if (m_login_pending && m_login_result != RC_OK)
      ShellWidgets::HintText(fmt::format("Signing in failed: {}.", rc_error_str(m_login_result)));
    else if (m_login_pending)
      ShellWidgets::HintText("Signing in");
  }

  SectionTitle("Options");
  m_dirty |= Toggle("Hardcore mode", Config::RA_HARDCORE_ENABLED);
  ShellWidgets::HintText("Hardcore mode turns off loading states, cheats, memory patches, speeds "
                         "below 100%, frame advance, free look and the debugger.");
  m_dirty |= Toggle("Unofficial achievements", Config::RA_UNOFFICIAL_ENABLED);
  m_dirty |= Toggle("Encore mode", Config::RA_ENCORE_ENABLED);
  if (Toggle("Spectator mode", Config::RA_SPECTATOR_ENABLED))
  {
    m_dirty = true;
    achievements.SetSpectatorMode();
  }
  m_dirty |= Toggle("Show the leaderboard tracker", Config::RA_LEADERBOARD_TRACKER_ENABLED);
  m_dirty |= Toggle("Show challenge indicators", Config::RA_CHALLENGE_INDICATORS_ENABLED);
  m_dirty |= Toggle("Show progress notifications", Config::RA_PROGRESS_ENABLED);
}

void SettingsPages::DrawGraphicsGeneral()
{
  SectionTitle("Renderer");
  const std::string backend = Config::Get(Config::MAIN_GFX_BACKEND);
  std::vector<ChoiceItem> backends;
  size_t selected = 0;
  for (size_t i = 0; i < VIDEO_BACKENDS.size(); ++i)
  {
    if (backend == VIDEO_BACKENDS[i].value)
      selected = i;
    backends.push_back({VIDEO_BACKENDS[i].value, VIDEO_BACKENDS[i].label});
  }
  if (UwpImGuiFrontend::ShellCombo("Backend", &selected, backends))
  {
    Config::SetBaseOrCurrent(Config::MAIN_GFX_BACKEND, std::string(VIDEO_BACKENDS[selected].value));
    m_dirty = true;
  }
  DrawAdapter();
  m_dirty |= Toggle("V-Sync", Config::GFX_VSYNC);
  m_dirty |= Select("Aspect ratio", Config::GFX_ASPECT_RATIO, ASPECT_MODES);
  const AspectMode aspect = Config::Get(Config::GFX_ASPECT_RATIO);
  if (aspect == AspectMode::Custom || aspect == AspectMode::CustomStretch)
  {
    m_dirty |= CustomAspectRatio();
  }
  if (!Common::UWP::IsXboxConsole())
  {
    m_dirty |= Toggle("Start games in fullscreen", Config::MAIN_FULLSCREEN);
    m_dirty |= Toggle("Adjust the window size to the game", Config::MAIN_RENDER_WINDOW_AUTOSIZE);
  }

  SectionTitle("Shaders");
  m_dirty |= Select("Shader compilation", Config::GFX_SHADER_COMPILATION_MODE, SHADER_MODES);
  m_dirty |=
      Toggle("Compile shaders before starting", Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING);

  SectionTitle("Free Look");
  m_dirty |= Toggle("Enable Free Look", Config::FREE_LOOK_ENABLED);
  m_dirty |= Select("Camera control", Config::FL1_CONTROL_TYPE, FREE_LOOK_CONTROL_TYPES);
  m_dirty |= Toggle("Allow background input", Config::FREE_LOOK_BACKGROUND_INPUT);
  ActionButton("Configure Free Look controls", "freelook-controls");
}

void SettingsPages::DrawAdapter()
{
  // Each Direct3D backend picks the adapter by its index in the DXGI adapter order.
  if (!m_adapters_loaded)
  {
    m_adapters_loaded = true;
    if (D3DCommon::LoadLibraries())
      m_adapters = D3DCommon::GetAdapterNames();
  }
  if (m_adapters.size() < 2)
    return;

  std::vector<ChoiceItem> items;
  for (size_t i = 0; i < m_adapters.size(); ++i)
    items.push_back({std::to_string(i), m_adapters[i]});
  size_t selected = static_cast<size_t>(
      std::clamp(Config::Get(Config::GFX_ADAPTER), 0, static_cast<int>(m_adapters.size()) - 1));
  if (UwpImGuiFrontend::ShellCombo("Adapter", &selected, items))
  {
    Config::SetBaseOrCurrent(Config::GFX_ADAPTER, static_cast<int>(selected));
    m_dirty = true;
  }
}

bool SettingsPages::CustomAspectRatio()
{
  constexpr int MAX_CUSTOM_ASPECT_RATIO = 10000;
  bool changed = false;
  for (const auto& [label, info] :
       {std::pair{"Aspect ratio width", &Config::GFX_CUSTOM_ASPECT_RATIO_WIDTH},
        std::pair{"Aspect ratio height", &Config::GFX_CUSTOM_ASPECT_RATIO_HEIGHT}})
  {
    int value = Config::Get(*info);
    if (UwpImGuiFrontend::ShellIntegerStepper(label, &value, 1, MAX_CUSTOM_ASPECT_RATIO))
    {
      Config::SetBaseOrCurrent(*info, value);
      changed = true;
    }
  }
  return changed;
}

void SettingsPages::DrawEnhancements()
{
  SectionTitle("Resolution");
  m_dirty |= Select("Internal resolution", Config::GFX_EFB_SCALE, RESOLUTIONS);
  m_dirty |= Select("Anti-aliasing", Config::GFX_MSAA, MSAA_MODES);
  m_dirty |= Toggle("Use supersampling", Config::GFX_SSAA);

  SectionTitle("Filtering");
  m_dirty |= Select("Anisotropic filtering", Config::GFX_ENHANCE_MAX_ANISOTROPY, ANISOTROPY_MODES);
  m_dirty |= Select("Texture filtering", Config::GFX_ENHANCE_FORCE_TEXTURE_FILTERING,
                    TEXTURE_FILTERING_MODES);
  m_dirty |= Select("Output resampling", Config::GFX_ENHANCE_OUTPUT_RESAMPLING, RESAMPLING_MODES);

  SectionTitle("Other");
  m_dirty |= Toggle("Scaled EFB copy", Config::GFX_HACK_COPY_EFB_SCALED);
  m_dirty |= Toggle("Per-pixel lighting", Config::GFX_ENABLE_PIXEL_LIGHTING);
  m_dirty |= Toggle("Widescreen hack", Config::GFX_WIDESCREEN_HACK);
  m_dirty |= Toggle("Disable fog", Config::GFX_DISABLE_FOG);
  m_dirty |= Toggle("Force 24-bit color", Config::GFX_ENHANCE_FORCE_TRUE_COLOR);
  m_dirty |= Toggle("Disable copy filter", Config::GFX_ENHANCE_DISABLE_COPY_FILTER);
  m_dirty |= Toggle("Arbitrary mipmap detection", Config::GFX_ENHANCE_ARBITRARY_MIPMAP_DETECTION);

  DrawPostProcessing();
  DrawColorCorrection();
}

void SettingsPages::DrawPostProcessing()
{
  SectionTitle("Post-processing");
  const StereoMode stereo_mode = Config::Get(Config::GFX_STEREO_MODE);
  std::vector<std::string> shaders =
      stereo_mode == StereoMode::Anaglyph ? VideoCommon::PostProcessing::GetAnaglyphShaderList() :
      stereo_mode == StereoMode::Passive  ? VideoCommon::PostProcessing::GetPassiveShaderList() :
                                            VideoCommon::PostProcessing::GetShaderList();
  const bool can_disable =
      stereo_mode != StereoMode::Anaglyph && stereo_mode != StereoMode::Passive;
  if (can_disable)
    shaders.insert(shaders.begin(), std::string());

  const std::string current = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  std::vector<ChoiceItem> items;
  size_t selected = 0;
  for (size_t i = 0; i < shaders.size(); ++i)
  {
    if (shaders[i] == current)
      selected = i;
    items.push_back({std::to_string(i), shaders[i].empty() ? std::string("Off") : shaders[i]});
  }
  if (!items.empty() && UwpImGuiFrontend::ShellCombo("Effect", &selected, items))
  {
    if (m_post_processing && m_post_processing->IsDirty())
      m_post_processing->SaveOptionsConfiguration();
    m_post_processing.reset();
    Config::SetBaseOrCurrent(Config::GFX_ENHANCE_POST_SHADER, shaders[selected]);
    m_dirty = true;
  }

  const std::string shader = Config::Get(Config::GFX_ENHANCE_POST_SHADER);
  if (shader.empty())
    return;
  if (!m_post_processing || m_post_processing->GetShader() != shader)
  {
    m_post_processing = std::make_unique<VideoCommon::PostProcessingConfiguration>();
    m_post_processing->LoadShader(shader);
  }

  auto& options = m_post_processing->GetOptions();
  std::vector<
      std::pair<std::string, VideoCommon::PostProcessingConfiguration::ConfigurationOption*>>
      sorted;
  for (auto& [name, option] : options)
    sorted.emplace_back(name, &option);
  std::ranges::sort(sorted, {}, [](const auto& entry) { return entry.second->m_gui_name; });

  using OptionType = VideoCommon::PostProcessingConfiguration::ConfigurationOption::OptionType;
  for (const auto& [name, option] : sorted)
  {
    if (!option->m_dependent_option.empty())
    {
      const auto parent = options.find(option->m_dependent_option);
      if (parent != options.end() && !parent->second.m_bool_value)
        continue;
    }

    ImGui::PushID(name.c_str());
    switch (option->m_type)
    {
    case OptionType::Bool:
    {
      bool value = option->m_bool_value;
      if (UwpImGuiFrontend::ShellCheckbox(option->m_gui_name.c_str(), &value))
        m_post_processing->SetOptionb(name, value);
      break;
    }
    case OptionType::Float:
      for (size_t i = 0; i < option->m_float_values.size(); ++i)
      {
        float value = option->m_float_values[i];
        const std::string label = option->m_float_values.size() > 1 ?
                                      fmt::format("{} {}", option->m_gui_name, i + 1) :
                                      option->m_gui_name;
        ImGui::PushID(static_cast<int>(i));
        if (UwpImGuiFrontend::ShellSliderFloat(label.c_str(), &value, option->m_float_min_values[i],
                                               option->m_float_max_values[i], "%.3f"))
        {
          m_post_processing->SetOptionf(name, static_cast<int>(i), value);
        }
        ImGui::PopID();
      }
      break;
    case OptionType::Integer:
      for (size_t i = 0; i < option->m_integer_values.size(); ++i)
      {
        int value = option->m_integer_values[i];
        const std::string label = option->m_integer_values.size() > 1 ?
                                      fmt::format("{} {}", option->m_gui_name, i + 1) :
                                      option->m_gui_name;
        ImGui::PushID(static_cast<int>(i));
        if (UwpImGuiFrontend::ShellIntegerStepper(
                label.c_str(), &value, option->m_integer_min_values[i],
                option->m_integer_max_values[i], std::max(option->m_integer_step_values[i], 1)))
        {
          m_post_processing->SetOptioni(name, static_cast<int>(i), value);
        }
        ImGui::PopID();
      }
      break;
    }
    ImGui::PopID();
  }
}

void SettingsPages::DrawColorCorrection()
{
  SectionTitle("Color correction");
  m_dirty |= Toggle("Correct color space", Config::GFX_CC_CORRECT_COLOR_SPACE);
  if (Config::Get(Config::GFX_CC_CORRECT_COLOR_SPACE))
    m_dirty |= Select("Game color space", Config::GFX_CC_GAME_COLOR_SPACE, GAME_COLOR_SPACES);
  m_dirty |= Toggle("Correct gamma", Config::GFX_CC_CORRECT_GAMMA);
  if (Config::Get(Config::GFX_CC_CORRECT_GAMMA))
  {
    m_dirty |= FloatSlider("Game gamma", Config::GFX_CC_GAME_GAMMA, Config::GFX_CC_GAME_GAMMA_MIN,
                           Config::GFX_CC_GAME_GAMMA_MAX);
    m_dirty |= Toggle("Display uses sRGB gamma", Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB);
    if (!Config::Get(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB))
    {
      m_dirty |= FloatSlider("Display gamma", Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA,
                             Config::GFX_CC_DISPLAY_GAMMA_MIN, Config::GFX_CC_DISPLAY_GAMMA_MAX);
    }
  }

  SectionTitle("HDR");
  m_dirty |= Toggle("HDR post-processing", Config::GFX_ENHANCE_HDR_OUTPUT);
  if (Config::Get(Config::GFX_ENHANCE_HDR_OUTPUT))
  {
    m_dirty |= FloatSlider("Paper white", Config::GFX_CC_HDR_PAPER_WHITE_NITS,
                           Config::GFX_CC_HDR_PAPER_WHITE_NITS_MIN,
                           Config::GFX_CC_HDR_PAPER_WHITE_NITS_MAX, "%.0f nits");
  }
  ShellWidgets::HintText("HDR output is used when the display supports it.");
}

void SettingsPages::DrawStereoscopy()
{
  SectionTitle("Stereoscopic 3D");
  if (Select("Mode", Config::GFX_STEREO_MODE, STEREO_MODES))
  {
    const StereoMode mode = Config::Get(Config::GFX_STEREO_MODE);
    std::string shader;
    if (mode == StereoMode::Anaglyph)
      shader = "dubois";
    else if (mode == StereoMode::Passive)
      shader = "horizontal";
    Config::SetBaseOrCurrent(Config::GFX_ENHANCE_POST_SHADER, shader);
    m_post_processing.reset();
    m_dirty = true;
  }
  if (Config::Get(Config::GFX_STEREO_MODE) == StereoMode::Off)
    return;

  m_dirty |= FloatSlider("Depth", Config::GFX_STEREO_DEPTH, 0.0f, Config::GFX_STEREO_DEPTH_MAXIMUM,
                         "%.0f");
  m_dirty |= FloatSlider("Convergence", Config::GFX_STEREO_CONVERGENCE, 0.0f,
                         Config::GFX_STEREO_CONVERGENCE_MAXIMUM, "%.0f");
  m_dirty |= Toggle("Swap eyes", Config::GFX_STEREO_SWAP_EYES);
  m_dirty |= Toggle("Use full resolution per eye", Config::GFX_STEREO_PER_EYE_RESOLUTION_FULL);
}

void SettingsPages::DrawHacks()
{
  SectionTitle("Embedded frame buffer");
  m_dirty |= Toggle("Skip EFB access from CPU", Config::GFX_HACK_EFB_ACCESS_ENABLE, true);
  m_dirty |= Toggle("Ignore format changes", Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true);
  m_dirty |= Toggle("Store EFB copies to texture only", Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  m_dirty |= Toggle("Defer EFB copies to RAM", Config::GFX_HACK_DEFER_EFB_COPIES);

  SectionTitle("Texture cache");
  m_dirty |= Select("Accuracy", Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES, TEXTURE_CACHE_MODES);
  m_dirty |= Toggle("GPU texture decoding", Config::GFX_ENABLE_GPU_TEXTURE_DECODING);

  SectionTitle("External frame buffer");
  m_dirty |= Toggle("Store XFB copies to texture only", Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  m_dirty |= Toggle("Immediately present XFB", Config::GFX_HACK_IMMEDIATE_XFB);
  m_dirty |= Toggle("Skip presenting duplicate frames", Config::GFX_HACK_SKIP_DUPLICATE_XFBS);

  SectionTitle("Other");
  m_dirty |= Toggle("Fast depth calculation", Config::GFX_FAST_DEPTH_CALC);
  m_dirty |= Toggle("Disable bounding box", Config::GFX_HACK_BBOX_ENABLE, true);
  m_dirty |= Toggle("Vertex rounding", Config::GFX_HACK_VERTEX_ROUNDING);
  m_dirty |= Toggle("VBI skip", Config::GFX_HACK_VI_SKIP);
  m_dirty |= Toggle("Save texture cache to state", Config::GFX_SAVE_TEXTURE_CACHE_TO_STATE);
}

void SettingsPages::DrawGraphicsAdvanced()
{
  SectionTitle("Debugging");
  m_dirty |= Toggle("Enable wireframe", Config::GFX_ENABLE_WIREFRAME);
  m_dirty |= Toggle("Texture format overlay", Config::GFX_TEXFMT_OVERLAY_ENABLE);
  m_dirty |= Toggle("Enable API validation layers", Config::GFX_ENABLE_VALIDATION_LAYER);
  m_dirty |= Toggle("Log render time to file", Config::GFX_LOG_RENDER_TIME_TO_FILE);
  ShellWidgets::HintText("API validation layers need the Graphics Tools feature of Windows.");

  SectionTitle("Utility");
  m_dirty |= Toggle("Load custom textures", Config::GFX_HIRES_TEXTURES);
  m_dirty |= Toggle("Prefetch custom textures", Config::GFX_CACHE_HIRES_TEXTURES);
  m_dirty |= Toggle("Enable graphics mods", Config::GFX_MODS_ENABLE);
  m_dirty |= Toggle("Dump EFB target", Config::GFX_DUMP_EFB_TARGET);
  m_dirty |= Toggle("Dump XFB target", Config::GFX_DUMP_XFB_TARGET);
  m_dirty |= Toggle("Disable EFB VRAM copies", Config::GFX_HACK_DISABLE_COPY_TO_VRAM);

  SectionTitle("Texture dumping");
  m_dirty |= Toggle("Dump textures", Config::GFX_DUMP_TEXTURES);
  if (Config::Get(Config::GFX_DUMP_TEXTURES))
  {
    m_dirty |= Toggle("Dump base textures", Config::GFX_DUMP_BASE_TEXTURES);
    m_dirty |= Toggle("Dump mip maps", Config::GFX_DUMP_MIP_TEXTURES);
  }

  SectionTitle("Frame dumping");
  m_dirty |= Select("Frame dump resolution", Config::GFX_FRAME_DUMPS_RESOLUTION_TYPE,
                    FRAME_DUMP_RESOLUTIONS);
  m_dirty |= IntegerSlider("PNG compression level", Config::GFX_PNG_COMPRESSION_LEVEL, 0, 9);
  ShellWidgets::HintText("Frames are dumped as PNG images in the Frames folder of the dumps "
                         "location. Start and stop frame dumping from the quick menu.");

  SectionTitle("Crop");
  m_dirty |= Toggle("Crop to the aspect ratio", Config::GFX_CROP_TO_ASPECT_RATIO);
  m_dirty |= Toggle("Custom crop", Config::GFX_CROP_CUSTOM);
  if (Config::Get(Config::GFX_CROP_CUSTOM))
  {
    m_dirty |= IntegerSlider("Left", Config::GFX_CROP_CUSTOM_LEFT, 0, 640);
    m_dirty |= IntegerSlider("Top", Config::GFX_CROP_CUSTOM_TOP, 0, 528);
    m_dirty |= IntegerSlider("Right", Config::GFX_CROP_CUSTOM_RIGHT, 0, 640);
    m_dirty |= IntegerSlider("Bottom", Config::GFX_CROP_CUSTOM_BOTTOM, 0, 528);
  }

  SectionTitle("Other");
  m_dirty |= Toggle("Force progressive scan output", Config::GFX_HACK_FORCE_PROGRESSIVE);
  m_dirty |= Toggle("Prefer VS for point and line expansion",
                    Config::GFX_PREFER_VS_FOR_LINE_POINT_EXPANSION);
  m_dirty |= Toggle("Cull vertices on the CPU", Config::GFX_CPU_CULL);

  SectionTitle("Experimental");
  m_dirty |= Toggle("Defer EFB cache invalidation", Config::GFX_HACK_EFB_DEFER_INVALIDATION);
  m_dirty |= Toggle("Manual texture sampling", Config::GFX_HACK_FAST_TEXTURE_SAMPLING, true);
}
}  // namespace DolphinUWP

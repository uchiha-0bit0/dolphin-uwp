// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/TasView.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>
#include <string_view>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/ImGuiShell.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/Config/Config.h"
#include "Common/MathUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/Core.h"
#include "Core/HW/GBAPad.h"
#include "Core/HW/GBAPadEmu.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/GCPadEmu.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/MotionPlus.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/Movie.h"
#include "Core/System.h"
#include "DolphinUWP/GBAControls.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/StickGate.h"
#include "InputCommon/InputConfig.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;
using ControllerEmu::ReshapableInput;

constexpr size_t PANEL_ROWS = 22;

enum class ControlType
{
  Heading,
  Button,
  Axis,
  Scaled,
};

struct Control
{
  ControlType type = ControlType::Heading;
  std::string label;
  std::string group;
  std::string control;
  // 0 is the controller and 1 the extension of a Wii Remote.
  int target = 0;
  int minimum = 0;
  int maximum = 1;
  int zero = 0;
  int initial = 0;
  double scale = 1.0;
};

ImU32 Color(const ImVec4& color)
{
  return ImGui::ColorConvertFloat4ToU32(color);
}

void AddHeading(std::vector<Control>* controls, std::string label)
{
  controls->push_back({ControlType::Heading, std::move(label)});
}

void AddButton(std::vector<Control>* controls, std::string label, std::string_view group,
               std::string_view control, int target = 0)
{
  controls->push_back({ControlType::Button, std::move(label), std::string(group),
                       std::string(control), target, 0, 2, 0, 0});
}

void AddValue(std::vector<Control>* controls, std::string label, std::string_view group,
              std::string_view control, int zero, int initial, int minimum, int maximum,
              int target = 0, std::optional<double> scale = std::nullopt)
{
  controls->push_back({scale ? ControlType::Scaled : ControlType::Axis, std::move(label),
                       std::string(group), std::string(control), target, minimum, maximum, zero,
                       initial, scale.value_or(1.0)});
}

void AddStick(std::vector<Control>* controls, const std::string& label, std::string_view group,
              int minimum, int maximum, int target = 0)
{
  const int center = static_cast<int>(std::lround(maximum / 2.0));
  AddValue(controls, label + " X", group, ReshapableInput::X_INPUT_OVERRIDE, center, center,
           minimum, maximum, target);
  AddValue(controls, label + " Y", group, ReshapableInput::Y_INPUT_OVERRIDE, center, center,
           minimum, maximum, target);
}

void AddDPad(std::vector<Control>* controls, std::string_view group, int target = 0)
{
  AddButton(controls, "Up", group, DIRECTION_UP, target);
  AddButton(controls, "Down", group, DIRECTION_DOWN, target);
  AddButton(controls, "Left", group, DIRECTION_LEFT, target);
  AddButton(controls, "Right", group, DIRECTION_RIGHT, target);
}

WiimoteEmu::Wiimote* GetWiimote(int index)
{
  return static_cast<WiimoteEmu::Wiimote*>(Wiimote::GetConfig()->GetController(index));
}

ControllerEmu::ControlGroupContainer* GetExtension(WiimoteEmu::Wiimote* wiimote,
                                                   WiimoteEmu::ExtensionNumber number)
{
  auto* const attachments = static_cast<ControllerEmu::Attachments*>(
      wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
  return attachments->GetAttachmentList()[number].get();
}

std::vector<Control> BuildControls(TasView::Target target, WiimoteEmu::ExtensionNumber extension,
                                   bool motion_plus)
{
  std::vector<Control> controls;
  switch (target.kind)
  {
  case TasView::Kind::GameCube:
    AddHeading(&controls, "Main stick");
    AddStick(&controls, "Main stick", GCPad::MAIN_STICK_GROUP, 1, 255);
    AddHeading(&controls, "C stick");
    AddStick(&controls, "C stick", GCPad::C_STICK_GROUP, 1, 255);
    AddHeading(&controls, "Triggers");
    AddValue(&controls, "L analog", GCPad::TRIGGERS_GROUP, GCPad::L_ANALOG, 0, 0, 0, 255);
    AddValue(&controls, "R analog", GCPad::TRIGGERS_GROUP, GCPad::R_ANALOG, 0, 0, 0, 255);
    AddButton(&controls, "L", GCPad::TRIGGERS_GROUP, GCPad::L_DIGITAL);
    AddButton(&controls, "R", GCPad::TRIGGERS_GROUP, GCPad::R_DIGITAL);
    AddHeading(&controls, "Buttons");
    AddButton(&controls, "A", GCPad::BUTTONS_GROUP, GCPad::A_BUTTON);
    AddButton(&controls, "B", GCPad::BUTTONS_GROUP, GCPad::B_BUTTON);
    AddButton(&controls, "X", GCPad::BUTTONS_GROUP, GCPad::X_BUTTON);
    AddButton(&controls, "Y", GCPad::BUTTONS_GROUP, GCPad::Y_BUTTON);
    AddButton(&controls, "Z", GCPad::BUTTONS_GROUP, GCPad::Z_BUTTON);
    AddButton(&controls, "Start", GCPad::BUTTONS_GROUP, GCPad::START_BUTTON);
    AddHeading(&controls, "D-Pad");
    AddDPad(&controls, GCPad::DPAD_GROUP);
    break;
  case TasView::Kind::GBA:
    AddHeading(&controls, "Buttons");
    AddButton(&controls, "A", GBAPad::BUTTONS_GROUP, GBAPad::A_BUTTON);
    AddButton(&controls, "B", GBAPad::BUTTONS_GROUP, GBAPad::B_BUTTON);
    AddButton(&controls, "L", GBAPad::BUTTONS_GROUP, GBAPad::L_BUTTON);
    AddButton(&controls, "R", GBAPad::BUTTONS_GROUP, GBAPad::R_BUTTON);
    AddButton(&controls, "Select", GBAPad::BUTTONS_GROUP, GBAPad::SELECT_BUTTON);
    AddButton(&controls, "Start", GBAPad::BUTTONS_GROUP, GBAPad::START_BUTTON);
    AddHeading(&controls, "D-Pad");
    AddDPad(&controls, GBAPad::DPAD_GROUP);
    break;
  case TasView::Kind::WiiRemote:
  {
    using WiimoteEmu::Wiimote;
    constexpr int ACCEL_ZERO_G = Wiimote::ACCEL_ZERO_G << 2;
    constexpr int ACCEL_ONE_G = Wiimote::ACCEL_ONE_G << 2;
    constexpr int ACCEL_MAX = (1 << 10) - 1;
    constexpr double ACCEL_SCALE = (ACCEL_ONE_G - ACCEL_ZERO_G) / MathUtil::GRAVITY_ACCELERATION;
    constexpr int IR_MAX_X = 1023;
    constexpr int IR_MAX_Y = 767;

    if (extension != WiimoteEmu::ExtensionNumber::CLASSIC)
    {
      AddHeading(&controls, "Pointer");
      AddValue(&controls, "Pointer X", Wiimote::IR_GROUP, ReshapableInput::X_INPUT_OVERRIDE,
               IR_MAX_X / 2 + 1, IR_MAX_X / 2 + 1, 0, IR_MAX_X);
      AddValue(&controls, "Pointer Y", Wiimote::IR_GROUP, ReshapableInput::Y_INPUT_OVERRIDE,
               IR_MAX_Y / 2 + 1, IR_MAX_Y / 2 + 1, 0, IR_MAX_Y);
      AddHeading(&controls, "Wii Remote buttons");
      AddButton(&controls, "A", Wiimote::BUTTONS_GROUP, Wiimote::A_BUTTON);
      AddButton(&controls, "B", Wiimote::BUTTONS_GROUP, Wiimote::B_BUTTON);
      AddButton(&controls, "1", Wiimote::BUTTONS_GROUP, Wiimote::ONE_BUTTON);
      AddButton(&controls, "2", Wiimote::BUTTONS_GROUP, Wiimote::TWO_BUTTON);
      AddButton(&controls, "+", Wiimote::BUTTONS_GROUP, Wiimote::PLUS_BUTTON);
      AddButton(&controls, "-", Wiimote::BUTTONS_GROUP, Wiimote::MINUS_BUTTON);
      AddButton(&controls, "Home", Wiimote::BUTTONS_GROUP, Wiimote::HOME_BUTTON);
      AddDPad(&controls, Wiimote::DPAD_GROUP);
      AddHeading(&controls, "Wii Remote accelerometer");
      AddValue(&controls, "Accelerometer X", Wiimote::ACCELEROMETER_GROUP,
               ReshapableInput::X_INPUT_OVERRIDE, ACCEL_ZERO_G, ACCEL_ZERO_G, 0, ACCEL_MAX, 0,
               ACCEL_SCALE);
      AddValue(&controls, "Accelerometer Y", Wiimote::ACCELEROMETER_GROUP,
               ReshapableInput::Y_INPUT_OVERRIDE, ACCEL_ZERO_G, ACCEL_ZERO_G, 0, ACCEL_MAX, 0,
               ACCEL_SCALE);
      AddValue(&controls, "Accelerometer Z", Wiimote::ACCELEROMETER_GROUP,
               ReshapableInput::Z_INPUT_OVERRIDE, ACCEL_ZERO_G, ACCEL_ONE_G, 0, ACCEL_MAX, 0,
               ACCEL_SCALE);
      if (motion_plus)
      {
        using WiimoteEmu::MotionPlus;
        // The precision of the slow scale with the range of the fast scale.
        constexpr double GYRO_STRETCH =
            static_cast<double>(MotionPlus::CALIBRATION_FAST_SCALE_DEGREES) /
            MotionPlus::CALIBRATION_SLOW_SCALE_DEGREES;
        constexpr int GYRO_MAX = static_cast<int>(MotionPlus::MAX_VALUE * GYRO_STRETCH);
        constexpr int GYRO_ZERO = static_cast<int>(MotionPlus::ZERO_VALUE * GYRO_STRETCH);
        constexpr double GYRO_SCALE = GYRO_MAX / 2 / MotionPlus::FAST_MAX_RAD_PER_SEC;
        AddHeading(&controls, "Wii Remote gyroscope");
        AddValue(&controls, "Gyroscope X", Wiimote::GYROSCOPE_GROUP,
                 ReshapableInput::X_INPUT_OVERRIDE, GYRO_ZERO, GYRO_ZERO, 0, GYRO_MAX, 0,
                 GYRO_SCALE);
        AddValue(&controls, "Gyroscope Y", Wiimote::GYROSCOPE_GROUP,
                 ReshapableInput::Y_INPUT_OVERRIDE, GYRO_ZERO, GYRO_ZERO, 0, GYRO_MAX, 0,
                 GYRO_SCALE);
        AddValue(&controls, "Gyroscope Z", Wiimote::GYROSCOPE_GROUP,
                 ReshapableInput::Z_INPUT_OVERRIDE, GYRO_ZERO, GYRO_ZERO, 0, GYRO_MAX, 0,
                 GYRO_SCALE);
      }
    }

    if (extension == WiimoteEmu::ExtensionNumber::NUNCHUK)
    {
      using WiimoteEmu::Nunchuk;
      AddHeading(&controls, "Nunchuk");
      AddStick(&controls, "Nunchuk stick", Nunchuk::STICK_GROUP, 0, 255, 1);
      AddButton(&controls, "C", Nunchuk::BUTTONS_GROUP, Nunchuk::C_BUTTON, 1);
      AddButton(&controls, "Z", Nunchuk::BUTTONS_GROUP, Nunchuk::Z_BUTTON, 1);
      AddValue(&controls, "Nunchuk accelerometer X", Nunchuk::ACCELEROMETER_GROUP,
               ReshapableInput::X_INPUT_OVERRIDE, ACCEL_ZERO_G, ACCEL_ZERO_G, 0, ACCEL_MAX, 1);
      AddValue(&controls, "Nunchuk accelerometer Y", Nunchuk::ACCELEROMETER_GROUP,
               ReshapableInput::Y_INPUT_OVERRIDE, ACCEL_ZERO_G, ACCEL_ZERO_G, 0, ACCEL_MAX, 1);
      AddValue(&controls, "Nunchuk accelerometer Z", Nunchuk::ACCELEROMETER_GROUP,
               ReshapableInput::Z_INPUT_OVERRIDE, ACCEL_ZERO_G, ACCEL_ONE_G, 0, ACCEL_MAX, 1);
    }
    else if (extension == WiimoteEmu::ExtensionNumber::CLASSIC)
    {
      using WiimoteEmu::Classic;
      AddHeading(&controls, "Classic Controller sticks");
      AddStick(&controls, "Left stick", Classic::LEFT_STICK_GROUP, 0, 63, 1);
      AddStick(&controls, "Right stick", Classic::RIGHT_STICK_GROUP, 0, 31, 1);
      AddHeading(&controls, "Classic Controller triggers");
      AddValue(&controls, "L analog", Classic::TRIGGERS_GROUP, Classic::L_ANALOG, 0, 0, 0, 31, 1);
      AddValue(&controls, "R analog", Classic::TRIGGERS_GROUP, Classic::R_ANALOG, 0, 0, 0, 31, 1);
      AddButton(&controls, "L", Classic::TRIGGERS_GROUP, Classic::L_DIGITAL, 1);
      AddButton(&controls, "R", Classic::TRIGGERS_GROUP, Classic::R_DIGITAL, 1);
      AddHeading(&controls, "Classic Controller buttons");
      AddButton(&controls, "A", Classic::BUTTONS_GROUP, Classic::A_BUTTON, 1);
      AddButton(&controls, "B", Classic::BUTTONS_GROUP, Classic::B_BUTTON, 1);
      AddButton(&controls, "X", Classic::BUTTONS_GROUP, Classic::X_BUTTON, 1);
      AddButton(&controls, "Y", Classic::BUTTONS_GROUP, Classic::Y_BUTTON, 1);
      AddButton(&controls, "ZL", Classic::BUTTONS_GROUP, Classic::ZL_BUTTON, 1);
      AddButton(&controls, "ZR", Classic::BUTTONS_GROUP, Classic::ZR_BUTTON, 1);
      AddButton(&controls, "+", Classic::BUTTONS_GROUP, Classic::PLUS_BUTTON, 1);
      AddButton(&controls, "-", Classic::BUTTONS_GROUP, Classic::MINUS_BUTTON, 1);
      AddButton(&controls, "Home", Classic::BUTTONS_GROUP, Classic::HOME_BUTTON, 1);
      AddDPad(&controls, Classic::DPAD_GROUP, 1);
    }
    break;
  }
  }
  return controls;
}
}  // namespace

struct TasControls
{
  std::mutex mutex;
  std::vector<Control> controls;
  // A button is 0 when released, 1 when held and 2 when it turbo presses every other frame.
  std::vector<int> values;
  WiimoteEmu::ExtensionNumber extension = WiimoteEmu::ExtensionNumber::NONE;
};

namespace
{
ControllerEmu::InputOverrideFunction MakeOverride(std::shared_ptr<TasControls> shared, int target)
{
  return [shared = std::move(shared), target](std::string_view group, std::string_view control,
                                              ControlState) -> std::optional<ControlState> {
    std::lock_guard lock(shared->mutex);
    for (size_t i = 0; i < shared->controls.size(); ++i)
    {
      const Control& entry = shared->controls[i];
      if (entry.type == ControlType::Heading || entry.target != target || entry.group != group ||
          entry.control != control)
      {
        continue;
      }
      const int value = shared->values[i];
      switch (entry.type)
      {
      case ControlType::Button:
        if (value == 2)
          return Core::System::GetInstance().GetMovie().GetCurrentFrame() % 2 == 0 ? 1.0 : 0.0;
        return value != 0 ? 1.0 : 0.0;
      case ControlType::Axis:
        return ControllerEmu::MapToFloat<ControlState, int>(value, entry.zero, entry.minimum,
                                                            entry.maximum);
      case ControlType::Scaled:
        return (value - entry.zero) / entry.scale;
      default:
        return std::nullopt;
      }
    }
    return std::nullopt;
  };
}
}  // namespace

TasView::TasView() = default;

TasView::~TasView()
{
  Close();
}

std::vector<TasView::Target> TasView::GetTargets()
{
  std::vector<Target> targets;
  for (int port = 0; port < 4; ++port)
  {
    const SerialInterface::SIDevices device = Config::Get(Config::GetInfoForSIDevice(port));
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      targets.push_back({Kind::GBA, port});
    else if (device != SerialInterface::SIDEVICE_NONE &&
             device != SerialInterface::SIDEVICE_GC_GBA &&
             device != SerialInterface::SIDEVICE_GC_KEYBOARD)
      targets.push_back({Kind::GameCube, port});
  }
  if (Core::System::GetInstance().IsWii())
  {
    for (int index = 0; index < 4; ++index)
    {
      if (Config::Get(Config::GetInfoForWiimoteSource(index)) == WiimoteSource::Emulated)
        targets.push_back({Kind::WiiRemote, index});
    }
  }
  return targets;
}

std::string TasView::GetName(Target target)
{
  switch (target.kind)
  {
  case Kind::GameCube:
    return fmt::format("GameCube controller {}", target.index + 1);
  case Kind::WiiRemote:
    return fmt::format("Wii Remote {}", target.index + 1);
  case Kind::GBA:
    return fmt::format("GBA {}", target.index + 1);
  }
  return {};
}

void TasView::Open(Target target)
{
  Close();
  m_target = target;
  m_shared = std::make_shared<TasControls>();

  const auto lock = ControllerEmu::EmulatedController::GetStateLock();
  bool motion_plus = false;
  if (target.kind == Kind::WiiRemote)
  {
    WiimoteEmu::Wiimote* const wiimote = GetWiimote(target.index);
    m_shared->extension = wiimote->GetActiveExtensionNumber();
    motion_plus = wiimote->GetMotionPlusSetting().GetValue();
  }
  m_shared->controls = BuildControls(target, m_shared->extension, motion_plus);
  for (const Control& control : m_shared->controls)
    m_shared->values.push_back(control.initial);

  switch (target.kind)
  {
  case Kind::GameCube:
    Pad::GetConfig()
        ->GetController(target.index)
        ->SetInputOverrideFunction(MakeOverride(m_shared, 0));
    break;
  case Kind::GBA:
    Pad::GetGBAConfig()
        ->GetController(target.index)
        ->SetInputOverrideFunction(MakeOverride(m_shared, 0));
    break;
  case Kind::WiiRemote:
  {
    WiimoteEmu::Wiimote* const wiimote = GetWiimote(target.index);
    if (m_shared->extension != WiimoteEmu::ExtensionNumber::CLASSIC)
      wiimote->SetInputOverrideFunction(MakeOverride(m_shared, 0));
    if (m_shared->extension == WiimoteEmu::ExtensionNumber::NUNCHUK ||
        m_shared->extension == WiimoteEmu::ExtensionNumber::CLASSIC)
    {
      GetExtension(wiimote, m_shared->extension)
          ->SetInputOverrideFunction(MakeOverride(m_shared, 1));
    }
    break;
  }
  }

  m_selected = 0;
  while (m_selected < m_shared->controls.size() &&
         m_shared->controls[m_selected].type == ControlType::Heading)
  {
    ++m_selected;
  }
  m_open = true;
  m_changed = true;
}

void TasView::Close()
{
  if (!m_open)
    return;
  m_open = false;
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    switch (m_target.kind)
    {
    case Kind::GameCube:
      Pad::GetConfig()->GetController(m_target.index)->ClearInputOverrideFunction();
      break;
    case Kind::GBA:
      Pad::GetGBAConfig()->GetController(m_target.index)->ClearInputOverrideFunction();
      break;
    case Kind::WiiRemote:
    {
      WiimoteEmu::Wiimote* const wiimote = GetWiimote(m_target.index);
      wiimote->ClearInputOverrideFunction();
      if (m_shared->extension != WiimoteEmu::ExtensionNumber::NONE)
        GetExtension(wiimote, m_shared->extension)->ClearInputOverrideFunction();
      break;
    }
    }
  }
  // The GameCube controllers and GBAs may have players' controls switched between them.
  if (m_target.kind != Kind::WiiRemote)
    GBAControls::Reapply();
  m_shared.reset();
}

void TasView::Adjust(int delta)
{
  std::lock_guard lock(m_shared->mutex);
  const Control& control = m_shared->controls[m_selected];
  if (control.type != ControlType::Axis && control.type != ControlType::Scaled)
    return;
  // Wide ranges such as the gyroscope move in bigger steps.
  const int range = control.maximum - control.minimum;
  const int step = std::abs(delta) > 1 ? std::max(16, range / 64) : std::max(1, range / 1024);
  int& value = m_shared->values[m_selected];
  value = std::clamp(value + (delta < 0 ? -step : step), control.minimum, control.maximum);
  m_changed = true;
}

TasView::Effect TasView::HandleInput(const Frontend::FrameInput& input)
{
  if (!m_open)
    return Effect::Close;
  if (input.back)
    return Effect::Close;

  const size_t count = m_shared->controls.size();
  if (input.up || input.down)
  {
    size_t next = m_selected;
    do
    {
      next = (next + (input.down ? 1 : count - 1)) % count;
    } while (next != m_selected && m_shared->controls[next].type == ControlType::Heading);
    m_selected = next;
    m_changed = true;
  }
  if (input.left || input.right)
    Adjust(input.left ? -1 : 1);
  if (input.leftShoulder || input.rightShoulder)
    Adjust(input.leftShoulder ? -16 : 16);

  if (input.accept || input.context)
  {
    std::lock_guard lock(m_shared->mutex);
    const Control& control = m_shared->controls[m_selected];
    int& value = m_shared->values[m_selected];
    if (control.type == ControlType::Button)
      value = input.context ? (value == 2 ? 0 : 2) : (value == 1 ? 0 : 1);
    else if (input.accept)
      value = control.initial;
    m_changed = true;
  }

  auto& system = Core::System::GetInstance();
  if (input.view)
  {
    Core::DoFrameStep(system);
    m_changed = true;
  }
  if (input.menu)
  {
    Core::SetState(system, Core::GetState(system) == Core::State::Paused ? Core::State::Running :
                                                                           Core::State::Paused);
    m_changed = true;
  }
  return Effect::None;
}

bool TasView::ConsumeChanged()
{
  return std::exchange(m_changed, false);
}

TasSnapshot TasView::BuildSnapshot() const
{
  TasSnapshot snapshot;
  if (!m_open)
    return snapshot;
  auto& system = Core::System::GetInstance();
  snapshot.title = fmt::format("TAS input: {}", GetName(m_target));
  snapshot.status = Core::GetState(system) == Core::State::Paused ?
                        fmt::format("Paused at frame {}", system.GetMovie().GetCurrentFrame()) :
                        std::string("Running");
  snapshot.selected = m_selected;
  snapshot.hint = "Left/Right Change   LB/RB Change more   A Hold or reset   X Turbo\n"
                  "View Next frame   Menu Play or pause   B Close";

  std::lock_guard lock(m_shared->mutex);
  for (size_t i = 0; i < m_shared->controls.size(); ++i)
  {
    const Control& control = m_shared->controls[i];
    const int value = m_shared->values[i];
    TasSnapshot::Row row;
    row.label = control.label;
    switch (control.type)
    {
    case ControlType::Heading:
      row.heading = true;
      break;
    case ControlType::Button:
      row.value = value == 2 ? "Turbo" : (value == 1 ? "Held" : "Released");
      break;
    default:
      row.value = std::to_string(value);
      row.fraction = static_cast<float>(value - control.minimum) /
                     static_cast<float>(std::max(1, control.maximum - control.minimum));
      break;
    }
    snapshot.rows.push_back(std::move(row));
  }
  return snapshot;
}

void TasView::Draw(const TasSnapshot& snapshot, const Frontend::ShellTheme& theme, ImFont* font)
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  if (!viewport || snapshot.rows.empty())
    return;
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoBringToFrontOnFocus;
  if (!ImGui::Begin("DolphinTasInput", nullptr, flags))
  {
    ImGui::End();
    return;
  }
  if (font)
    ImGui::PushFont(font);

  // The panel keeps to the right, so the game stays in view.
  ImDrawList* const draw = ImGui::GetWindowDrawList();
  const float margin = std::max(20.0f, viewport->WorkSize.x * 0.02f);
  const float width = std::max(420.0f, viewport->WorkSize.x * 0.34f);
  const ImVec2 panel_min{viewport->WorkPos.x + viewport->WorkSize.x - margin - width,
                         viewport->WorkPos.y + margin};
  const ImVec2 panel_max{viewport->WorkPos.x + viewport->WorkSize.x - margin,
                         viewport->WorkPos.y + viewport->WorkSize.y - margin};
  Frontend::DrawShellPanelPx(draw, panel_min, panel_max, theme, 22.0f, 0.92f);

  const float line = ImGui::GetTextLineHeight();
  const float left = panel_min.x + 24.0f;
  const float right = panel_max.x - 24.0f;
  const ImU32 primary = Color(theme.textPrimary);
  const ImU32 secondary = Color(theme.textSecondary);
  const ImU32 accent = Color(theme.cursorNormal);

  float y = panel_min.y + 20.0f;
  Frontend::DrawClippedText(draw, {left, y}, {right, y + line}, primary, snapshot.title.c_str());
  y += line + 6.0f;
  draw->AddText({left, y}, accent, snapshot.status.c_str());
  y += line + 16.0f;

  const float hint_height =
      ImGui::CalcTextSize(snapshot.hint.c_str(), nullptr, false, right - left).y;
  const float bottom = panel_max.y - hint_height - 28.0f;
  const float row_height = line + 10.0f;
  const size_t visible =
      std::min(PANEL_ROWS, static_cast<size_t>(std::max(1.0f, (bottom - y) / row_height)));
  const size_t first = snapshot.selected >= visible / 2 ? snapshot.selected - visible / 2 : 0;
  const size_t start =
      std::min(first, snapshot.rows.size() > visible ? snapshot.rows.size() - visible : size_t{0});
  for (size_t i = start; i < std::min(snapshot.rows.size(), start + visible); ++i)
  {
    const TasSnapshot::Row& row = snapshot.rows[i];
    const float row_y = y + static_cast<float>(i - start) * row_height;
    if (row.heading)
    {
      draw->AddText({left, row_y + 2.0f}, accent, row.label.c_str());
      continue;
    }
    if (i == snapshot.selected)
    {
      Frontend::DrawReplicaFocus(draw, {left - 8.0f, row_y - 3.0f},
                                 {right + 8.0f, row_y + row_height - 5.0f}, 8.0f, true, false);
    }
    draw->AddText({left + 12.0f, row_y}, primary, row.label.c_str());
    const float value_width = ImGui::CalcTextSize(row.value.c_str()).x;
    draw->AddText({right - value_width, row_y}, row.value == "Released" ? secondary : primary,
                  row.value.c_str());
    if (row.fraction >= 0.0f)
    {
      const float bar_left = left + (right - left) * 0.45f;
      const float bar_right = right - ImGui::CalcTextSize("000000").x - 12.0f;
      const float bar_y = row_y + line * 0.5f;
      draw->AddRectFilled({bar_left, bar_y - 2.0f}, {bar_right, bar_y + 2.0f},
                          Frontend::ColorWithAlpha(theme.textSecondary, 0.35f), 2.0f);
      const float x = bar_left + (bar_right - bar_left) * std::clamp(row.fraction, 0.0f, 1.0f);
      draw->AddCircleFilled({x, bar_y}, 6.0f, accent);
    }
  }

  draw->PushClipRect(panel_min, panel_max, true);
  ImGui::SetCursorScreenPos({left, panel_max.y - hint_height - 18.0f});
  ImGui::PushTextWrapPos(right);
  ImGui::PushStyleColor(ImGuiCol_Text, theme.textSecondary);
  ImGui::TextUnformatted(snapshot.hint.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  draw->PopClipRect();

  if (font)
    ImGui::PopFont();
  ImGui::End();
}
}  // namespace DolphinUWP

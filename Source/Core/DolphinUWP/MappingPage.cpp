// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/MappingPage.h"

#include <algorithm>
#include <utility>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/Widgets.h>

#include "DolphinUWP/ShellWidgets.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/ControllerEmu/Control/Control.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

constexpr auto CAPTURE_DELAY = std::chrono::milliseconds(150);
constexpr auto DETECT_INITIAL_TIME = std::chrono::seconds(3);
constexpr auto DETECT_CONFIRMATION_TIME = std::chrono::milliseconds(0);
constexpr auto DETECT_MAXIMUM_TIME = std::chrono::seconds(5);

// Shows control names without backticks or the qualifier of the default device.
std::string FormatExpression(std::string_view expression, const std::string& default_device)
{
  std::string result;
  for (size_t i = 0; i < expression.size();)
  {
    if (expression[i] != '`')
    {
      result += expression[i++];
      continue;
    }

    const size_t end = expression.find('`', i + 1);
    if (end == std::string_view::npos)
    {
      result.append(expression.substr(i + 1));
      break;
    }

    std::string_view token = expression.substr(i + 1, end - i - 1);
    const size_t colon = token.find(':');
    if (colon != std::string_view::npos && token.substr(0, colon) == default_device)
      token.remove_prefix(colon + 1);
    result.append(token);
    i = end + 1;
  }
  return result;
}
}  // namespace

MappingPage::MappingPage() = default;
MappingPage::~MappingPage() = default;

void MappingPage::Open(InputConfig* config, std::vector<Tab> tabs,
                       std::vector<std::string> hidden_controls)
{
  Close();
  m_config = config;
  m_controller = config ? config->GetController(0) : nullptr;
  if (!m_controller)
  {
    m_config = nullptr;
    return;
  }
  m_tabs = std::move(tabs);
  m_hidden_controls = std::move(hidden_controls);
  m_view = {};
  m_view.State().RequestFocus(Frontend::SplitPane::Navigation);
}

void MappingPage::Close()
{
  m_detector.reset();
  m_capture_target = nullptr;
  m_config = nullptr;
  m_controller = nullptr;
  m_tabs.clear();
}

bool MappingPage::IsCapturing() const
{
  return m_capture_target != nullptr;
}

void MappingPage::Update()
{
  if (!m_capture_target)
    return;

  if (!m_detector)
  {
    if (std::chrono::steady_clock::now() < m_capture_start)
      return;
    m_detector = std::make_unique<ciface::Core::InputDetector>();
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    m_detector->Start(g_controller_interface, g_controller_interface.GetAllDeviceStrings());
    return;
  }

  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::Host);
  g_controller_interface.UpdateInput();
  m_detector->Update(DETECT_INITIAL_TIME, DETECT_CONFIRMATION_TIME, DETECT_MAXIMUM_TIME);
  if (m_detector->IsComplete())
    FinishCapture();
}

bool MappingPage::Draw(float controller_scroll, bool clear_pressed)
{
  if (!IsOpen())
    return true;

  m_clear_pressed = clear_pressed && !IsCapturing();

  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = IsCapturing() ? 0.0f : controller_scroll;
  model.pages.push_back({"device", "Device", true, [this] { DrawDeviceTab(); }});
  for (const Tab& tab : m_tabs)
  {
    model.pages.push_back({tab.id, tab.label, true, [this, &tab] {
                             for (ControllerEmu::ControlGroup* group : tab.groups)
                               DrawGroup(group);
                           }});
  }

  const Frontend::GraphicsViewResult result = m_view.Draw(model, ImGui::GetContentRegionAvail());
  return result.closeRequested && !IsCapturing();
}

void MappingPage::DrawDeviceTab()
{
  Frontend::SectionTitle("Device");
  const std::vector<std::string> devices = g_controller_interface.GetAllDeviceStrings();
  const std::string current = m_controller->GetDefaultDevice().ToString();

  std::vector<Frontend::ChoiceItem> items;
  size_t selected = 0;
  if (std::ranges::find(devices, current) == devices.end())
    items.push_back({current, fmt::format("{} (not connected)", current)});
  for (const std::string& device : devices)
  {
    if (device == current)
      selected = items.size();
    items.push_back({device, device});
  }

  if (Frontend::ShellCombo("Default device", &selected, items) && selected < items.size())
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    m_controller->SetDefaultDevice(items[selected].id);
    m_controller->UpdateReferences(g_controller_interface);
    Save();
  }

  Frontend::SectionTitle("Bindings");
  if (Frontend::ShellButton("Reset all to defaults"))
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    m_controller->LoadDefaults(g_controller_interface);
    m_controller->UpdateReferences(g_controller_interface);
    Save();
  }
  if (Frontend::ShellButton("Clear all bindings"))
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    for (const Tab& tab : m_tabs)
    {
      for (ControllerEmu::ControlGroup* group : tab.groups)
      {
        for (const auto& control : group->controls)
          control->control_ref->SetExpression("");
      }
    }
    m_controller->UpdateReferences(g_controller_interface);
    Save();
  }
  ShellWidgets::HintText("Select a binding, then press the input to assign within five seconds. "
                         "Press X on a binding to clear it.");
}

void MappingPage::DrawGroup(ControllerEmu::ControlGroup* group)
{
  Frontend::SectionTitle(group->ui_name.c_str());
  const std::string default_device = m_controller->GetDefaultDevice().ToString();

  for (const auto& control : group->controls)
  {
    if (std::ranges::find(m_hidden_controls, control->name) != m_hidden_controls.end())
      continue;

    ControlReference* const reference = control->control_ref.get();
    const bool capturing = m_capture_target == reference;
    std::string value = capturing ? "Press an input..." :
                                    FormatExpression(reference->GetExpression(), default_device);
    if (value.empty())
      value = "Not set";

    ImGui::PushID(control.get());
    if (ShellWidgets::ValueRow(control->name.c_str(), control->ui_name, value, capturing) &&
        !IsCapturing())
    {
      StartCapture(reference);
    }
    if (m_clear_pressed && ImGui::IsItemFocused())
      SetExpression(reference, "");
    ImGui::PopID();
  }

  for (const auto& setting : group->numeric_settings)
  {
    if (!setting->IsSimpleValue())
      continue;

    ImGui::PushID(setting.get());
    switch (setting->GetType())
    {
    case ControllerEmu::SettingType::Bool:
    {
      auto* const typed = static_cast<ControllerEmu::NumericSetting<bool>*>(setting.get());
      bool value = typed->GetValue();
      if (Frontend::ShellCheckbox(typed->GetUIName(), &value))
      {
        typed->SetValue(value);
        Save();
      }
      break;
    }
    case ControllerEmu::SettingType::Double:
    {
      auto* const typed = static_cast<ControllerEmu::NumericSetting<double>*>(setting.get());
      float value = static_cast<float>(typed->GetValue());
      std::string format = "%.2f";
      for (const char* suffix = typed->GetUISuffix(); suffix && *suffix; ++suffix)
        format += *suffix == '%' ? "%%" : std::string(1, *suffix);
      if (Frontend::ShellSliderFloat(typed->GetUIName(), &value,
                                     static_cast<float>(typed->GetMinValue()),
                                     static_cast<float>(typed->GetMaxValue()), format.c_str()))
      {
        typed->SetValue(value);
        Save();
      }
      break;
    }
    case ControllerEmu::SettingType::Int:
    {
      auto* const typed = static_cast<ControllerEmu::NumericSetting<int>*>(setting.get());
      int value = typed->GetValue();
      if (Frontend::ShellSliderInt(typed->GetUIName(), &value, typed->GetMinValue(),
                                   typed->GetMaxValue()))
      {
        typed->SetValue(value);
        Save();
      }
      break;
    }
    }
    ImGui::PopID();
  }
}

void MappingPage::StartCapture(ControlReference* reference)
{
  m_detector.reset();
  m_capture_target = reference;
  m_capture_start = std::chrono::steady_clock::now() + CAPTURE_DELAY;
}

void MappingPage::FinishCapture()
{
  ciface::Core::InputDetector::Results results = m_detector->TakeResults();
  m_detector.reset();
  ControlReference* const target = std::exchange(m_capture_target, nullptr);
  if (!target || !ciface::MappingCommon::ContainsCompleteDetection(results))
    return;

  ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&results);
  SetExpression(target,
                ciface::MappingCommon::BuildExpression(results, m_controller->GetDefaultDevice(),
                                                       ciface::MappingCommon::Quote::On));
}

void MappingPage::SetExpression(ControlReference* reference, const std::string& expression)
{
  {
    const auto lock = ControllerEmu::EmulatedController::GetStateLock();
    reference->SetExpression(expression);
    m_controller->UpdateSingleControlReference(g_controller_interface, reference);
  }
  Save();
}

void MappingPage::Save()
{
  if (m_config)
    m_config->SaveConfig();
}
}  // namespace DolphinUWP

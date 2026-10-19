// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <UwpImGuiFrontend/GraphicsView.h>

class ControlReference;
class InputConfig;

namespace ControllerEmu
{
class ControlGroup;
class EmulatedController;
}  // namespace ControllerEmu

namespace ciface::Core
{
class InputDetector;
}

namespace DolphinUWP
{
// Maps the controls of a single emulated controller, such as the hotkeys or Free Look.
class MappingPage
{
public:
  struct Tab
  {
    std::string id;
    std::string label;
    std::vector<ControllerEmu::ControlGroup*> groups;
  };

  MappingPage();
  ~MappingPage();

  // Controls named in hidden_controls are not shown.
  void Open(InputConfig* config, std::vector<Tab> tabs,
            std::vector<std::string> hidden_controls = {});
  void Close();
  bool IsOpen() const { return m_config != nullptr; }
  bool IsCapturing() const;

  // Call once per frame on the UI thread.
  void Update();

  // Returns true when the page asks to close. Clearing applies to the focused binding.
  bool Draw(float controller_scroll, bool clear_pressed);

private:
  void DrawDeviceTab();
  void DrawGroup(ControllerEmu::ControlGroup* group);
  void StartCapture(ControlReference* reference);
  void FinishCapture();
  void SetExpression(ControlReference* reference, const std::string& expression);
  void Save();

  InputConfig* m_config = nullptr;
  ControllerEmu::EmulatedController* m_controller = nullptr;
  std::vector<Tab> m_tabs;
  std::vector<std::string> m_hidden_controls;
  UwpImGuiFrontend::GraphicsView m_view;

  bool m_clear_pressed = false;
  ControlReference* m_capture_target = nullptr;
  std::chrono::steady_clock::time_point m_capture_start;
  std::unique_ptr<ciface::Core::InputDetector> m_detector;
};
}  // namespace DolphinUWP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>

#include <winrt/Windows.System.h>

#include <UwpImGuiFrontend/Input.h>

namespace DolphinUWP
{
// Collects CoreWindow keyboard/pointer events and Windows.Gaming.Input gamepads on the UI thread.
class InputSource
{
public:
  void SetKey(winrt::Windows::System::VirtualKey key, bool pressed);
  void SetPointer(std::optional<UwpImGuiFrontend::Vec2> position, bool pressed);
  void AddWheel(float delta);
  void RequestBack();
  void Reset();

  UwpImGuiFrontend::FrameInput Read(float delta_seconds);
  float TakeWheel();

  bool ConsumeQuickMenuRequest();
  bool IsAnyInputHeld();
  bool IsGamepadConnected() const;

private:
  bool IsKeyDown(winrt::Windows::System::VirtualKey key) const;

  std::array<bool, 256> m_keys{};
  std::optional<UwpImGuiFrontend::Vec2> m_pointer;
  bool m_pointer_pressed = false;
  float m_wheel = 0.0f;
  bool m_back_requested = false;
  bool m_quick_menu_held = false;
};
}  // namespace DolphinUWP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/InputSource.h"

#include <cmath>
#include <utility>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>

#include "Common/CommonTypes.h"

namespace DolphinUWP
{
namespace
{
namespace GamingInput = winrt::Windows::Gaming::Input;
using winrt::Windows::System::VirtualKey;

constexpr float STICK_ACTIVE_THRESHOLD = 0.2f;

bool HasButton(GamingInput::GamepadButtons buttons, GamingInput::GamepadButtons button)
{
  return (static_cast<u32>(buttons) & static_cast<u32>(button)) != 0;
}

std::optional<GamingInput::GamepadReading> ReadFirstGamepad()
{
  try
  {
    const auto gamepads = GamingInput::Gamepad::Gamepads();
    if (gamepads.Size() == 0)
      return std::nullopt;
    return gamepads.GetAt(0).GetCurrentReading();
  }
  catch (const winrt::hresult_error&)
  {
    return std::nullopt;
  }
}
}  // namespace

void InputSource::SetKey(VirtualKey key, bool pressed)
{
  const auto index = static_cast<size_t>(key);
  if (index < m_keys.size())
    m_keys[index] = pressed;
}

void InputSource::SetPointer(std::optional<UwpImGuiFrontend::Vec2> position, bool pressed)
{
  m_pointer = position;
  m_pointer_pressed = pressed;
}

void InputSource::AddWheel(float delta)
{
  m_wheel += delta;
}

void InputSource::RequestBack()
{
  m_back_requested = true;
}

void InputSource::Reset()
{
  m_keys.fill(false);
  m_pointer.reset();
  m_pointer_pressed = false;
  m_wheel = 0.0f;
  m_back_requested = false;
}

bool InputSource::IsKeyDown(VirtualKey key) const
{
  const auto index = static_cast<size_t>(key);
  return index < m_keys.size() && m_keys[index];
}

UwpImGuiFrontend::FrameInput InputSource::Read(float delta_seconds)
{
  UwpImGuiFrontend::FrameInput input;
  input.deltaSeconds = delta_seconds;
  input.up = IsKeyDown(VirtualKey::Up) || IsKeyDown(VirtualKey::W);
  input.down = IsKeyDown(VirtualKey::Down) || IsKeyDown(VirtualKey::S);
  input.left = IsKeyDown(VirtualKey::Left) || IsKeyDown(VirtualKey::A);
  input.right = IsKeyDown(VirtualKey::Right) || IsKeyDown(VirtualKey::D);
  input.accept = IsKeyDown(VirtualKey::Enter) || IsKeyDown(VirtualKey::Space);
  input.back = std::exchange(m_back_requested, false) || IsKeyDown(VirtualKey::Escape) ||
               IsKeyDown(VirtualKey::Back);
  input.context = IsKeyDown(VirtualKey::X);
  input.alternate = IsKeyDown(VirtualKey::Y);
  input.menu = IsKeyDown(VirtualKey::F1);
  input.view = IsKeyDown(VirtualKey::Tab);
  input.leftShoulder = IsKeyDown(VirtualKey::Q);
  input.rightShoulder = IsKeyDown(VirtualKey::E);
  input.pointer = m_pointer;
  input.pointerPressed = m_pointer_pressed;

  if (const auto reading = ReadFirstGamepad())
  {
    const auto buttons = reading->Buttons;
    input.up |= HasButton(buttons, GamingInput::GamepadButtons::DPadUp);
    input.down |= HasButton(buttons, GamingInput::GamepadButtons::DPadDown);
    input.left |= HasButton(buttons, GamingInput::GamepadButtons::DPadLeft);
    input.right |= HasButton(buttons, GamingInput::GamepadButtons::DPadRight);
    input.accept |= HasButton(buttons, GamingInput::GamepadButtons::A);
    input.back |= HasButton(buttons, GamingInput::GamepadButtons::B);
    input.context |= HasButton(buttons, GamingInput::GamepadButtons::X);
    input.alternate |= HasButton(buttons, GamingInput::GamepadButtons::Y);
    input.menu |= HasButton(buttons, GamingInput::GamepadButtons::Menu);
    input.view |= HasButton(buttons, GamingInput::GamepadButtons::View);
    input.leftShoulder |= HasButton(buttons, GamingInput::GamepadButtons::LeftShoulder);
    input.rightShoulder |= HasButton(buttons, GamingInput::GamepadButtons::RightShoulder);
    input.leftStickX = static_cast<float>(reading->LeftThumbstickX);
    input.leftStickY = static_cast<float>(reading->LeftThumbstickY);
    input.rightStickX = static_cast<float>(reading->RightThumbstickX);
    input.rightStickY = static_cast<float>(reading->RightThumbstickY);
    input.controllerInputHeld = static_cast<u32>(buttons) != 0 ||
                                std::abs(input.leftStickX) > STICK_ACTIVE_THRESHOLD ||
                                std::abs(input.leftStickY) > STICK_ACTIVE_THRESHOLD ||
                                std::abs(input.rightStickX) > STICK_ACTIVE_THRESHOLD ||
                                std::abs(input.rightStickY) > STICK_ACTIVE_THRESHOLD ||
                                reading->LeftTrigger > STICK_ACTIVE_THRESHOLD ||
                                reading->RightTrigger > STICK_ACTIVE_THRESHOLD;
  }

  return input;
}

float InputSource::TakeWheel()
{
  return std::exchange(m_wheel, 0.0f);
}

bool InputSource::ConsumeQuickMenuRequest()
{
  bool chord = IsKeyDown(VirtualKey::Escape);
  try
  {
    for (const auto& gamepad : GamingInput::Gamepad::Gamepads())
    {
      const auto buttons = gamepad.GetCurrentReading().Buttons;
      chord |= HasButton(buttons, GamingInput::GamepadButtons::Menu) &&
               HasButton(buttons, GamingInput::GamepadButtons::View);
    }
  }
  catch (const winrt::hresult_error&)
  {
  }

  const bool pressed = chord && !m_quick_menu_held;
  m_quick_menu_held = chord;
  return pressed;
}

bool InputSource::IsAnyInputHeld()
{
  for (const bool key : m_keys)
  {
    if (key)
      return true;
  }

  const auto reading = ReadFirstGamepad();
  return reading && (static_cast<u32>(reading->Buttons) != 0 ||
                     reading->LeftTrigger > STICK_ACTIVE_THRESHOLD ||
                     reading->RightTrigger > STICK_ACTIVE_THRESHOLD);
}

bool InputSource::IsGamepadConnected() const
{
  try
  {
    return GamingInput::Gamepad::Gamepads().Size() > 0;
  }
  catch (const winrt::hresult_error&)
  {
    return false;
  }
}
}  // namespace DolphinUWP

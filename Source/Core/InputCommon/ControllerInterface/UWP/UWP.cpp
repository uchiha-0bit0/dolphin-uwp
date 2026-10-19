// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/ControllerInterface/UWP/UWP.h"

#include <array>
#include <atomic>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <fmt/format.h>

#include "Common/Matrix.h"
#include "Core/Host.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/WGInput/WGInput.h"

namespace ciface::UWP
{
static std::array<std::atomic<bool>, 256> s_key_states{};

static std::atomic<float> s_cursor_x = 0.0f;
static std::atomic<float> s_cursor_y = 0.0f;
static std::atomic<u32> s_mouse_buttons = 0;
static std::atomic<bool> s_center_requested = false;
static std::mutex s_movement_mutex;
static Common::Vec3 s_movement;

namespace
{
struct NamedKey
{
  const char* name;
  std::vector<u8> keys;
};

std::vector<NamedKey> BuildKeyList()
{
  std::vector<NamedKey> list;
  for (char c = 'A'; c <= 'Z'; ++c)
    list.push_back({nullptr, {static_cast<u8>(c)}});
  for (char c = '0'; c <= '9'; ++c)
    list.push_back({nullptr, {static_cast<u8>(c)}});

  const auto add = [&list](const char* name, std::initializer_list<u8> keys) {
    list.push_back({name, keys});
  };

  add("UP", {VK_UP});
  add("DOWN", {VK_DOWN});
  add("LEFT", {VK_LEFT});
  add("RIGHT", {VK_RIGHT});
  add("RETURN", {VK_RETURN});
  add("ESCAPE", {VK_ESCAPE});
  add("BACK", {VK_BACK});
  add("TAB", {VK_TAB});
  add("SPACE", {VK_SPACE});
  add("INSERT", {VK_INSERT});
  add("DELETE", {VK_DELETE});
  add("HOME", {VK_HOME});
  add("END", {VK_END});
  add("PRIOR", {VK_PRIOR});
  add("NEXT", {VK_NEXT});
  add("CAPITAL", {VK_CAPITAL});
  add("PAUSE", {VK_PAUSE});
  add("NUMLOCK", {VK_NUMLOCK});
  add("SCROLL", {VK_SCROLL});
  add("SEMICOLON", {VK_OEM_1});
  add("EQUALS", {VK_OEM_PLUS});
  add("COMMA", {VK_OEM_COMMA});
  add("MINUS", {VK_OEM_MINUS});
  add("PERIOD", {VK_OEM_PERIOD});
  add("SLASH", {VK_OEM_2});
  add("GRAVE", {VK_OEM_3});
  add("LBRACKET", {VK_OEM_4});
  add("BACKSLASH", {VK_OEM_5});
  add("RBRACKET", {VK_OEM_6});
  add("APOSTROPHE", {VK_OEM_7});
  add("MULTIPLY", {VK_MULTIPLY});
  add("ADD", {VK_ADD});
  add("SUBTRACT", {VK_SUBTRACT});
  add("DECIMAL", {VK_DECIMAL});
  add("DIVIDE", {VK_DIVIDE});
  add("LSHIFT", {VK_LSHIFT});
  add("RSHIFT", {VK_RSHIFT});
  add("LCONTROL", {VK_LCONTROL});
  add("RCONTROL", {VK_RCONTROL});
  add("LMENU", {VK_LMENU});
  add("RMENU", {VK_RMENU});
  add("Shift", {VK_SHIFT, VK_LSHIFT, VK_RSHIFT});
  add("Ctrl", {VK_CONTROL, VK_LCONTROL, VK_RCONTROL});
  add("Alt", {VK_MENU, VK_LMENU, VK_RMENU});

  static constexpr std::array numpad_names = {"NUMPAD0", "NUMPAD1", "NUMPAD2", "NUMPAD3",
                                              "NUMPAD4", "NUMPAD5", "NUMPAD6", "NUMPAD7",
                                              "NUMPAD8", "NUMPAD9"};
  for (u8 i = 0; i < numpad_names.size(); ++i)
    add(numpad_names[i], {static_cast<u8>(VK_NUMPAD0 + i)});

  static constexpr std::array function_names = {
      "F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",  "F10", "F11", "F12",
      "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21", "F22", "F23", "F24"};
  for (u8 i = 0; i < function_names.size(); ++i)
    add(function_names[i], {static_cast<u8>(VK_F1 + i)});

  return list;
}

class Key final : public Core::Device::Input
{
public:
  explicit Key(NamedKey key) : m_key(std::move(key))
  {
    if (!m_key.name)
      m_name = std::string(1, static_cast<char>(m_key.keys.front()));
    else
      m_name = m_key.name;
  }

  std::string GetName() const override { return m_name; }

  ControlState GetState() const override
  {
    for (const u8 key : m_key.keys)
    {
      if (s_key_states[key].load(std::memory_order_relaxed))
        return 1.0;
    }
    return 0.0;
  }

private:
  NamedKey m_key;
  std::string m_name;
};

class Keyboard final : public Core::Device
{
public:
  Keyboard()
  {
    for (NamedKey& key : BuildKeyList())
      AddInput(new Key(std::move(key)));
  }

  std::string GetName() const override { return "Keyboard"; }
  std::string GetSource() const override { return "UWP"; }
  int GetSortPriority() const override { return 5; }
};

// Lower is more sensitive. Matches the desktop mouse axes.
constexpr float MOUSE_AXIS_SENSITIVITY = 8.0f;
constexpr u8 MOUSE_BUTTON_COUNT = 5;
constexpr u8 MOUSE_AXIS_COUNT = 3;

class Mouse final : public Core::Device
{
public:
  Mouse()
  {
    for (u8 i = 0; i < MOUSE_BUTTON_COUNT; ++i)
      AddInput(new Button(*this, i));
    for (u8 i = 0; i < MOUSE_AXIS_COUNT; ++i)
    {
      AddInput(new Axis(*this, i, false));
      AddInput(new Axis(*this, i, true));
    }
    for (u8 i = 0; i < 2; ++i)
    {
      AddInput(new Cursor(*this, i, false));
      AddInput(new Cursor(*this, i, true));
    }
    for (u8 i = 0; i < MOUSE_AXIS_COUNT; ++i)
    {
      AddInput(new RelativeAxis(*this, i, false));
      AddInput(new RelativeAxis(*this, i, true));
    }
  }

  std::string GetName() const override { return "Mouse"; }
  std::string GetSource() const override { return "UWP"; }
  int GetSortPriority() const override { return 4; }

  Core::DeviceRemoval UpdateInput() override
  {
    if (g_controller_interface.IsMouseCenteringRequested() && Host_RendererHasFocus())
    {
      s_cursor_x = 0.0f;
      s_cursor_y = 0.0f;
      s_center_requested = true;
      g_controller_interface.SetMouseCenteringRequested(false);
    }

    Common::Vec3 movement;
    {
      std::lock_guard lock(s_movement_mutex);
      movement = std::exchange(s_movement, Common::Vec3{});
    }
    m_relative.Move(movement);
    m_relative.Update();
    for (u8 i = 0; i < MOUSE_AXIS_COUNT; ++i)
      m_axes[i] = (m_axes[i] + movement.data[i]) / 2;

    m_buttons = s_mouse_buttons.load(std::memory_order_relaxed);
    const Common::Vec2 scale = g_controller_interface.GetWindowInputScale();
    m_cursor = {s_cursor_x.load(std::memory_order_relaxed) * scale.x,
                s_cursor_y.load(std::memory_order_relaxed) * scale.y};
    return Core::DeviceRemoval::Keep;
  }

private:
  class Button final : public Input
  {
  public:
    Button(const Mouse& mouse, u8 index) : m_mouse(mouse), m_index(index) {}
    std::string GetName() const override { return fmt::format("Click {}", m_index); }
    ControlState GetState() const override { return (m_mouse.m_buttons >> m_index) & 1; }

  private:
    const Mouse& m_mouse;
    const u8 m_index;
  };

  // Mouse movement since the last update, including the wheel.
  class Axis final : public Input
  {
  public:
    Axis(const Mouse& mouse, u8 index, bool positive)
        : m_mouse(mouse), m_index(index), m_positive(positive)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("Axis {}{}", char('X' + m_index), m_positive ? '+' : '-');
    }
    bool IsDetectable() const override { return false; }
    ControlState GetState() const override
    {
      const float range = m_index == 2 ? 1.0f : MOUSE_AXIS_SENSITIVITY;
      return m_mouse.m_axes[m_index] / (m_positive ? range : -range);
    }

  private:
    const Mouse& m_mouse;
    const u8 m_index;
    const bool m_positive;
  };

  // Cursor position relative to the center of the window.
  class Cursor final : public Input
  {
  public:
    Cursor(const Mouse& mouse, u8 index, bool positive)
        : m_mouse(mouse), m_index(index), m_positive(positive)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("Cursor {}{}", char('X' + m_index), m_positive ? '+' : '-');
    }
    ControlState GetState() const override
    {
      return m_mouse.m_cursor.data[m_index] * (m_positive ? 1.0f : -1.0f);
    }

  private:
    const Mouse& m_mouse;
    const u8 m_index;
    const bool m_positive;
  };

  class RelativeAxis final : public RelativeInput
  {
  public:
    RelativeAxis(const Mouse& mouse, u8 index, bool positive)
        : m_mouse(mouse), m_index(index), m_positive(positive)
    {
    }
    std::string GetName() const override
    {
      return fmt::format("RelativeMouse {}{}", char('X' + m_index), m_positive ? '+' : '-');
    }
    ControlState GetState() const override
    {
      return m_mouse.m_relative.GetValue().data[m_index] * (m_positive ? 1.0f : -1.0f);
    }

  private:
    const Mouse& m_mouse;
    const u8 m_index;
    const bool m_positive;
  };

  RelativeInputState<Common::Vec3> m_relative;
  std::array<float, MOUSE_AXIS_COUNT> m_axes{};
  Common::Vec2 m_cursor{};
  u32 m_buttons = 0;
};

class InputBackend final : public ciface::InputBackend
{
public:
  explicit InputBackend(ControllerInterface* controller_interface)
      : ciface::InputBackend(controller_interface)
  {
    WGInput::Init();
  }

  ~InputBackend() override { WGInput::DeInit(); }

  void PopulateDevices() override
  {
    GetControllerInterface().PlatformPopulateDevices([this] {
      GetControllerInterface().AddDevice(std::make_shared<Keyboard>());
      GetControllerInterface().AddDevice(std::make_shared<Mouse>());
      WGInput::PopulateDevices();
    });
  }
};
}  // namespace

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface)
{
  return std::make_unique<InputBackend>(controller_interface);
}

void SetKeyState(u32 virtual_key, bool pressed)
{
  if (virtual_key < s_key_states.size())
    s_key_states[virtual_key].store(pressed, std::memory_order_relaxed);
}

bool IsKeyPressed(u32 virtual_key)
{
  return virtual_key < s_key_states.size() &&
         s_key_states[virtual_key].load(std::memory_order_relaxed);
}

void ResetKeyStates()
{
  for (auto& state : s_key_states)
    state.store(false, std::memory_order_relaxed);
}

void SetCursorPosition(float x, float y)
{
  s_cursor_x.store(x, std::memory_order_relaxed);
  s_cursor_y.store(y, std::memory_order_relaxed);
}

void SetMouseButtons(u32 buttons)
{
  s_mouse_buttons.store(buttons, std::memory_order_relaxed);
}

void AddMouseMovement(float x, float y, float wheel)
{
  std::lock_guard lock(s_movement_mutex);
  s_movement += Common::Vec3{x, y, wheel};
}

void ResetMouseState()
{
  s_mouse_buttons.store(0, std::memory_order_relaxed);
  std::lock_guard lock(s_movement_mutex);
  s_movement = {};
}

bool ConsumeCursorCenterRequest()
{
  return s_center_requested.exchange(false);
}
}  // namespace ciface::UWP

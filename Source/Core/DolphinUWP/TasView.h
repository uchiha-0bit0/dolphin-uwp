// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <UwpImGuiFrontend/Input.h>

#include "Common/CommonTypes.h"

struct ImFont;

namespace UwpImGuiFrontend
{
struct ShellTheme;
}

namespace DolphinUWP
{
struct TasControls;

// What the TAS input panel shows. Built on the UI thread, drawn on the video thread.
struct TasSnapshot
{
  struct Row
  {
    std::string label;
    std::string value;
    // From 0 to 1 for an analog input, which shows as a bar.
    float fraction = -1.0f;
    bool heading = false;
  };

  std::string title;
  std::string status;
  std::vector<Row> rows;
  size_t selected = 0;
  std::string hint;
};

// While the panel is open, the controller follows its values instead of the real controllers.
class TasView
{
public:
  enum class Kind
  {
    GameCube,
    WiiRemote,
    GBA,
  };

  struct Target
  {
    Kind kind = Kind::GameCube;
    int index = 0;
  };

  enum class Effect
  {
    None,
    Close,
  };

  TasView();
  ~TasView();

  static std::vector<Target> GetTargets();
  static std::string GetName(Target target);

  void Open(Target target);
  void Close();
  bool IsOpen() const { return m_open; }
  Effect HandleInput(const UwpImGuiFrontend::FrameInput& input);
  bool ConsumeChanged();
  TasSnapshot BuildSnapshot() const;

  static void Draw(const TasSnapshot& snapshot, const UwpImGuiFrontend::ShellTheme& theme,
                   ImFont* font);

private:
  void Adjust(int delta);

  std::shared_ptr<TasControls> m_shared;
  Target m_target;
  size_t m_selected = 0;
  bool m_open = false;
  bool m_changed = true;
};
}  // namespace DolphinUWP

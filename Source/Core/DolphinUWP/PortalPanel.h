// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DolphinUWP/Portals.h"

namespace DolphinUWP
{
struct ShellServices;

class PortalPanel
{
public:
  enum class Kind
  {
    Skylanders,
    Infinity,
  };

  explicit PortalPanel(Kind kind) : m_kind(kind) {}

  void Draw(const ShellServices& services);

private:
  enum class Dialog
  {
    None,
    Slot,
    Create,
  };

  u8 SlotCount() const;
  std::string SlotLabel(u8 slot) const;
  std::string SlotFigure(u8 slot) const;
  void DrawDialog(const ShellServices& services);
  void Report(const ShellServices& services, const std::string& error, std::string success);

  Kind m_kind;
  Dialog m_dialog = Dialog::None;
  u8 m_slot = 0;
  std::vector<Portals::FigureFile> m_files;
  std::vector<Portals::FigureType> m_types;
};
}  // namespace DolphinUWP

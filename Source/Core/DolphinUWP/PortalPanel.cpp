// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/PortalPanel.h"

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/Views.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/ShellWidgets.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;
}

u8 PortalPanel::SlotCount() const
{
  return m_kind == Kind::Skylanders ? Portals::SKYLANDER_SLOTS : Portals::INFINITY_SLOTS;
}

std::string PortalPanel::SlotLabel(u8 slot) const
{
  if (m_kind == Kind::Skylanders)
    return fmt::format("Slot {}", slot + 1);
  return Portals::GetInfinitySlotLabel(slot);
}

std::string PortalPanel::SlotFigure(u8 slot) const
{
  return m_kind == Kind::Skylanders ? Portals::GetSkylanderName(slot) :
                                      Portals::GetInfinityName(slot);
}

void PortalPanel::Report(const ShellServices& services, const std::string& error,
                         std::string success)
{
  if (error.empty())
    services.Notify(std::move(success));
  else
    PromptService::GetInstance().Inform("Figure", error, Common::MsgType::Warning);
}

void PortalPanel::Draw(const ShellServices& services)
{
  const bool skylanders = m_kind == Kind::Skylanders;
  Frontend::SectionTitle(skylanders ? "Skylanders Portal" : "Infinity Base");
  const Config::Info<bool>& enabled_info =
      skylanders ? Config::MAIN_EMULATE_SKYLANDER_PORTAL : Config::MAIN_EMULATE_INFINITY_BASE;
  bool enabled = Config::Get(enabled_info);
  if (Frontend::ShellCheckbox(skylanders ? "Emulate Skylanders portal" : "Emulate Infinity base",
                              &enabled))
  {
    Config::SetBaseOrCurrent(enabled_info, enabled);
    Config::Save();
  }
  ShellWidgets::HintText(fmt::format(
      "Figures are stored in {}. Figures placed here stay in place when a game starts.",
      skylanders ? Portals::GetSkylanderCollection() : Portals::GetInfinityCollection()));

  Frontend::SectionTitle("Figures");
  for (u8 slot = 0; slot < SlotCount(); ++slot)
  {
    const std::string figure = SlotFigure(slot);
    ImGui::PushID(slot);
    if (ShellWidgets::ValueRow("slot", SlotLabel(slot), figure.empty() ? "Empty" : figure,
                               !figure.empty()))
    {
      m_slot = slot;
      m_files = skylanders ? Portals::ListSkylanderFiles() : Portals::ListInfinityFiles();
      m_dialog = Dialog::Slot;
    }
    ImGui::PopID();
  }
  DrawDialog(services);
}

void PortalPanel::DrawDialog(const ShellServices& services)
{
  if (m_dialog == Dialog::None)
    return;

  const bool skylanders = m_kind == Kind::Skylanders;
  const std::string popup =
      fmt::format("{}-{}", skylanders ? "skylander-figures" : "infinity-figures",
                  m_dialog == Dialog::Slot ? "slot" : "create");
  if (!ImGui::IsPopupOpen(popup.c_str()))
    ImGui::OpenPopup(popup.c_str());

  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  const Frontend::ShellLayoutContext context =
      Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
  Frontend::SelectionDialogModel model;
  model.popupId = popup;
  model.closeLabel = "Cancel";

  if (m_dialog == Dialog::Slot)
  {
    model.title = SlotLabel(m_slot);
    model.entries.push_back({"create", "Create a new figure"});
    model.entries.push_back({"remove", "Remove the figure", {}, !SlotFigure(m_slot).empty()});
    for (const Portals::FigureFile& file : m_files)
      model.entries.push_back({file.path, file.name});

    const Frontend::SelectionDialogResult result = Frontend::DrawSelectionDialog(
        model, context, Frontend::CurrentWidgetTheme(), {640.0f, 0.0f});
    if (!result.selected)
    {
      if (result.closed)
        m_dialog = Dialog::None;
      return;
    }

    m_dialog = Dialog::None;
    if (*result.selected == 0)
    {
      m_types = skylanders ? Portals::ListSkylanderTypes() : Portals::ListInfinityTypes(m_slot);
      m_dialog = Dialog::Create;
    }
    else if (*result.selected == 1)
    {
      if (skylanders)
        Report(services, Portals::ClearSkylander(m_slot), "Removed the figure");
      else
      {
        Portals::ClearInfinity(m_slot);
        services.Notify("Removed the figure");
      }
    }
    else if (const size_t file = *result.selected - 2; file < m_files.size())
    {
      const std::string& path = m_files[file].path;
      Report(services,
             skylanders ? Portals::LoadSkylander(m_slot, path) :
                          Portals::LoadInfinity(m_slot, path),
             "Placed the figure");
    }
    return;
  }

  model.title = "Create a new figure";
  for (const Portals::FigureType& type : m_types)
    model.entries.push_back({std::to_string(type.id), type.name});
  const Frontend::SelectionDialogResult result =
      Frontend::DrawSelectionDialog(model, context, Frontend::CurrentWidgetTheme(), {640.0f, 0.0f});
  if (!result.selected)
  {
    if (result.closed)
      m_dialog = Dialog::None;
    return;
  }
  m_dialog = Dialog::None;
  if (*result.selected < m_types.size())
  {
    const u32 id = m_types[*result.selected].id;
    Report(services,
           skylanders ? Portals::CreateSkylander(m_slot, id) : Portals::CreateInfinity(m_slot, id),
           "Created and placed the figure");
  }
}
}  // namespace DolphinUWP

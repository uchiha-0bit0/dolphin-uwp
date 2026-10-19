// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/TitlePages.h"

#include <algorithm>
#include <cfloat>
#include <memory>
#include <unordered_map>
#include <utility>
#include <variant>

#include <fmt/format.h>
#include <imgui.h>

#include <UwpImGuiFrontend/ImGuiShell.h>
#include <UwpImGuiFrontend/NativeTextInput.h>
#include <UwpImGuiFrontend/Views.h>
#include <UwpImGuiFrontend/Widgets.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/GeckoCodeConfig.h"
#include "DiscIO/RiivolutionPatcher.h"
#include "DiscIO/Volume.h"
#include "DolphinUWP/BackgroundTask.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellServices.h"
#include "DolphinUWP/ShellWidgets.h"
#include "UICommon/GameFile.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

template <size_t N>
void CopyToBuffer(std::array<char, N>& buffer, std::string_view text)
{
  buffer.fill('\0');
  text.copy(buffer.data(), std::min(text.size(), N - 1));
}

Frontend::ShellLayoutContext ViewportLayout()
{
  const ImGuiViewport* const viewport = ImGui::GetMainViewport();
  return Frontend::MakeShellLayout(viewport->WorkSize, viewport->WorkPos);
}

bool ToggleSetting(const char* label, const Config::Info<bool>& info)
{
  bool value = Config::Get(info);
  if (!Frontend::ShellCheckbox(label, &value))
    return false;
  Config::SetBaseOrCurrent(info, value);
  Config::Save();
  return true;
}
}  // namespace

void CheatsPage::Open(const UICommon::GameFile& game)
{
  m_cheats.Load(game.GetGameID(), game.GetRevision());
  m_gametdb_id = game.GetGameTDBID();
  m_editor.reset();
  m_actions.reset();
  m_view = {};
  m_view.State().RequestFocus(Frontend::SplitPane::Navigation);
}

void CheatsPage::Close()
{
  Frontend::DismissNativeTextInput();
  m_editor.reset();
  m_actions.reset();
}

bool CheatsPage::Draw(float controller_scroll, bool context_pressed)
{
  m_context_pressed = context_pressed && !m_editor && !m_actions;

  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = controller_scroll;
  model.pages = {
      {"gecko", "Gecko Codes", true, [this] { DrawCodes(Kind::Gecko); }},
      {"action-replay", "Action Replay", true, [this] { DrawCodes(Kind::ActionReplay); }},
      {"patches", "Patches", true, [this] { DrawCodes(Kind::Patch); }},
  };
  const bool close = m_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
  DrawActions();
  DrawEditor();
  return close && !m_editor && !m_actions;
}

bool CheatsPage::IsUserDefined(Kind kind, size_t index) const
{
  switch (kind)
  {
  case Kind::Gecko:
    return m_cheats.gecko[index].user_defined;
  case Kind::ActionReplay:
    return m_cheats.action_replay[index].user_defined;
  default:
    return m_cheats.patches[index].user_defined;
  }
}

std::string CheatsPage::GetName(Kind kind, size_t index) const
{
  switch (kind)
  {
  case Kind::Gecko:
    return m_cheats.gecko[index].name;
  case Kind::ActionReplay:
    return m_cheats.action_replay[index].name;
  default:
    return m_cheats.patches[index].name;
  }
}

void CheatsPage::DrawCodes(Kind kind)
{
  ImGui::PushID(static_cast<int>(kind));
  if (kind != Kind::Patch && !Config::Get(Config::MAIN_ENABLE_CHEATS))
  {
    ShellWidgets::HintText("Cheats are turned off, so these codes are not applied.");
    ToggleSetting("Enable cheats", Config::MAIN_ENABLE_CHEATS);
  }

  Frontend::SectionTitle(kind == Kind::Gecko        ? "Gecko codes" :
                         kind == Kind::ActionReplay ? "Action Replay codes" :
                                                      "Patches");
  if (Frontend::ShellButton(kind == Kind::Patch ? "Add a patch" : "Add a code"))
    OpenEditor(kind, std::nullopt);
  if (kind == Kind::Gecko && Frontend::ShellButton("Download codes"))
    DownloadGeckoCodes();
  ShellWidgets::HintText(
      "Press A to turn an entry on or off, or X to edit or delete it. Changes apply the next "
      "time the game starts, or from the in-game menu while it runs.");

  const auto draw_entry = [this, kind](size_t index, std::string label, bool* enabled) {
    ImGui::PushID(static_cast<int>(index));
    if (Frontend::ShellCheckbox(label.c_str(), enabled))
      m_cheats.Save();
    if (m_context_pressed && ImGui::IsItemFocused())
      m_actions = std::pair{kind, index};
    ImGui::PopID();
  };

  size_t count = 0;
  switch (kind)
  {
  case Kind::Gecko:
    count = m_cheats.gecko.size();
    for (size_t i = 0; i < count; ++i)
    {
      Gecko::GeckoCode& code = m_cheats.gecko[i];
      draw_entry(i,
                 code.creator.empty() ? code.name : fmt::format("{} [{}]", code.name, code.creator),
                 &code.enabled);
    }
    break;
  case Kind::ActionReplay:
    count = m_cheats.action_replay.size();
    for (size_t i = 0; i < count; ++i)
      draw_entry(i, m_cheats.action_replay[i].name, &m_cheats.action_replay[i].enabled);
    break;
  case Kind::Patch:
    count = m_cheats.patches.size();
    for (size_t i = 0; i < count; ++i)
      draw_entry(i, m_cheats.patches[i].name, &m_cheats.patches[i].enabled);
    break;
  }
  if (count == 0)
    ShellWidgets::HintText("There are no entries yet.");
  ImGui::PopID();
}

void CheatsPage::DrawActions()
{
  constexpr const char* popup = "cheat-actions";
  if (!m_actions)
    return;
  if (!ImGui::IsPopupOpen(popup))
    ImGui::OpenPopup(popup);

  const auto [kind, index] = *m_actions;
  Frontend::SelectionDialogModel model;
  model.popupId = popup;
  model.title = GetName(kind, index);
  model.entries = {
      {"edit", "Edit"},
      {"delete", "Delete", IsUserDefined(kind, index) ? "" : "Included with Dolphin",
       IsUserDefined(kind, index)},
  };
  model.closeLabel = "Cancel";
  const Frontend::SelectionDialogResult result =
      Frontend::DrawSelectionDialog(model, ViewportLayout(), Frontend::CurrentWidgetTheme());
  if (!result.selected && !result.closed)
    return;
  m_actions.reset();
  if (result.selected == 0u)
    OpenEditor(kind, index);
  else if (result.selected == 1u)
    Delete(kind, index);
}

void CheatsPage::OpenEditor(Kind kind, std::optional<size_t> index)
{
  Editor editor;
  editor.kind = kind;
  editor.index = index;
  if (index)
  {
    switch (kind)
    {
    case Kind::Gecko:
    {
      const Gecko::GeckoCode& code = m_cheats.gecko[*index];
      CopyToBuffer(editor.name, code.name);
      CopyToBuffer(editor.creator, code.creator);
      CopyToBuffer(editor.code, FormatGeckoCode(code));
      break;
    }
    case Kind::ActionReplay:
      CopyToBuffer(editor.name, m_cheats.action_replay[*index].name);
      CopyToBuffer(editor.code, FormatActionReplayCode(m_cheats.action_replay[*index]));
      break;
    case Kind::Patch:
      CopyToBuffer(editor.name, m_cheats.patches[*index].name);
      CopyToBuffer(editor.code, FormatPatch(m_cheats.patches[*index]));
      break;
    }
  }
  m_editor = std::move(editor);
}

bool CheatsPage::SaveEditor()
{
  Editor& editor = *m_editor;
  const std::string name(StripWhitespace(editor.name.data()));
  const std::string text = editor.code.data();
  if (name.empty())
  {
    editor.error = "Enter a name.";
    return false;
  }

  std::string error;
  switch (editor.kind)
  {
  case Kind::Gecko:
  {
    auto codes = ParseGeckoCode(text, &error);
    if (!codes)
      break;
    Gecko::GeckoCode code;
    if (editor.index)
      code = m_cheats.gecko[*editor.index];
    else
      code.enabled = true;
    code.name = name;
    code.creator = std::string(StripWhitespace(editor.creator.data()));
    code.codes = std::move(*codes);
    code.user_defined = true;
    if (editor.index)
      m_cheats.gecko[*editor.index] = std::move(code);
    else
      m_cheats.gecko.push_back(std::move(code));
    break;
  }
  case Kind::ActionReplay:
  {
    auto entries = ParseActionReplayCode(text, &error);
    if (!entries)
      break;
    ActionReplay::ARCode code;
    code.enabled = true;
    if (editor.index)
      code = m_cheats.action_replay[*editor.index];
    code.name = name;
    code.ops = std::move(*entries);
    code.user_defined = true;
    // Built-in codes are kept, and the edited version is added as a user code.
    if (editor.index && m_cheats.action_replay[*editor.index].user_defined)
      m_cheats.action_replay[*editor.index] = std::move(code);
    else
      m_cheats.action_replay.push_back(std::move(code));
    break;
  }
  case Kind::Patch:
  {
    auto entries = ParsePatch(text, &error);
    if (!entries)
      break;
    PatchEngine::Patch patch;
    patch.enabled = true;
    if (editor.index)
      patch = m_cheats.patches[*editor.index];
    patch.name = name;
    patch.entries = std::move(*entries);
    patch.user_defined = true;
    if (editor.index && m_cheats.patches[*editor.index].user_defined)
    {
      m_cheats.patches[*editor.index] = std::move(patch);
    }
    else
    {
      if (editor.index && patch.name == m_cheats.patches[*editor.index].name)
        patch.name += " (Copy)";
      m_cheats.patches.push_back(std::move(patch));
    }
    break;
  }
  }

  if (!error.empty())
  {
    editor.error = std::move(error);
    return false;
  }
  m_cheats.Save();
  return true;
}

void CheatsPage::DrawEditor()
{
  constexpr const char* popup = "cheat-editor";
  if (!m_editor)
    return;
  if (!ImGui::IsPopupOpen(popup))
    ImGui::OpenPopup(popup);

  const Frontend::ShellLayoutContext context = ViewportLayout();
  if (!Frontend::BeginThemedModal(popup, context, Frontend::CurrentWidgetTheme(),
                                  Frontend::ToPx(context, 780.0f, 0.0f)))
  {
    return;
  }

  Editor& editor = *m_editor;
  const char* noun = editor.kind == Kind::Patch ? "patch" : "code";
  Frontend::SectionTitle(fmt::format("{} {}", editor.index ? "Edit the" : "Add a", noun).c_str());

  ImGui::TextUnformatted("Name");
  ImGui::SetNextItemWidth(-FLT_MIN);
  (void)Frontend::NativeInputText("##name", editor.name.data(), editor.name.size());
  if (std::exchange(editor.focus, false))
  {
    ImGui::SetItemDefaultFocus();
    ImGui::SetNavCursorVisible(true);
  }
  if (editor.kind == Kind::Gecko)
  {
    ImGui::TextUnformatted("Creator");
    ImGui::SetNextItemWidth(-FLT_MIN);
    (void)Frontend::NativeInputText("##creator", editor.creator.data(), editor.creator.size());
  }

  ImGui::TextUnformatted(editor.kind == Kind::Patch ? "Patch lines" : "Code lines");
  (void)Frontend::NativeInputTextMultiline("##code", editor.code.data(), editor.code.size(),
                                           {-FLT_MIN, Frontend::ToPx(context, 0.0f, 190.0f).y});
  ShellWidgets::HintText(editor.kind == Kind::Patch ?
                             "Enter lines such as 0x80001234:dword:0x60000000, separated by "
                             "spaces or new lines." :
                         editor.kind == Kind::Gecko ?
                             "Enter pairs of eight-digit values such as 04001234 60000000, "
                             "separated by spaces or new lines." :
                             "Enter decrypted pairs such as 04001234 60000000, or encrypted lines "
                             "such as ABCD-EFGH-IJKLM, separated by spaces or new lines.");
  if (!editor.error.empty())
  {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.86f, 0.20f, 0.24f, 1.0f));
    ImGui::TextWrapped("%s", editor.error.c_str());
    ImGui::PopStyleColor();
  }

  const ImVec2 button_size = Frontend::ToPx(context, 170.0f, 46.0f);
  bool close = false;
  if (Frontend::ShellButton("Save", button_size) && SaveEditor())
    close = true;
  ImGui::SameLine();
  if (Frontend::ShellButton("Cancel", button_size) ||
      (!Frontend::IsNativeTextInputActive() && Frontend::ConsumeWidgetBackPressed()))
  {
    close = true;
  }
  if (close)
  {
    Frontend::DismissNativeTextInput();
    ImGui::CloseCurrentPopup();
    m_editor.reset();
  }
  Frontend::EndThemedModal();
}

void CheatsPage::Delete(Kind kind, size_t index)
{
  if (!IsUserDefined(kind, index))
    return;
  PromptService::GetInstance().Confirm(
      "Delete", fmt::format("Delete \"{}\"?", GetName(kind, index)), [this, kind, index](bool yes) {
        if (!yes)
          return;
        switch (kind)
        {
        case Kind::Gecko:
          if (index < m_cheats.gecko.size())
            m_cheats.gecko.erase(m_cheats.gecko.begin() + index);
          break;
        case Kind::ActionReplay:
          if (index < m_cheats.action_replay.size())
            m_cheats.action_replay.erase(m_cheats.action_replay.begin() + index);
          break;
        case Kind::Patch:
          if (index < m_cheats.patches.size())
            m_cheats.patches.erase(m_cheats.patches.begin() + index);
          break;
        }
        m_cheats.Save();
      });
}

void CheatsPage::DownloadGeckoCodes()
{
  if (m_gametdb_id.empty())
    return;
  m_services.task->Start(
      "Downloading Gecko codes", false,
      [this, id = m_gametdb_id](BackgroundTask::Context&) -> BackgroundTask::Completion {
        auto result = Gecko::DownloadCodes(id);
        return [this, result = std::move(result)] {
          if (!result)
          {
            std::string message;
            if (result.error() == 404)
              message = "There are no codes for this game.";
            else if (result.error() <= 0)
              message = "The codes could not be downloaded. Check your Internet connection.";
            else
              message = fmt::format("The codes could not be downloaded (HTTP {}).", result.error());
            PromptService::GetInstance().Inform("Download codes", std::move(message),
                                                Common::MsgType::Warning);
            return;
          }
          size_t added = 0;
          for (const Gecko::GeckoCode& code : *result)
          {
            if (std::ranges::find(m_cheats.gecko, code) != m_cheats.gecko.end())
              continue;
            m_cheats.gecko.push_back(code);
            ++added;
          }
          m_cheats.Save();
          PromptService::GetInstance().Inform(
              "Download codes", fmt::format("Downloaded {} code(s). {} new code(s) were added.",
                                            result->size(), added));
        };
      });
}

void GraphicsModsPage::Open(const UICommon::GameFile& game)
{
  m_mods.Load(game.GetGameID());
  m_view = {};
  m_view.State().RequestFocus(Frontend::SplitPane::Navigation);
}

void GraphicsModsPage::Close()
{
  m_mods.Clear();
}

bool GraphicsModsPage::Draw(float controller_scroll)
{
  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = controller_scroll;
  model.pages = {{"mods", "Graphics Mods", true, [this] {
                    Frontend::SectionTitle("Graphics mods");
                    ToggleSetting("Enable graphics mods", Config::GFX_MODS_ENABLE);
                    if (m_draw_storage)
                    {
                      m_draw_storage("graphics-mod-settings");
                      ShellWidgets::HintText("The mods that each game uses are kept here. Files "
                                             "already in the old location are not moved.");
                    }
                    const std::vector<GraphicsModList::Mod> mods = m_mods.GetMods();
                    if (mods.empty())
                    {
                      ShellWidgets::HintText(
                          fmt::format("No graphics mods were found for this game. Place mods in {}",
                                      File::GetUserPath(D_GRAPHICSMOD_IDX)));
                    }
                    for (size_t i = 0; i < mods.size(); ++i)
                    {
                      const GraphicsModList::Mod& mod = mods[i];
                      ImGui::PushID(static_cast<int>(i));
                      bool enabled = mod.enabled;
                      if (Frontend::ShellCheckbox(mod.title.c_str(), &enabled))
                        m_mods.SetEnabled(mod.path, enabled);
                      std::string detail =
                          mod.author.empty() ? std::string() : fmt::format("By {}", mod.author);
                      if (!mod.description.empty())
                        detail += (detail.empty() ? "" : ". ") + mod.description;
                      if (!detail.empty())
                        ShellWidgets::HintText(detail);
                      ImGui::PopID();
                    }
                  }}};
  return m_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
}

void RiivolutionPage::Open(const UICommon::GameFile& game, UwpImGuiFrontend::ItemId item,
                           std::string title)
{
  m_discs.clear();
  m_path = game.GetFilePath();
  m_item = item;
  m_title = std::move(title);
  m_view = {};
  m_view.State().RequestFocus(Frontend::SplitPane::Navigation);

  const std::unique_ptr<BootParameters> parameters = BootParameters::GenerateFromFile(m_path);
  const auto* disc =
      parameters ? std::get_if<BootParameters::Disc>(&parameters->parameters) : nullptr;
  if (!disc || !disc->volume)
    return;
  m_game_id = disc->volume->GetGameID();
  m_revision = disc->volume->GetRevision();
  m_disc_number = disc->volume->GetDiscNumber();

  const std::string root = File::GetUserPath(D_RIIVOLUTION_IDX);
  std::optional<DiscIO::Riivolution::Config> config;
  if (m_game_id.size() >= 4)
  {
    config = DiscIO::Riivolution::ParseConfigFile(
        fmt::format("{}/riivolution/config/{}.xml", root, m_game_id.substr(0, 4)));
  }
  for (const std::string& path : Common::DoFileSearch(root + "riivolution", ".xml"))
  {
    auto parsed = DiscIO::Riivolution::ParseFile(path);
    if (!parsed || !parsed->IsValidForGame(m_game_id, m_revision, m_disc_number))
      continue;
    if (config)
      DiscIO::Riivolution::ApplyConfigDefaults(&*parsed, *config);
    std::string name;
    SplitPath(path, nullptr, &name, nullptr);
    m_discs.push_back({std::move(*parsed), root, std::move(name)});
  }
}

void RiivolutionPage::Close()
{
  m_discs.clear();
}

void RiivolutionPage::SaveConfig() const
{
  if (m_game_id.size() != 4 && m_game_id.size() != 6)
    return;

  std::unordered_map<std::string, DiscIO::Riivolution::Config> configs;
  for (const PatchDisc& entry : m_discs)
  {
    auto& options = configs[entry.root].m_options;
    for (const auto& section : entry.disc.m_sections)
    {
      for (const auto& option : section.m_options)
      {
        options.push_back({option.m_id.empty() ? section.m_name + option.m_name : option.m_id,
                           option.m_selected_choice});
      }
    }
  }
  for (const auto& [root, config] : configs)
  {
    DiscIO::Riivolution::WriteConfigFile(
        fmt::format("{}/riivolution/config/{}.xml", root, m_game_id.substr(0, 4)), config);
  }
}

void RiivolutionPage::Boot()
{
  SaveConfig();
  std::unique_ptr<BootParameters> parameters = BootParameters::GenerateFromFile(m_path);
  if (!parameters)
  {
    m_services.Notify("The game could not be started");
    return;
  }

  std::vector<DiscIO::Riivolution::Patch> patches;
  for (const PatchDisc& entry : m_discs)
  {
    for (auto& patch : entry.disc.GeneratePatches(m_game_id))
    {
      patch.m_file_data_loader = std::make_shared<DiscIO::Riivolution::FileDataLoaderHostFS>(
          entry.root, entry.disc.m_xml_path, patch.m_root);
      patches.push_back(std::move(patch));
    }
  }
  AddRiivolutionPatches(parameters.get(), std::move(patches));
  m_services.boot({std::move(parameters), m_title, m_item});
}

bool RiivolutionPage::Draw(float controller_scroll)
{
  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = controller_scroll;
  for (size_t index = 0; index < m_discs.size(); ++index)
  {
    model.pages.push_back(
        {fmt::format("disc-{}", index), m_discs[index].name, true, [this, index] {
           PatchDisc& entry = m_discs[index];
           if (Frontend::ShellButton("Start the game with these patches"))
             Boot();
           for (size_t s = 0; s < entry.disc.m_sections.size(); ++s)
           {
             auto& section = entry.disc.m_sections[s];
             ImGui::PushID(static_cast<int>(s));
             Frontend::SectionTitle(section.m_name.c_str());
             for (size_t o = 0; o < section.m_options.size(); ++o)
             {
               auto& option = section.m_options[o];
               std::vector<Frontend::ChoiceItem> choices{{"0", "Disabled"}};
               for (size_t c = 0; c < option.m_choices.size(); ++c)
                 choices.push_back({std::to_string(c + 1), option.m_choices[c].m_name});
               size_t selected = std::min<size_t>(option.m_selected_choice, choices.size() - 1);
               ImGui::PushID(static_cast<int>(o));
               if (Frontend::ShellCombo(option.m_name.c_str(), &selected, choices))
                 option.m_selected_choice = static_cast<u32>(selected);
               ImGui::PopID();
             }
             ImGui::PopID();
           }
         }});
  }
  if (m_discs.empty())
  {
    model.pages.push_back({"none", "Riivolution", true, [this] {
                             Frontend::SectionTitle("Riivolution");
                             ShellWidgets::HintText(fmt::format(
                                 "No Riivolution patches were found for this game. Place the "
                                 "patch XML files in {}riivolution, with their patch folders "
                                 "next to them as on an SD card.",
                                 File::GetUserPath(D_RIIVOLUTION_IDX)));
                             if (Frontend::ShellButton("Start the game without patches"))
                               Boot();
                           }});
  }
  return m_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
}
}  // namespace DolphinUWP

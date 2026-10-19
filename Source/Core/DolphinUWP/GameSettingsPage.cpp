// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GameSettingsPage.h"

#include <optional>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <UwpImGuiFrontend/Widgets.h>

#include "Common/Config/Layer.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "DolphinUWP/PromptService.h"
#include "DolphinUWP/ShellWidgets.h"
#include "UICommon/GameFile.h"

namespace DolphinUWP
{
namespace
{
namespace Frontend = UwpImGuiFrontend;

constexpr std::array<Choice<std::string_view>, 3> DETERMINISM_MODES{{
    {"auto", "Automatic"},
    {"none", "None"},
    {"fake-completion", "Fake completion"},
}};
}  // namespace

GameSettingsPage::GameSettingsPage() = default;
GameSettingsPage::~GameSettingsPage() = default;

void GameSettingsPage::Open(const UICommon::GameFile& game)
{
  m_game_id = game.GetGameID();
  m_layer = std::make_unique<Config::Layer>(
      ConfigLoaders::GenerateLocalGameConfigLoader(m_game_id, game.GetRevision()));
  m_global_layer = std::make_unique<Config::Layer>(
      ConfigLoaders::GenerateGlobalGameConfigLoader(m_game_id, game.GetRevision()));
  m_view = {};
}

void GameSettingsPage::Close()
{
  m_layer.reset();
  m_global_layer.reset();
}

bool GameSettingsPage::Draw(float controller_scroll)
{
  if (!m_layer)
    return true;

  Frontend::GraphicsViewModel model;
  model.contentControllerScroll = controller_scroll;
  model.pages = {
      {"core", "Core", true, [this] { DrawCore(); }},
      {"graphics", "Graphics", true, [this] { DrawGraphics(); }},
  };
  return m_view.Draw(model, ImGui::GetContentRegionAvail()).closeRequested;
}

template <typename T>
T GameSettingsPage::GetInherited(const Config::Info<T>& info) const
{
  if (const std::optional<T> value = m_global_layer->Get<T>(info.GetLocation()))
    return *value;
  return Config::GetBase(info);
}

void GameSettingsPage::BoolSetting(const char* label, const Config::Info<bool>& info, bool invert)
{
  const bool inherited = GetInherited(info) != invert;
  const std::optional<bool> overridden = m_layer->Get<bool>(info.GetLocation());
  size_t selected = overridden ? (*overridden != invert ? 1 : 2) : 0;
  const std::array<Frontend::ChoiceItem, 3> items{{
      {"default", fmt::format("Default ({})", inherited ? "On" : "Off")},
      {"on", "On"},
      {"off", "Off"},
  }};
  if (!Frontend::ShellCombo(label, &selected, items))
    return;

  if (selected == 0)
    m_layer->DeleteKey(info.GetLocation());
  else
    m_layer->Set(info, (selected == 1) != invert);
  m_layer->Save();
}

template <typename T, size_t N>
void GameSettingsPage::ChoiceSetting(const char* label, const Config::Info<T>& info,
                                     const std::array<Choice<T>, N>& choices)
{
  const T inherited = GetInherited(info);
  const std::optional<T> overridden = m_layer->Get<T>(info.GetLocation());
  const char* inherited_label = "Custom";
  std::vector<Frontend::ChoiceItem> items{{"default", {}}};
  size_t selected = 0;
  for (size_t i = 0; i < N; ++i)
  {
    if (choices[i].value == inherited)
      inherited_label = choices[i].label;
    if (overridden && *overridden == choices[i].value)
      selected = i + 1;
    items.push_back({std::to_string(i), choices[i].label});
  }
  items.front().label = fmt::format("Default ({})", inherited_label);
  if (!Frontend::ShellCombo(label, &selected, items))
    return;

  if (selected == 0)
    m_layer->DeleteKey(info.GetLocation());
  else
    m_layer->Set(info, T(choices[selected - 1].value));
  m_layer->Save();
}

void GameSettingsPage::DrawCore()
{
  Frontend::SectionTitle("Game settings");
  ShellWidgets::HintText(fmt::format(
      "These settings override Dolphin's settings when {} starts. Settings left at Default follow "
      "the global settings and Dolphin's recommendations for this game.",
      m_game_id));
  if (Frontend::ShellButton("Use the defaults for every setting"))
    Reset();

  Frontend::SectionTitle("Core");
  BoolSetting("Dual core", Config::MAIN_CPU_THREAD);
  BoolSetting("MMU", Config::MAIN_MMU);
  BoolSetting("FPRF", Config::MAIN_FPRF);
  BoolSetting("Synchronize GPU thread", Config::MAIN_SYNC_GPU);
  BoolSetting("Emulate disc speed", Config::MAIN_FAST_DISC_SPEED, true);
  BoolSetting("DSP HLE", Config::MAIN_DSP_HLE);

  const std::string inherited = GetInherited(Config::MAIN_GPU_DETERMINISM_MODE);
  const std::optional<std::string> overridden =
      m_layer->Get<std::string>(Config::MAIN_GPU_DETERMINISM_MODE.GetLocation());
  std::vector<Frontend::ChoiceItem> items{{"default", {}}};
  const char* inherited_label = "Automatic";
  size_t selected = 0;
  for (size_t i = 0; i < DETERMINISM_MODES.size(); ++i)
  {
    if (DETERMINISM_MODES[i].value == inherited)
      inherited_label = DETERMINISM_MODES[i].label;
    if (overridden && *overridden == DETERMINISM_MODES[i].value)
      selected = i + 1;
    items.push_back({std::string(DETERMINISM_MODES[i].value), DETERMINISM_MODES[i].label});
  }
  items.front().label = fmt::format("Default ({})", inherited_label);
  if (Frontend::ShellCombo("Deterministic dual core", &selected, items))
  {
    if (selected == 0)
    {
      m_layer->DeleteKey(Config::MAIN_GPU_DETERMINISM_MODE.GetLocation());
    }
    else
    {
      m_layer->Set(Config::MAIN_GPU_DETERMINISM_MODE,
                   std::string(DETERMINISM_MODES[selected - 1].value));
    }
    m_layer->Save();
  }

  if (m_draw_storage)
  {
    Frontend::SectionTitle("Settings location");
    m_draw_storage("game-settings");
    ShellWidgets::HintText("The settings of every game are kept here. Files already in the old "
                           "location are not moved.");
  }
}

void GameSettingsPage::DrawGraphics()
{
  Frontend::SectionTitle("Rendering");
  ChoiceSetting("Internal resolution", Config::GFX_EFB_SCALE, RESOLUTIONS);
  ChoiceSetting("Aspect ratio", Config::GFX_ASPECT_RATIO, ASPECT_MODES);
  BoolSetting("Widescreen hack", Config::GFX_WIDESCREEN_HACK);
  ChoiceSetting("Shader compilation", Config::GFX_SHADER_COMPILATION_MODE, SHADER_MODES);

  Frontend::SectionTitle("Embedded frame buffer");
  BoolSetting("Skip EFB access from CPU", Config::GFX_HACK_EFB_ACCESS_ENABLE, true);
  BoolSetting("Ignore format changes", Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true);
  BoolSetting("Store EFB copies to texture only", Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM);
  BoolSetting("Defer EFB copies to RAM", Config::GFX_HACK_DEFER_EFB_COPIES);

  Frontend::SectionTitle("Texture cache and external frame buffer");
  ChoiceSetting("Texture cache accuracy", Config::GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES,
                TEXTURE_CACHE_MODES);
  BoolSetting("Store XFB copies to texture only", Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM);
  BoolSetting("Immediately present XFB", Config::GFX_HACK_IMMEDIATE_XFB);

  Frontend::SectionTitle("Other");
  BoolSetting("Vertex rounding", Config::GFX_HACK_VERTEX_ROUNDING);
  BoolSetting("Bounding box", Config::GFX_HACK_BBOX_ENABLE);
  BoolSetting("Monoscopic shadows", Config::GFX_STEREO_EFB_MONO_DEPTH);
}

void GameSettingsPage::Reset()
{
  PromptService::GetInstance().Confirm(
      "Game settings",
      fmt::format("Use the defaults for every setting of {}? Codes and patches are kept.",
                  m_game_id),
      [this](bool confirmed) {
        if (!confirmed || !m_layer)
          return;
        m_layer->DeleteAllKeys();
        m_layer->Save();
      });
}
}  // namespace DolphinUWP

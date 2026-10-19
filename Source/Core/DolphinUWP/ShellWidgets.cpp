// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/ShellWidgets.h"

#include <algorithm>
#include <string>

#include <UwpImGuiFrontend/ImGuiShell.h>
#include <UwpImGuiFrontend/Text.h>
#include <UwpImGuiFrontend/Widgets.h>

namespace DolphinUWP::ShellWidgets
{
namespace
{
ImU32 Color(const ImVec4& color)
{
  return ImGui::ColorConvertFloat4ToU32(color);
}

struct RowState
{
  ImVec2 min;
  ImVec2 max;
  bool hovered = false;
  bool focused = false;
  bool activated = false;
};

RowState BeginRow(const char* id, float height)
{
  namespace Frontend = UwpImGuiFrontend;
  RowState state;
  const ImVec2 size{std::max(1.0f, ImGui::GetContentRegionAvail().x), height};
  state.min = ImGui::GetCursorScreenPos();
  state.max = {state.min.x + size.x, state.min.y + size.y};

  ImGui::PushID(id);
  (void)Frontend::ApplyPendingWidgetFocus();
  ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
  const bool clicked = ImGui::InvisibleButton("row", size, ImGuiButtonFlags_EnableNav);
  ImGui::PopStyleColor();
  state.hovered = ImGui::IsItemHovered();
  state.focused = ImGui::IsItemFocused();
  state.activated = clicked || (state.focused && Frontend::IsWidgetAcceptPressed());
  ImGui::PopID();

  ImDrawList* const draw = ImGui::GetWindowDrawList();
  draw->PushClipRect({state.min.x - 8.0f, state.min.y - 8.0f},
                     {state.max.x + 8.0f, state.max.y + 8.0f}, true);
  const Frontend::ShellTheme& theme = Frontend::CurrentWidgetTheme();
  Frontend::DrawShellPanelPx(draw, state.min, state.max, theme, 14.0f,
                             state.hovered || state.focused ? 0.70f : 0.48f);
  Frontend::DrawReplicaFocus(draw, state.min, state.max, 14.0f, state.focused, state.hovered);
  return state;
}

#if IMGUI_VERSION_NUM >= 19200
ImTextureRef TextureReference(std::uintptr_t id)
{
  return ImTextureRef(static_cast<ImTextureID>(id));
}
#else
ImTextureID TextureReference(std::uintptr_t id)
{
  return reinterpret_cast<ImTextureID>(id);
}
#endif
}  // namespace

bool ValueRow(const char* id, std::string_view label, std::string_view value, bool highlight_value,
              float height)
{
  namespace Frontend = UwpImGuiFrontend;
  const RowState row = BeginRow(id, height);
  const Frontend::ShellTheme& theme = Frontend::CurrentWidgetTheme();
  ImDrawList* const draw = ImGui::GetWindowDrawList();

  const std::string value_text(Frontend::TranslateText(value));
  const float value_width = ImGui::CalcTextSize(value_text.c_str()).x;
  const float split = row.min.x + (row.max.x - row.min.x) * 0.45f;
  const float value_left = std::max(split, row.max.x - 16.0f - value_width);
  Frontend::DrawClippedText(draw, {row.min.x + 16.0f, row.min.y}, {value_left - 12.0f, row.max.y},
                            Color(theme.textPrimary), std::string(label).c_str());
  Frontend::DrawClippedText(draw, {value_left, row.min.y}, {row.max.x - 16.0f, row.max.y},
                            Color(highlight_value ? theme.cursorNormal : theme.textSecondary),
                            value_text.c_str());
  draw->PopClipRect();
  return row.activated;
}

bool DetailRow(const char* id, UwpImGuiFrontend::TextureHandle image, ImVec2 image_size,
               std::string_view title, std::string_view subtitle, std::string_view value,
               float height)
{
  namespace Frontend = UwpImGuiFrontend;
  const RowState row = BeginRow(id, height);
  const Frontend::ShellTheme& theme = Frontend::CurrentWidgetTheme();
  ImDrawList* const draw = ImGui::GetWindowDrawList();

  float text_left = row.min.x + 16.0f;
  if (image_size.x > 0.0f && image_size.y > 0.0f)
  {
    const float scale = std::min(1.0f, (height - 16.0f) / image_size.y);
    const ImVec2 size{image_size.x * scale, image_size.y * scale};
    const ImVec2 image_min{text_left, row.min.y + (height - size.y) * 0.5f};
    const ImVec2 image_max{image_min.x + size.x, image_min.y + size.y};
    if (image)
      draw->AddImage(TextureReference(image.id), image_min, image_max);
    else
      draw->AddRectFilled(image_min, image_max, Frontend::ColorWithAlpha(theme.panelBorder, 0.35f),
                          4.0f);
    text_left = image_max.x + 14.0f;
  }

  const std::string value_text(value);
  const float value_width = value_text.empty() ? 0.0f : ImGui::CalcTextSize(value_text.c_str()).x;
  const float value_left = row.max.x - 16.0f - value_width;
  const float text_right = value_text.empty() ? row.max.x - 16.0f : value_left - 16.0f;
  const float line = ImGui::GetTextLineHeight();
  const float middle = row.min.y + height * 0.5f;
  if (subtitle.empty())
  {
    Frontend::DrawClippedText(draw, {text_left, row.min.y}, {text_right, row.max.y},
                              Color(theme.textPrimary), std::string(title).c_str());
  }
  else
  {
    Frontend::DrawClippedText(draw, {text_left, middle - line - 1.0f}, {text_right, middle - 1.0f},
                              Color(theme.textPrimary), std::string(title).c_str());
    Frontend::DrawClippedText(draw, {text_left, middle + 1.0f}, {text_right, middle + line + 1.0f},
                              Color(theme.textSecondary), std::string(subtitle).c_str());
  }
  if (!value_text.empty())
  {
    Frontend::DrawClippedText(draw, {value_left, row.min.y}, {row.max.x - 16.0f, row.max.y},
                              Color(theme.textSecondary), value_text.c_str());
  }
  draw->PopClipRect();
  return row.activated;
}

void HintText(std::string_view text)
{
  text = UwpImGuiFrontend::TranslateText(text);
  ImGui::PushStyleColor(ImGuiCol_Text, UwpImGuiFrontend::CurrentWidgetTheme().textSecondary);
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text.data(), text.data() + text.size());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}
}  // namespace DolphinUWP::ShellWidgets

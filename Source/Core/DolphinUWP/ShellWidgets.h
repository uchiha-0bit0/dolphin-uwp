// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <imgui.h>

#include <UwpImGuiFrontend/Types.h>

namespace DolphinUWP::ShellWidgets
{
// A full-width row with a label on the left and a value on the right. Returns true on activation.
bool ValueRow(const char* id, std::string_view label, std::string_view value,
              bool highlight_value = false, float height = 48.0f);

// A full-width row with an optional image, a title above a subtitle, and a value on the right.
bool DetailRow(const char* id, UwpImGuiFrontend::TextureHandle image, ImVec2 image_size,
               std::string_view title, std::string_view subtitle, std::string_view value = {},
               float height = 72.0f);

// Unformatted wrapped text in the secondary text colour.
void HintText(std::string_view text);
}  // namespace DolphinUWP::ShellWidgets

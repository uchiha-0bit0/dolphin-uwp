// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Text without a translation stays in English.
namespace DolphinUWP::Translation
{
// The interface languages, as pairs of a code and the name of the language in that language.
std::vector<std::pair<std::string, std::string>> GetLanguages();
// Uses the language of the settings, or the first of the system's languages that Dolphin has.
// Call on the UI thread while no game runs.
void Apply();
std::string_view Translate(std::string_view text);
}  // namespace DolphinUWP::Translation

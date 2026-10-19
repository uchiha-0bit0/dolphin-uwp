// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/Config/Config.h"

namespace Config
{
extern const Info<int> UWP_THEME;
extern const Info<bool> UWP_SHOW_ACTION_HINTS;
extern const Info<bool> UWP_ANIMATE_LAUNCHES;
extern const Info<int> UWP_STATE_SLOT;
extern const Info<std::string> UWP_FAVORITES;
// Locations of folders that Dolphin keeps in its user folder, which the app can move.
extern const Info<std::string> UWP_GC_PATH;
extern const Info<std::string> UWP_CONTROLLER_PROFILES_PATH;
extern const Info<std::string> UWP_GAME_SETTINGS_PATH;
extern const Info<std::string> UWP_CACHE_PATH;
extern const Info<std::string> UWP_GRAPHICS_MOD_SETTINGS_PATH;
extern const Info<int> UWP_LIBRARY_PLATFORM;
extern const Info<int> UWP_LIBRARY_SORT;

extern const Info<bool> UWP_SCREENSCRAPER_ENABLED;
extern const Info<std::string> UWP_SCREENSCRAPER_MEDIA_DIRECTORY;
extern const Info<int> UWP_SCREENSCRAPER_LIST_ARTWORK;
extern const Info<int> UWP_SCREENSCRAPER_DETAIL_ARTWORK;
extern const Info<int> UWP_SCREENSCRAPER_BACKGROUND;
extern const Info<int> UWP_SCREENSCRAPER_REGION;
extern const Info<int> UWP_SCREENSCRAPER_DOWNLOADS;
extern const Info<bool> UWP_SCREENSCRAPER_OVERWRITE;

extern const Info<std::string> UWP_DSU_SERVERS;
extern const Info<bool> UWP_STREAMING_ENABLED;
extern const Info<std::string> UWP_STREAMING_URL;

enum class GBALayout
{
  Bottom,
  Top,
  Left,
  Right,
  Corners,
  Hidden,
};

extern const Info<GBALayout> UWP_GBA_LAYOUT;
// 0 fits the screens to the layout; 1-4 is a fixed scale.
extern const Info<int> UWP_GBA_SCALE;
extern const Info<bool> UWP_GBA_FRAME;
extern const Info<bool> UWP_GBA_INTERFRAME_BLENDING;
// Keeps a GBA screen on the TV while a companion streams it.
extern const Info<bool> UWP_GBA_SHOW_STREAMED;
// -1 follows the setting of all GBAs. Otherwise the size is as UWP_GBA_SCALE, and blending is 0
// for off and 1 for on.
extern const std::array<Info<int>, 4> UWP_GBA_SCALES;
extern const std::array<Info<int>, 4> UWP_GBA_BLENDING;
// Draws a GBA screen above the in-game menus.
extern const std::array<Info<bool>, 4> UWP_GBA_ALWAYS_ON_TOP;
}  // namespace Config

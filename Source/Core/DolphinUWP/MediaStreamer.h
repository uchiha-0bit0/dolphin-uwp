// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <UwpImGuiFrontend/DsuCompanion.h>

#include "Common/CommonTypes.h"

// Streams the game output and the integrated GBA screens as H.264 and AAC in MPEG-TS over UDP.
// Companions that watch the same source share one encoder, which keeps the settings of the first
// request. A companion that watches a GBA with sound takes that GBA's sound out of the game audio.
namespace DolphinUWP::MediaStreamer
{
enum class Source : u8
{
  Game,
  GBA1,
  GBA2,
  GBA3,
  GBA4,
};
constexpr std::size_t SOURCE_COUNT = 5;

constexpr Source GBASource(std::size_t port)
{
  return static_cast<Source>(static_cast<std::size_t>(Source::GBA1) + port);
}

// The screen is the one the companion asked for, which names the stream in its status.
bool Start(u32 peer, u8 screen, Source source, const UwpImGuiFrontend::StreamRequest& request,
           std::string* error);
void Stop(u32 peer, u8 screen);
void StopAll();
std::optional<UwpImGuiFrontend::ActiveStream> GetActiveStream(u32 peer, u8 screen);
std::optional<std::string> TakeFailure(u32 peer, u8 screen);
bool IsStreaming(Source source);
}  // namespace DolphinUWP::MediaStreamer

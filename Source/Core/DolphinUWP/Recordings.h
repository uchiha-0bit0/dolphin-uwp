// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace DolphinUWP::Recordings
{
struct RecordingFile
{
  std::string path;
  std::string name;
  std::string game_id;
};

// Input recordings and FIFO logs made in the app are stored in the user folder.
std::string GetMovieDirectory();
std::string GetFifoDirectory();
std::string MakeFileName(std::string_view directory, std::string_view game_id,
                         std::string_view extension);

std::vector<RecordingFile> ListMovies();
std::vector<RecordingFile> ListFifoLogs();
std::optional<std::string> ReadMovieGameId(const std::string& path);

// Arms input recording with the configured controllers. Starts on boot when stopped.
bool BeginMovieRecording();
// Saves the active recording under the movie directory. Returns the path, empty on failure.
std::string SaveMovieRecording();
}  // namespace DolphinUWP::Recordings

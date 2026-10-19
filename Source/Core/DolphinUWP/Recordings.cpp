// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/Recordings.h"

#include <algorithm>
#include <array>
#include <ctime>

#include <fmt/format.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/StringUtil.h"
#include "Common/TimeUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Wiimote.h"
#include "Core/Movie.h"
#include "Core/System.h"

namespace DolphinUWP::Recordings
{
namespace
{
std::vector<RecordingFile> ListFiles(const std::string& directory, const std::string& extension,
                                     bool read_movie_ids)
{
  std::vector<RecordingFile> files;
  for (const std::string& path : Common::DoFileSearch(directory, extension))
  {
    RecordingFile file;
    file.path = path;
    SplitPath(path, nullptr, &file.name, nullptr);
    if (read_movie_ids)
      file.game_id = ReadMovieGameId(path).value_or("");
    files.push_back(std::move(file));
  }
  std::ranges::sort(files, std::ranges::greater{}, &RecordingFile::name);
  return files;
}
}  // namespace

std::string GetMovieDirectory()
{
  return File::GetUserPath(D_USER_IDX) + "Movies" DIR_SEP;
}

std::string GetFifoDirectory()
{
  return File::GetUserPath(D_DUMP_IDX) + "FIFO" DIR_SEP;
}

std::string MakeFileName(std::string_view directory, std::string_view game_id,
                         std::string_view extension)
{
  const std::optional<std::tm> time = Common::LocalTime(std::time(nullptr));
  const std::string stamp =
      time ?
          fmt::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", time->tm_year + 1900, time->tm_mon + 1,
                      time->tm_mday, time->tm_hour, time->tm_min, time->tm_sec) :
          std::string("recording");
  const std::string base =
      fmt::format("{}{}_{}", directory, game_id.empty() ? "Dolphin" : game_id, stamp);
  std::string path = fmt::format("{}{}", base, extension);
  for (int index = 2; File::Exists(path); ++index)
    path = fmt::format("{}_{}{}", base, index, extension);
  return path;
}

std::vector<RecordingFile> ListMovies()
{
  return ListFiles(GetMovieDirectory(), ".dtm", true);
}

std::vector<RecordingFile> ListFifoLogs()
{
  return ListFiles(GetFifoDirectory(), ".dff", false);
}

std::optional<std::string> ReadMovieGameId(const std::string& path)
{
  File::IOFile file(path, "rb");
  Movie::DTMHeader header{};
  if (!file || !file.ReadArray(&header, 1))
    return std::nullopt;
  constexpr std::array<u8, 4> signature{'D', 'T', 'M', 0x1A};
  if (header.filetype != signature)
    return std::nullopt;
  return std::string(header.GetGameID());
}

bool BeginMovieRecording()
{
  auto& system = Core::System::GetInstance();
  auto& movie = system.GetMovie();
  if (movie.IsRecordingInput() || movie.IsPlayingInput())
    return false;

  Movie::ControllerTypeArray controllers{};
  Movie::WiimoteEnabledArray wiimotes{};
  for (int i = 0; i < 4; ++i)
  {
    const SerialInterface::SIDevices device = Config::Get(Config::GetInfoForSIDevice(i));
    if (device == SerialInterface::SIDEVICE_GC_GBA_EMULATED)
      controllers[i] = Movie::ControllerType::GBA;
    else if (SerialInterface::SIDevice_IsGCController(device))
      controllers[i] = Movie::ControllerType::GC;
    else
      controllers[i] = Movie::ControllerType::None;
    wiimotes[i] = Config::Get(Config::GetInfoForWiimoteSource(i)) != WiimoteSource::None;
  }

  movie.SetReadOnly(false);
  return movie.BeginRecordingInput(controllers, wiimotes);
}

std::string SaveMovieRecording()
{
  auto& system = Core::System::GetInstance();
  auto& movie = system.GetMovie();
  if (!movie.IsRecordingInput())
    return {};

  const std::string directory = GetMovieDirectory();
  File::CreateFullPath(directory);
  const std::string path = MakeFileName(directory, SConfig::GetInstance().GetGameID(), ".dtm");
  const Core::CPUThreadGuard guard(system);
  movie.SaveRecording(path);
  return File::Exists(path) ? path : std::string();
}
}  // namespace DolphinUWP::Recordings

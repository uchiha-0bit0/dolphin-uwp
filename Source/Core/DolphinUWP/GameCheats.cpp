// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/GameCheats.h"

#include <algorithm>
#include <cctype>
#include <variant>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/ARDecrypt.h"
#include "Core/ConfigManager.h"
#include "Core/GeckoCodeConfig.h"

namespace DolphinUWP
{
namespace
{
std::vector<std::string> Tokenize(std::string_view text)
{
  std::vector<std::string> tokens;
  std::string current;
  for (const char c : text)
  {
    if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';')
    {
      if (!current.empty())
        tokens.push_back(std::move(current));
      current.clear();
    }
    else
    {
      current.push_back(c);
    }
  }
  if (!current.empty())
    tokens.push_back(std::move(current));
  return tokens;
}

bool IsHexWord(std::string_view token)
{
  return token.size() == 8 && std::ranges::all_of(token, [](char c) {
           return std::isxdigit(static_cast<unsigned char>(c)) != 0;
         });
}

// Pairs consecutive eight-digit values into "XXXXXXXX YYYYYYYY" lines.
std::optional<std::vector<std::string>> PairWords(const std::vector<std::string>& tokens,
                                                  std::string* error)
{
  std::vector<std::string> lines;
  for (size_t i = 0; i < tokens.size(); i += 2)
  {
    if (i + 1 >= tokens.size() || !IsHexWord(tokens[i]) || !IsHexWord(tokens[i + 1]))
    {
      *error = fmt::format("\"{}\" is not part of a valid pair of eight-digit values.", tokens[i]);
      return std::nullopt;
    }
    lines.push_back(fmt::format("{} {}", tokens[i], tokens[i + 1]));
  }
  return lines;
}

std::string GetLocalIniPath(const std::string& game_id)
{
  return File::GetUserPath(D_GAMESETTINGS_IDX) + game_id + ".ini";
}
}  // namespace

void GameCheats::Load(std::string game_id, u16 revision)
{
  m_game_id = std::move(game_id);
  m_revision = revision;

  Common::IniFile local;
  local.Load(GetLocalIniPath(m_game_id));
  const Common::IniFile global = SConfig::LoadDefaultGameIni(m_game_id, m_revision);
  action_replay = ActionReplay::LoadCodes(global, local);
  gecko = Gecko::LoadCodes(global, local);
  patches.clear();
  PatchEngine::LoadPatchSection("OnFrame", &patches, global, local);
}

void GameCheats::Save() const
{
  if (m_game_id.empty())
    return;
  const std::string path = GetLocalIniPath(m_game_id);
  Common::IniFile ini;
  ini.Load(path);
  ActionReplay::SaveCodes(&ini, action_replay);
  Gecko::SaveCodes(ini, gecko);
  PatchEngine::SavePatchSection(&ini, patches);
  ini.Save(path);
}

void GameCheats::ApplyToRunningGame() const
{
  ActionReplay::ApplyCodes(action_replay, m_game_id, m_revision);
  Gecko::SetActiveCodes(gecko, m_game_id, m_revision);
}

std::optional<std::vector<ActionReplay::AREntry>> ParseActionReplayCode(std::string_view text,
                                                                        std::string* error)
{
  const std::vector<std::string> tokens = Tokenize(text);
  std::vector<std::string> lines;
  for (size_t i = 0; i < tokens.size(); ++i)
  {
    if (tokens[i].find('-') != std::string::npos)
    {
      lines.push_back(tokens[i]);
      continue;
    }
    if (i + 1 >= tokens.size() || !IsHexWord(tokens[i]) || !IsHexWord(tokens[i + 1]))
    {
      *error = fmt::format("\"{}\" is not a valid Action Replay value.", tokens[i]);
      return std::nullopt;
    }
    lines.push_back(fmt::format("{} {}", tokens[i], tokens[i + 1]));
    ++i;
  }

  std::vector<ActionReplay::AREntry> entries;
  std::vector<std::string> encrypted;
  for (const std::string& line : lines)
  {
    const auto parsed = ActionReplay::DeserializeLine(line);
    if (const auto* entry = std::get_if<ActionReplay::AREntry>(&parsed))
      entries.push_back(*entry);
    else if (const auto* line_text = std::get_if<ActionReplay::EncryptedLine>(&parsed))
      encrypted.push_back(*line_text);
    else
    {
      *error = fmt::format("\"{}\" is not a valid encrypted or decrypted code line.", line);
      return std::nullopt;
    }
  }

  if (!encrypted.empty() && !entries.empty())
  {
    *error = "The code mixes encrypted and decrypted lines.";
    return std::nullopt;
  }
  if (!encrypted.empty())
    ActionReplay::DecryptARCode(encrypted, &entries);
  if (entries.empty())
  {
    *error = "The code does not contain any lines.";
    return std::nullopt;
  }
  return entries;
}

std::optional<std::vector<Gecko::GeckoCode::Code>> ParseGeckoCode(std::string_view text,
                                                                  std::string* error)
{
  const auto lines = PairWords(Tokenize(text), error);
  if (!lines)
    return std::nullopt;

  std::vector<Gecko::GeckoCode::Code> codes;
  for (const std::string& line : *lines)
  {
    const auto code = Gecko::DeserializeLine(line);
    if (!code)
    {
      *error = fmt::format("\"{}\" is not a valid Gecko code line.", line);
      return std::nullopt;
    }
    codes.push_back(*code);
  }
  if (codes.empty())
  {
    *error = "The code does not contain any lines.";
    return std::nullopt;
  }
  return codes;
}

std::optional<std::vector<PatchEngine::PatchEntry>> ParsePatch(std::string_view text,
                                                               std::string* error)
{
  std::vector<PatchEngine::PatchEntry> entries;
  for (const std::string& token : Tokenize(text))
  {
    const auto entry = PatchEngine::DeserializeLine(token);
    if (!entry)
    {
      *error = fmt::format("\"{}\" is not a valid patch line. Use the form "
                           "0xADDRESS:byte|word|dword:0xVALUE.",
                           token);
      return std::nullopt;
    }
    entries.push_back(*entry);
  }
  if (entries.empty())
  {
    *error = "The patch does not contain any lines.";
    return std::nullopt;
  }
  return entries;
}

std::string FormatActionReplayCode(const ActionReplay::ARCode& code)
{
  std::string text;
  for (const ActionReplay::AREntry& entry : code.ops)
    text += ActionReplay::SerializeLine(entry) + "\n";
  return text;
}

std::string FormatGeckoCode(const Gecko::GeckoCode& code)
{
  std::string text;
  for (const Gecko::GeckoCode::Code& line : code.codes)
  {
    text += line.original_line.empty() ? fmt::format("{:08X} {:08X}", line.address, line.data) :
                                         line.original_line;
    text += "\n";
  }
  return text;
}

std::string FormatPatch(const PatchEngine::Patch& patch)
{
  std::string text;
  for (const PatchEngine::PatchEntry& entry : patch.entries)
    text += PatchEngine::SerializeLine(entry) + "\n";
  return text;
}
}  // namespace DolphinUWP

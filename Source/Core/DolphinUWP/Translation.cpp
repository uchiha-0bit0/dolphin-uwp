// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/Translation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

#include <UwpImGuiFrontend/Text.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Core/Config/MainSettings.h"
#include "Core/Host.h"

namespace DolphinUWP::Translation
{
namespace
{
constexpr u32 MO_MAGIC_NUMBER = 0x950412de;

constexpr std::array<std::pair<const char*, const char*>, 29> LANGUAGES{{
    {"ms", "Bahasa Melayu"}, {"ca", "Català"},
    {"cs", "Čeština"},       {"da", "Dansk"},
    {"de", "Deutsch"},       {"en", "English"},
    {"es", "Español"},       {"fr", "Français"},
    {"hr", "Hrvatski"},      {"it", "Italiano"},
    {"hu", "Magyar"},        {"nl", "Nederlands"},
    {"nb", "Norsk bokmål"},  {"pl", "Polski"},
    {"pt", "Português"},     {"pt_BR", "Português (Brasil)"},
    {"ro", "Română"},        {"sr", "Srpski"},
    {"fi", "Suomi"},         {"sv", "Svenska"},
    {"tr", "Türkçe"},        {"el", "Ελληνικά"},
    {"ru", "Русский"},       {"ar", "العربية"},
    {"fa", "فارسی"},         {"ko", "한국어"},
    {"ja", "日本語"},        {"zh_CN", "简体中文"},
    {"zh_TW", "繁體中文"},
}};

u32 ReadU32(const std::vector<char>& data, size_t offset)
{
  u32 value = 0;
  if (offset + sizeof(value) <= data.size())
    std::memcpy(&value, &data[offset], sizeof(value));
  return value;
}

class Catalog
{
public:
  static std::unique_ptr<Catalog> Load(const std::string& path)
  {
    File::IOFile file(path, "rb");
    if (!file.IsOpen())
      return nullptr;
    auto catalog = std::make_unique<Catalog>();
    catalog->m_data.resize(file.GetSize());
    if (catalog->m_data.size() < 20 ||
        !file.ReadBytes(catalog->m_data.data(), catalog->m_data.size()) ||
        ReadU32(catalog->m_data, 0) != MO_MAGIC_NUMBER || (ReadU32(catalog->m_data, 4) >> 16) > 1)
    {
      ERROR_LOG_FMT(COMMON, "The translation file {} isn't valid", path);
      return nullptr;
    }
    catalog->m_count = ReadU32(catalog->m_data, 8);
    catalog->m_originals = ReadU32(catalog->m_data, 12);
    catalog->m_translations = ReadU32(catalog->m_data, 16);
    return catalog;
  }

  std::optional<std::string_view> Translate(std::string_view text) const
  {
    u32 low = 0;
    u32 high = m_count;
    while (low < high)
    {
      const u32 middle = low + (high - low) / 2;
      const std::string_view original = String(m_originals, middle);
      if (original == text)
      {
        const std::string_view translation = String(m_translations, middle);
        return translation.empty() ? std::nullopt : std::optional(translation);
      }
      if (original < text)
        low = middle + 1;
      else
        high = middle;
    }
    return std::nullopt;
  }

private:
  std::string_view String(u32 table, u32 index) const
  {
    const u32 length = ReadU32(m_data, table + index * 8);
    const u32 offset = ReadU32(m_data, table + index * 8 + 4);
    if (static_cast<size_t>(offset) + length > m_data.size())
      return {};
    return {&m_data[offset], length};
  }

  std::vector<char> m_data;
  u32 m_count = 0;
  u32 m_originals = 0;
  u32 m_translations = 0;
};

constexpr size_t MAX_UNTRANSLATED = 4096;

std::mutex s_mutex;
std::unique_ptr<Catalog> s_catalog;
// The views that Translate returns point into these strings. The previous language's are kept
// for the frame that changes the language.
std::unordered_map<std::string, std::string> s_translations;
std::unordered_map<std::string, std::string> s_previous_translations;
// Texts without a translation, which include every changing value that is drawn.
std::unordered_set<std::string> s_untranslated;

std::string CatalogPath(std::string_view code)
{
  return fmt::format("{}/Languages/{}/dolphin-emu.mo", File::GetExeDirectory(), code);
}

// zh-Hans-CN tries zh_Hans_CN, zh_Hans, zh_CN and zh.
std::vector<std::string> CandidateCodes(std::string tag)
{
  std::ranges::replace(tag, '-', '_');
  std::vector<std::string> codes{tag};
  while (tag.find('_') != std::string::npos)
  {
    tag.erase(tag.rfind('_'));
    codes.push_back(tag);
  }
  for (const auto& [script, region] :
       {std::pair{"zh_Hans", "zh_CN"}, std::pair{"zh_Hant", "zh_TW"}})
  {
    const auto found = std::ranges::find(codes, script);
    if (found != codes.end())
      codes.insert(found + 1, region);
  }
  return codes;
}

// The UWP labels are in sentence case, and the same labels of DolphinQt are in title case.
std::string TitleCase(std::string_view text)
{
  std::string result(text);
  bool word_start = true;
  for (char& c : result)
  {
    if (word_start && std::islower(static_cast<unsigned char>(c)))
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    word_start = c == ' ' || c == '-' || c == '(' || c == '/';
  }
  return result;
}

std::optional<std::string_view> Find(std::string_view text)
{
  if (const auto translation = s_catalog->Translate(text))
    return translation;
  const std::string title = TitleCase(text);
  if (title != text)
  {
    if (const auto translation = s_catalog->Translate(title))
      return translation;
  }
  // The catalogs' labels next to a value end with a colon.
  return s_catalog->Translate(title + ':');
}

std::optional<std::string> Lookup(std::string_view text)
{
  if (!s_catalog)
    return std::nullopt;
  // Toggles show "On" next to "Off", and the catalogs only translate "Off".
  if (text == "Off" && !s_catalog->Translate("On"))
    return std::nullopt;
  const std::optional<std::string_view> translation = Find(text);
  if (!translation)
    return std::nullopt;
  // The text shows the colon itself where it needs one.
  std::string result(*translation);
  if (!text.ends_with(':'))
  {
    while (!result.empty() && (result.back() == ':' || result.back() == ' '))
      result.pop_back();
  }
  return result.empty() ? std::nullopt : std::optional(std::move(result));
}
}  // namespace

std::vector<std::pair<std::string, std::string>> GetLanguages()
{
  std::vector<std::pair<std::string, std::string>> languages;
  for (const auto& [code, name] : LANGUAGES)
  {
    if (std::string_view(code) == "en" || File::Exists(CatalogPath(code)))
      languages.emplace_back(code, name);
  }
  return languages;
}

void Apply()
{
  std::unique_ptr<Catalog> catalog;
  std::vector<std::string> tags;
  if (const std::string configured = Config::Get(Config::MAIN_INTERFACE_LANGUAGE);
      !configured.empty())
  {
    tags.push_back(configured);
  }
  else
  {
    tags = Host_GetPreferredLocales();
  }
  for (const std::string& tag : tags)
  {
    for (const std::string& code : CandidateCodes(tag))
    {
      catalog = Catalog::Load(CatalogPath(code));
      if (catalog)
        break;
    }
    // English is the language of the text itself.
    if (catalog || tag.starts_with("en"))
      break;
  }

  {
    std::lock_guard lock(s_mutex);
    s_catalog = std::move(catalog);
    s_previous_translations = std::move(s_translations);
    s_translations.clear();
    s_untranslated.clear();
  }
  Common::RegisterStringTranslator([](const char* text) { return std::string(Translate(text)); });
  UwpImGuiFrontend::SetTextTranslator([](std::string_view text) { return Translate(text); });
}

std::string_view Translate(std::string_view text)
{
  std::lock_guard lock(s_mutex);
  if (!s_catalog || text.empty())
    return text;
  std::string key(text);
  if (const auto found = s_translations.find(key); found != s_translations.end())
    return found->second;
  if (s_untranslated.contains(key))
    return text;
  std::optional<std::string> translation = Lookup(text);
  if (!translation)
  {
    if (s_untranslated.size() >= MAX_UNTRANSLATED)
      s_untranslated.clear();
    s_untranslated.insert(std::move(key));
    return text;
  }
  return s_translations.emplace(std::move(key), std::move(*translation)).first->second;
}
}  // namespace DolphinUWP::Translation

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/LogBuffer.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>

#include "Common/Logging/LogManager.h"

namespace DolphinUWP::LogBuffer
{
namespace
{
constexpr size_t CAPACITY = 1000;

std::mutex s_mutex;
std::deque<Entry> s_entries;
std::atomic<u64> s_version = 0;

class Listener final : public Common::Log::LogListener
{
public:
  void Log(Common::Log::LogLevel level, const char* message) override
  {
    std::string_view text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
      text.remove_suffix(1);

    std::lock_guard lock(s_mutex);
    if (s_entries.size() == CAPACITY)
      s_entries.pop_front();
    s_entries.push_back({level, std::string(text)});
    ++s_version;
  }
};
}  // namespace

void Install()
{
  auto* const manager = Common::Log::LogManager::GetInstance();
  manager->RegisterListener(Common::Log::LogListener::LOG_WINDOW_LISTENER,
                            std::make_unique<Listener>());
  manager->EnableListener(Common::Log::LogListener::LOG_WINDOW_LISTENER, true);
}

std::vector<Entry> GetEntries()
{
  std::lock_guard lock(s_mutex);
  return {s_entries.begin(), s_entries.end()};
}

u64 GetVersion()
{
  return s_version;
}

void Clear()
{
  std::lock_guard lock(s_mutex);
  s_entries.clear();
  ++s_version;
}
}  // namespace DolphinUWP::LogBuffer

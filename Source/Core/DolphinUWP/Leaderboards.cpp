// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/Leaderboards.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <rcheevos/include/rc_client.h>

#include "Core/AchievementManager.h"

namespace DolphinUWP::Leaderboards
{
namespace
{
constexpr u32 TOP_ENTRIES = 10;
constexpr u32 ENTRIES_AROUND_PLAYER = 11;

struct Cache
{
  int pending = 0;
  bool failed = false;
  // By the position in the leaderboard, which unlike the rank is unique.
  std::map<u32, Entry> entries;
};

std::mutex s_mutex;
std::unordered_map<u32, Cache> s_boards;
std::atomic<bool> s_changed = false;

void Received(int result, const char*, rc_client_leaderboard_entry_list_t* list, rc_client_t*,
              void* userdata)
{
  const std::unique_ptr<u32> id(static_cast<u32*>(userdata));
  {
    std::lock_guard lock(s_mutex);
    Cache& cache = s_boards[*id];
    cache.pending = std::max(cache.pending - 1, 0);
    if (result != RC_OK || !list)
    {
      cache.failed = cache.entries.empty();
    }
    else
    {
      for (u32 i = 0; i < list->num_entries; ++i)
      {
        const rc_client_leaderboard_entry_t& entry = list->entries[i];
        cache.entries[entry.index] = {entry.rank, entry.user ? entry.user : "", entry.display,
                                      static_cast<s32>(i) == list->user_index};
      }
    }
  }
  if (list)
    rc_client_destroy_leaderboard_entry_list(list);
  s_changed = true;
}
}  // namespace

bool Available()
{
  AchievementManager& achievements = AchievementManager::GetInstance();
  std::lock_guard lock(achievements.GetLock());
  rc_client_t* const client = achievements.GetClient();
  return client && achievements.IsGameLoaded() && rc_client_has_leaderboards(client) != 0;
}

std::vector<Board> List()
{
  std::vector<Board> boards;
  AchievementManager& achievements = AchievementManager::GetInstance();
  std::lock_guard lock(achievements.GetLock());
  rc_client_t* const client = achievements.GetClient();
  if (!client || !achievements.IsGameLoaded())
    return boards;

  rc_client_leaderboard_list_t* const list =
      rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_TRACKING);
  if (!list)
    return boards;
  for (u32 bucket = 0; bucket < list->num_buckets; ++bucket)
  {
    const rc_client_leaderboard_bucket_t& group = list->buckets[bucket];
    for (u32 i = 0; i < group.num_leaderboards; ++i)
    {
      const rc_client_leaderboard_t& board = *group.leaderboards[i];
      boards.push_back({board.id, group.label ? group.label : "", board.title ? board.title : "",
                        board.description ? board.description : "",
                        board.state == RC_CLIENT_LEADERBOARD_STATE_TRACKING && board.tracker_value ?
                            board.tracker_value :
                            ""});
    }
  }
  rc_client_destroy_leaderboard_list(list);
  return boards;
}

void Fetch(u32 id)
{
  AchievementManager& achievements = AchievementManager::GetInstance();
  std::lock_guard achievements_lock(achievements.GetLock());
  rc_client_t* const client = achievements.GetClient();
  if (!client)
    return;
  {
    std::lock_guard lock(s_mutex);
    Cache& cache = s_boards[id];
    if (cache.pending > 0)
      return;
    cache = {};
    cache.pending = 2;
  }
  rc_client_begin_fetch_leaderboard_entries(client, id, 1, TOP_ENTRIES, Received, new u32(id));
  rc_client_begin_fetch_leaderboard_entries_around_user(client, id, ENTRIES_AROUND_PLAYER, Received,
                                                        new u32(id));
}

Entries Get(u32 id)
{
  std::lock_guard lock(s_mutex);
  const auto found = s_boards.find(id);
  if (found == s_boards.end())
    return {};
  Entries entries;
  entries.loading = found->second.pending > 0;
  entries.failed = found->second.failed;
  for (const auto& [index, entry] : found->second.entries)
    entries.entries.push_back(entry);
  return entries;
}

bool ConsumeChanged()
{
  return s_changed.exchange(false);
}
}  // namespace DolphinUWP::Leaderboards

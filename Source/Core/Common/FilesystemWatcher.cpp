// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/FilesystemWatcher.h"

#ifdef DOLPHIN_UWP
#include <array>
#include <functional>
#include <thread>

#include <windows.h>

#include "Common/CommonFuncs.h"
#include "Common/Thread.h"
#include "Common/UWP/FileSystem.h"
#else
#include <wtr/watcher.hpp>
#endif

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

namespace Common
{
#ifdef DOLPHIN_UWP
namespace UWP
{
// Watches a directory tree through a brokered handle, so picker-granted folders work as well.
class DirectoryWatch
{
public:
  enum class Action
  {
    Added,
    Modified,
    Renamed,
    Deleted,
  };

  using Callback =
      std::function<void(Action action, const std::string& path, const std::string& new_path)>;

  DirectoryWatch(std::string path, Callback callback)
      : m_path(std::move(path)), m_callback(std::move(callback))
  {
    m_directory = OpenFileHandle(m_path, FILE_LIST_DIRECTORY,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED);
    if (m_directory == INVALID_HANDLE_VALUE)
    {
      ERROR_LOG_FMT(COMMON, "Filesystem watcher: failed to open '{}': {}", m_path,
                    GetLastErrorString());
      return;
    }

    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event || !m_io_event)
    {
      ERROR_LOG_FMT(COMMON, "Filesystem watcher: failed to create events: {}",
                    GetLastErrorString());
      return;
    }

    m_thread = std::thread([this] {
      SetCurrentThreadName("Filesystem Watcher");
      Run();
    });
  }

  ~DirectoryWatch()
  {
    if (m_thread.joinable())
    {
      SetEvent(m_stop_event);
      m_thread.join();
    }
    if (m_io_event)
      CloseHandle(m_io_event);
    if (m_stop_event)
      CloseHandle(m_stop_event);
    if (m_directory != INVALID_HANDLE_VALUE)
      CloseHandle(m_directory);
  }

  DirectoryWatch(const DirectoryWatch&) = delete;
  DirectoryWatch& operator=(const DirectoryWatch&) = delete;

private:
  static constexpr DWORD NOTIFY_FILTER =
      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
      FILE_NOTIFY_CHANGE_SECURITY;

  void Run()
  {
    alignas(DWORD) std::array<u8, 64 * 1024> buffer;
    std::string renamed_from;

    while (true)
    {
      OVERLAPPED overlapped{};
      overlapped.hEvent = m_io_event;
      ResetEvent(m_io_event);
      if (!ReadDirectoryChangesW(m_directory, buffer.data(), static_cast<DWORD>(buffer.size()),
                                 TRUE, NOTIFY_FILTER, nullptr, &overlapped, nullptr))
      {
        ERROR_LOG_FMT(COMMON, "Filesystem watcher: failed to read changes in '{}': {}", m_path,
                      GetLastErrorString());
        return;
      }

      const std::array<HANDLE, 2> handles{m_io_event, m_stop_event};
      DWORD bytes = 0;
      if (WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE,
                                 INFINITE) != WAIT_OBJECT_0)
      {
        CancelIoEx(m_directory, &overlapped);
        GetOverlappedResult(m_directory, &overlapped, &bytes, TRUE);
        return;
      }

      if (!GetOverlappedResult(m_directory, &overlapped, &bytes, FALSE))
      {
        ERROR_LOG_FMT(COMMON, "Filesystem watcher: failed to receive changes in '{}': {}", m_path,
                      GetLastErrorString());
        return;
      }

      if (bytes == 0)
      {
        WARN_LOG_FMT(COMMON, "Filesystem watcher: change buffer overflowed for '{}'", m_path);
        continue;
      }

      for (size_t offset = 0;;)
      {
        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(&buffer[offset]);
        const std::string path = MakePath(
            std::wstring_view(info->FileName, info->FileNameLength / sizeof(wchar_t)));

        switch (info->Action)
        {
        case FILE_ACTION_ADDED:
          m_callback(Action::Added, path, {});
          break;
        case FILE_ACTION_MODIFIED:
          m_callback(Action::Modified, path, {});
          break;
        case FILE_ACTION_REMOVED:
          m_callback(Action::Deleted, path, {});
          break;
        case FILE_ACTION_RENAMED_OLD_NAME:
          renamed_from = path;
          break;
        case FILE_ACTION_RENAMED_NEW_NAME:
          if (renamed_from.empty())
            WARN_LOG_FMT(COMMON, "Rename on path '{}' seen without association!", path);
          else
            m_callback(Action::Renamed, std::exchange(renamed_from, {}), path);
          break;
        default:
          break;
        }

        if (info->NextEntryOffset == 0)
          break;
        offset += info->NextEntryOffset;
      }
    }
  }

  std::string MakePath(std::wstring_view name) const
  {
    std::string path = m_path;
    if (!path.empty() && path.back() != '/' && path.back() != '\\')
      path += '/';
    path += WStringToUTF8(name);
    return WithUnifiedPathSeparators(path);
  }

  std::string m_path;
  Callback m_callback;
  HANDLE m_directory = INVALID_HANDLE_VALUE;
  HANDLE m_stop_event = nullptr;
  HANDLE m_io_event = nullptr;
  std::thread m_thread;
};
}  // namespace UWP
#endif

FilesystemWatcher::FilesystemWatcher() = default;
FilesystemWatcher::~FilesystemWatcher() = default;

#ifdef DOLPHIN_UWP
void FilesystemWatcher::Watch(const std::string& path)
{
  const auto [iter, inserted] = m_watched_paths.try_emplace(path, nullptr);
  if (!inserted)
    return;

  using Action = UWP::DirectoryWatch::Action;
  iter->second = std::make_unique<UWP::DirectoryWatch>(
      path, [this](Action action, const std::string& changed_path, const std::string& new_path) {
        switch (action)
        {
        case Action::Added:
          PathAdded(changed_path);
          break;
        case Action::Modified:
          PathModified(changed_path);
          break;
        case Action::Renamed:
          PathRenamed(changed_path, new_path);
          break;
        case Action::Deleted:
          PathDeleted(changed_path);
          break;
        }
      });
}

void FilesystemWatcher::Unwatch(const std::string& path)
{
  m_watched_paths.erase(path);
}
#else
void FilesystemWatcher::Watch(const std::string& path)
{
  const auto [iter, inserted] = m_watched_paths.try_emplace(path, nullptr);
  if (inserted)
  {
    iter->second = std::make_unique<wtr::watch>(path, [this](wtr::event e) {
      const auto watched_path = PathToString(e.path_name);
      if (e.path_type == wtr::event::path_type::watcher)
      {
        if (watched_path.starts_with('e'))
          ERROR_LOG_FMT(COMMON, "Filesystem watcher: '{}'", watched_path);
        else if (watched_path.starts_with('w'))
          WARN_LOG_FMT(COMMON, "Filesystem watcher: '{}'", watched_path);
        return;
      }

      if (e.effect_type == wtr::event::effect_type::create)
      {
        const auto unified_path = WithUnifiedPathSeparators(watched_path);
        PathAdded(unified_path);
      }
      else if (e.effect_type == wtr::event::effect_type::modify)
      {
        const auto unified_path = WithUnifiedPathSeparators(watched_path);
        PathModified(unified_path);
      }
      else if (e.effect_type == wtr::event::effect_type::rename)
      {
        if (!e.associated)
        {
          WARN_LOG_FMT(COMMON, "Rename on path '{}' seen without association!", watched_path);
          return;
        }

        const auto old_path = WithUnifiedPathSeparators(watched_path);
        const auto new_path = WithUnifiedPathSeparators(PathToString(e.associated->path_name));
        PathRenamed(old_path, new_path);
      }
      else if (e.effect_type == wtr::event::effect_type::destroy)
      {
        const auto unified_path = WithUnifiedPathSeparators(watched_path);
        PathDeleted(unified_path);
      }
    });
  }
}

void FilesystemWatcher::Unwatch(const std::string& path)
{
  m_watched_paths.erase(path);
}
#endif
}  // namespace Common

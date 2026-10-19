// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/UWP/FileSystem.h"

#include <algorithm>
#include <vector>

#include <fcntl.h>
#include <fileapifromapp.h>
#include <io.h>

#include "Common/CommonTypes.h"
#include "Common/StringUtil.h"

namespace Common::UWP
{
std::wstring ToNativePath(std::string_view path)
{
  std::wstring native = UTF8ToWString(path);
  std::ranges::replace(native, L'/', L'\\');
  return native;
}

HANDLE OpenFileHandle(std::string_view path, DWORD access, DWORD share, DWORD disposition,
                      DWORD flags)
{
  CREATEFILE2_EXTENDED_PARAMETERS parameters{};
  parameters.dwSize = sizeof(parameters);
  parameters.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
  parameters.dwFileFlags = flags;
  return CreateFile2FromAppW(ToNativePath(path).c_str(), access, share, disposition, &parameters);
}

std::FILE* OpenCFile(std::string_view path, const char* mode, DWORD share)
{
  const std::string_view mode_view(mode);
  const bool update = mode_view.contains('+');

  DWORD access;
  DWORD disposition;
  int flags = mode_view.contains('b') ? _O_BINARY : _O_TEXT;
  switch (mode_view.empty() ? '\0' : mode_view.front())
  {
  case 'r':
    access = update ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    disposition = OPEN_EXISTING;
    flags |= update ? _O_RDWR : _O_RDONLY;
    break;
  case 'w':
    access = update ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE;
    disposition = CREATE_ALWAYS;
    flags |= update ? _O_RDWR : _O_WRONLY;
    break;
  case 'a':
    access = update ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE;
    disposition = OPEN_ALWAYS;
    flags |= (update ? _O_RDWR : _O_WRONLY) | _O_APPEND;
    break;
  default:
    return nullptr;
  }

  const HANDLE handle = OpenFileHandle(path, access, share, disposition);
  if (handle == INVALID_HANDLE_VALUE)
    return nullptr;

  const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), flags);
  if (fd == -1)
  {
    CloseHandle(handle);
    return nullptr;
  }

  std::FILE* const file = _fdopen(fd, mode);
  if (!file)
    _close(fd);
  return file;
}

bool QueryFileAttributes(std::string_view path, WIN32_FILE_ATTRIBUTE_DATA* data)
{
  std::wstring native = ToNativePath(path);
  while (native.size() > 3 && native.back() == L'\\')
    native.pop_back();
  return GetFileAttributesExFromAppW(native.c_str(), GetFileExInfoStandard, data) != FALSE;
}

bool RemoveFile(std::string_view path)
{
  return DeleteFileFromAppW(ToNativePath(path).c_str()) != FALSE;
}

bool MakeDirectory(std::string_view path)
{
  if (CreateDirectoryFromAppW(ToNativePath(path).c_str(), nullptr))
    return true;

  WIN32_FILE_ATTRIBUTE_DATA data;
  return GetLastError() == ERROR_ALREADY_EXISTS && QueryFileAttributes(path, &data) &&
         (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool RemoveEmptyDirectory(std::string_view path)
{
  return RemoveDirectoryFromAppW(ToNativePath(path).c_str()) != FALSE;
}

bool RenamePath(std::string_view source, std::string_view destination)
{
  const std::wstring native_source = ToNativePath(source);
  const std::wstring native_destination = ToNativePath(destination);
  if (MoveFileFromAppW(native_source.c_str(), native_destination.c_str()))
    return true;

  const DWORD error = GetLastError();
  if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
    return false;
  return ReplaceFileFromAppW(native_destination.c_str(), native_source.c_str(), nullptr,
                             REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
}

// CopyFileFromAppW is denied for some brokered sources, so the data is copied through handles.
bool CopyFileContents(std::string_view source, std::string_view destination, bool overwrite)
{
  const HANDLE input = OpenFileHandle(source, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN);
  if (input == INVALID_HANDLE_VALUE)
    return false;
  const HANDLE output =
      OpenFileHandle(destination, GENERIC_WRITE, 0, overwrite ? CREATE_ALWAYS : CREATE_NEW);
  if (output == INVALID_HANDLE_VALUE)
  {
    const DWORD error = GetLastError();
    CloseHandle(input);
    SetLastError(error);
    return false;
  }

  std::vector<u8> buffer(1024 * 1024);
  bool success = true;
  while (success)
  {
    DWORD read = 0;
    success = ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr);
    if (!success || read == 0)
      break;
    DWORD written = 0;
    success = WriteFile(output, buffer.data(), read, &written, nullptr) && written == read;
  }

  const DWORD error = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(output);
  CloseHandle(input);
  if (!success)
  {
    DeleteFileFromAppW(ToNativePath(destination).c_str());
    SetLastError(error);
  }
  return success;
}

void ForEachDirectoryEntry(std::string_view directory, bool recursive,
                           const std::function<void(const std::wstring&, bool)>& callback)
{
  std::wstring root = ToNativePath(directory);
  while (!root.empty() && root.back() == L'\\')
    root.pop_back();

  std::vector<std::wstring> pending{std::move(root)};
  while (!pending.empty())
  {
    const std::wstring current = std::move(pending.back());
    pending.pop_back();

    WIN32_FIND_DATAW find_data;
    const HANDLE find = FindFirstFileExFromAppW((current + L"\\*").c_str(), FindExInfoBasic,
                                                &find_data, FindExSearchNameMatch, nullptr,
                                                FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
      continue;

    do
    {
      const std::wstring_view name = find_data.cFileName;
      if (name == L"." || name == L"..")
        continue;

      std::wstring path = current + L'\\' + find_data.cFileName;
      const bool is_directory = (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      callback(path, is_directory);

      if (recursive && is_directory &&
          (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
      {
        pending.push_back(std::move(path));
      }
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
  }
}
}  // namespace Common::UWP

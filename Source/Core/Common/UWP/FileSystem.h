// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <string_view>

#include <windows.h>

// Brokered file access, so paths granted through pickers or broadFileSystemAccess are reachable.
namespace Common::UWP
{
std::wstring ToNativePath(std::string_view path);

HANDLE OpenFileHandle(std::string_view path, DWORD access, DWORD share, DWORD disposition,
                      DWORD flags = 0);
std::FILE* OpenCFile(std::string_view path, const char* mode, DWORD share);
bool QueryFileAttributes(std::string_view path, WIN32_FILE_ATTRIBUTE_DATA* data);
bool RemoveFile(std::string_view path);
// Succeeds when the directory already exists.
bool MakeDirectory(std::string_view path);
bool RemoveEmptyDirectory(std::string_view path);
// Replaces an existing destination file, like std::filesystem::rename.
bool RenamePath(std::string_view source, std::string_view destination);
bool CopyFileContents(std::string_view source, std::string_view destination, bool overwrite);
void ForEachDirectoryEntry(std::string_view directory, bool recursive,
                           const std::function<void(const std::wstring&, bool)>& callback);
}  // namespace Common::UWP

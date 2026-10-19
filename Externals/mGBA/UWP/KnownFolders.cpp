// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "KnownFolders.h"

#include <cstring>

#include <combaseapi.h>
#include <winrt/Windows.Storage.h>

extern "C" const GUID DOLPHIN_UWP_FOLDERID_RoamingAppData = {
    0x3EB685DB,
    0x65F9,
    0x4CF6,
    {0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D}};

extern "C" HRESULT DolphinUWP_SHGetKnownFolderPath(const GUID *, DWORD, HANDLE,
                                                   PWSTR *path) {
  *path = nullptr;
  try {
    const winrt::hstring folder =
        winrt::Windows::Storage::ApplicationData::Current()
            .LocalFolder()
            .Path();
    const size_t bytes = (folder.size() + 1) * sizeof(wchar_t);
    *path = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
    if (!*path)
      return E_OUTOFMEMORY;
    std::memcpy(*path, folder.c_str(), bytes);
    return S_OK;
  } catch (const winrt::hresult_error &error) {
    return error.code();
  }
}

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// UWP apps cannot use the shell's known folders, so mGBA's configuration
// directory resolves to the app's local folder instead.

#pragma once

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const GUID DOLPHIN_UWP_FOLDERID_RoamingAppData;
HRESULT DolphinUWP_SHGetKnownFolderPath(const GUID *folder, DWORD flags,
                                        HANDLE token, PWSTR *path);

#ifdef __cplusplus
}
#endif

#define FOLDERID_RoamingAppData DOLPHIN_UWP_FOLDERID_RoamingAppData
#define SHGetKnownFolderPath DolphinUWP_SHGetKnownFolderPath

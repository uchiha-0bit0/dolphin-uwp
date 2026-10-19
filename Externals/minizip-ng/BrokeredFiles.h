// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// UWP apps open files outside their package only through brokered functions.
#include <windows.h>

#include <fileapifromapp.h>

#define CreateFile2 CreateFile2FromAppW

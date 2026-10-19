// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <windows.h>

// The app partition headers redirect these to the *FromApp variants, which reject executable
// protections. These bind the kernel32 exports directly.
namespace Common::Kernel32
{
LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect);
BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect);
HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect,
                          DWORD maximum_size_high, DWORD maximum_size_low, LPCWSTR name);
LPVOID MapViewOfFileEx(HANDLE file_mapping, DWORD desired_access, DWORD offset_high,
                       DWORD offset_low, SIZE_T size, LPVOID base_address);
PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler);
ULONG RemoveVectoredExceptionHandler(PVOID handle);
BOOL SetThreadStackGuarantee(PULONG stack_size);
}  // namespace Common::Kernel32

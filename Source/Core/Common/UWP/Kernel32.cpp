// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/UWP/Kernel32.h"

namespace Common::Kernel32
{
namespace
{
struct Functions
{
  Functions()
  {
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32)
      return;

    Bind(kernel32, "VirtualAlloc", virtual_alloc);
    Bind(kernel32, "VirtualProtect", virtual_protect);
    Bind(kernel32, "CreateFileMappingW", create_file_mapping);
    Bind(kernel32, "MapViewOfFileEx", map_view_of_file_ex);
    Bind(kernel32, "AddVectoredExceptionHandler", add_vectored_exception_handler);
    Bind(kernel32, "RemoveVectoredExceptionHandler", remove_vectored_exception_handler);
    Bind(kernel32, "SetThreadStackGuarantee", set_thread_stack_guarantee);
  }

  template <typename T>
  static void Bind(HMODULE module, const char* name, T& function)
  {
    function = reinterpret_cast<T>(GetProcAddress(module, name));
  }

  LPVOID(WINAPI* virtual_alloc)(LPVOID, SIZE_T, DWORD, DWORD) = nullptr;
  BOOL(WINAPI* virtual_protect)(LPVOID, SIZE_T, DWORD, PDWORD) = nullptr;
  HANDLE(WINAPI* create_file_mapping)
  (HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR) = nullptr;
  LPVOID(WINAPI* map_view_of_file_ex)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, LPVOID) = nullptr;
  PVOID(WINAPI* add_vectored_exception_handler)(ULONG, PVECTORED_EXCEPTION_HANDLER) = nullptr;
  ULONG(WINAPI* remove_vectored_exception_handler)(PVOID) = nullptr;
  BOOL(WINAPI* set_thread_stack_guarantee)(PULONG) = nullptr;
};

const Functions& GetFunctions()
{
  static const Functions functions;
  return functions;
}

template <typename Result>
Result Unavailable(Result result)
{
  SetLastError(ERROR_PROC_NOT_FOUND);
  return result;
}
}  // namespace

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocation_type, DWORD protect)
{
  const auto function = GetFunctions().virtual_alloc;
  return function ? function(address, size, allocation_type, protect) : Unavailable(LPVOID{});
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect)
{
  const auto function = GetFunctions().virtual_protect;
  return function ? function(address, size, new_protect, old_protect) : Unavailable(FALSE);
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES attributes, DWORD protect,
                          DWORD maximum_size_high, DWORD maximum_size_low, LPCWSTR name)
{
  const auto function = GetFunctions().create_file_mapping;
  return function ? function(file, attributes, protect, maximum_size_high, maximum_size_low, name) :
                    Unavailable(HANDLE{});
}

LPVOID MapViewOfFileEx(HANDLE file_mapping, DWORD desired_access, DWORD offset_high,
                       DWORD offset_low, SIZE_T size, LPVOID base_address)
{
  const auto function = GetFunctions().map_view_of_file_ex;
  return function ?
             function(file_mapping, desired_access, offset_high, offset_low, size, base_address) :
             Unavailable(LPVOID{});
}

PVOID AddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)
{
  const auto function = GetFunctions().add_vectored_exception_handler;
  return function ? function(first, handler) : Unavailable(PVOID{});
}

ULONG RemoveVectoredExceptionHandler(PVOID handle)
{
  const auto function = GetFunctions().remove_vectored_exception_handler;
  return function ? function(handle) : Unavailable(ULONG{});
}

BOOL SetThreadStackGuarantee(PULONG stack_size)
{
  const auto function = GetFunctions().set_thread_stack_guarantee;
  return function ? function(stack_size) : Unavailable(FALSE);
}
}  // namespace Common::Kernel32

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/CrashHandler.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

#include <Windows.h>

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <winrt/Windows.Storage.h>

#include "Common/CommonTypes.h"
#include "Common/Version.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace DolphinUWP
{
namespace
{
// The SDK only declares MiniDumpWriteDump for desktop apps, so it is loaded from the packaged
// dbgcore.dll.
constexpr DWORD DUMP_WITH_DATA_SEGS = 0x00000001;
constexpr DWORD DUMP_SCAN_MEMORY = 0x00000010;
constexpr DWORD DUMP_WITH_UNLOADED_MODULES = 0x00000020;
constexpr DWORD DUMP_WITH_INDIRECTLY_REFERENCED_MEMORY = 0x00000040;
constexpr DWORD DUMP_WITH_FULL_MEMORY_INFO = 0x00000800;
constexpr DWORD DUMP_WITH_THREAD_INFO = 0x00001000;
constexpr DWORD DUMP_TYPE = DUMP_WITH_DATA_SEGS | DUMP_SCAN_MEMORY | DUMP_WITH_UNLOADED_MODULES |
                            DUMP_WITH_INDIRECTLY_REFERENCED_MEMORY | DUMP_WITH_FULL_MEMORY_INFO |
                            DUMP_WITH_THREAD_INFO;

#pragma pack(push, 4)
struct DumpExceptionInformation
{
  DWORD thread_id;
  EXCEPTION_POINTERS* exception_pointers;
  BOOL client_pointers;
};
#pragma pack(pop)

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE process, DWORD process_id, HANDLE file,
                                          DWORD dump_type,
                                          const DumpExceptionInformation* exception_information,
                                          const void* user_streams, const void* callback);

MiniDumpWriteDumpFn s_write_dump = nullptr;
std::wstring s_dump_prefix;
std::atomic_bool s_crash_handled{false};

struct DumpRequest
{
  std::wstring path;
  DWORD thread_id = 0;
  EXCEPTION_POINTERS* pointers = nullptr;
};

// Runs on its own thread so that a stack overflow on the crashing thread does not stop the dump.
DWORD WINAPI WriteDumpThread(LPVOID parameter)
{
  const auto* request = static_cast<const DumpRequest*>(parameter);
  const HANDLE file =
      CreateFile2(request->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return 0;
  const DumpExceptionInformation information{request->thread_id, request->pointers, FALSE};
  s_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, DUMP_TYPE,
               request->pointers ? &information : nullptr, nullptr, nullptr);
  CloseHandle(file);
  return 0;
}

void WriteReport(const std::wstring& path, std::string_view reason, EXCEPTION_POINTERS* pointers)
{
  const u64 image_base = reinterpret_cast<u64>(&__ImageBase);
  std::string report = fmt::format("Dolphin {}\n{}\nDolphinUWP.exe at 0x{:x}\n",
                                   Common::GetScmDescStr(), reason, image_base);
  if (pointers && pointers->ExceptionRecord)
  {
    const EXCEPTION_RECORD& record = *pointers->ExceptionRecord;
    const u64 address = reinterpret_cast<u64>(record.ExceptionAddress);
    report += fmt::format("Exception 0x{:08x} at 0x{:x} (DolphinUWP.exe+0x{:x})\n",
                          static_cast<u32>(record.ExceptionCode), address, address - image_base);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
    {
      const ULONG_PTR access = record.ExceptionInformation[0];
      report += fmt::format("{} of address 0x{:x}\n",
                            access == 0 ? "Read" : (access == 8 ? "Execute" : "Write"),
                            static_cast<u64>(record.ExceptionInformation[1]));
    }
  }

  const HANDLE file = CreateFile2(path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
  CloseHandle(file);
}

void HandleCrash(std::string_view reason, EXCEPTION_POINTERS* pointers)
{
  if (s_crash_handled.exchange(true) || s_dump_prefix.empty())
    return;

  const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  const std::wstring base =
      s_dump_prefix + winrt::to_hstring(fmt::format("{:%Y%m%d_%H%M%S}", now)).c_str();
  WriteReport(base + L".txt", reason, pointers);
  if (!s_write_dump)
    return;

  DumpRequest request{base + L".dmp", GetCurrentThreadId(), pointers};
  if (const HANDLE thread = CreateThread(nullptr, 0, WriteDumpThread, &request, 0, nullptr))
  {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
  }
}

void HandleFatalTermination(std::string_view reason)
{
  CONTEXT context{};
  RtlCaptureContext(&context);
  EXCEPTION_RECORD record{};
  record.ExceptionCode = STATUS_FATAL_APP_EXIT;
  record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
  EXCEPTION_POINTERS pointers{&record, &context};
  HandleCrash(reason, &pointers);
}

LONG WINAPI ExceptionFilter(EXCEPTION_POINTERS* pointers)
{
  HandleCrash("Unhandled exception", pointers);
  // Keep searching so that the system's error reporting still sees the crash.
  return EXCEPTION_CONTINUE_SEARCH;
}

void AbortSignalHandler(int)
{
  std::string reason = "Abort";
  if (const std::exception_ptr exception = std::current_exception())
  {
    try
    {
      std::rethrow_exception(exception);
    }
    catch (const std::exception& error)
    {
      reason = fmt::format("Uncaught exception: {}", error.what());
    }
    catch (...)
    {
      reason = "Uncaught exception";
    }
  }
  HandleFatalTermination(reason);
}

void InvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                             uintptr_t)
{
  HandleFatalTermination("Invalid CRT parameter");
  std::abort();
}

void PureCallHandler()
{
  HandleFatalTermination("Pure virtual call");
  std::abort();
}
}  // namespace

void InstallCrashHandler()
{
  const std::filesystem::path directory =
      std::filesystem::path(
          winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path().c_str()) /
      L"CrashDumps";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  s_dump_prefix = (directory / L"DolphinUWP_").wstring();

  if (const HMODULE dbgcore = LoadPackagedLibrary(L"dbgcore.dll", 0))
  {
    s_write_dump =
        reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbgcore, "MiniDumpWriteDump"));
  }

  SetUnhandledExceptionFilter(ExceptionFilter);
  // These end the process without passing through the exception filter.
  std::signal(SIGABRT, AbortSignalHandler);
  _set_invalid_parameter_handler(InvalidParameterHandler);
  _set_purecall_handler(PureCallHandler);
}
}  // namespace DolphinUWP

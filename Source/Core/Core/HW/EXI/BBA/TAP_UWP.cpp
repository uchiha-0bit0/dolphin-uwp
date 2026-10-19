// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cwchar>
#include <string>
#include <vector>

#include <winsock2.h>

#include <iphlpapi.h>
#include <tchar.h>

#include "Core/HW/EXI/BBA/TAP_Win32.h"

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

namespace
{
// The registry keys used to identify TAP adapters on desktop are not readable from an app
// container, so adapters are matched by their driver description instead.
std::vector<std::wstring> GetTAPAdapterGuids()
{
  constexpr ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                          GAA_FLAG_SKIP_DNS_SERVER;
  ULONG size = 16 * 1024;
  std::vector<u8> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
  {
    buffer.resize(size);
    result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }

  std::vector<std::wstring> guids;
  if (result != NO_ERROR)
  {
    ERROR_LOG_FMT(SP1, "GetAdaptersAddresses failed (err={:#x})", result);
    return guids;
  }

  for (const auto* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
       adapter = adapter->Next)
  {
    if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD && adapter->AdapterName &&
        adapter->Description && std::wcsstr(adapter->Description, L"TAP"))
    {
      guids.push_back(UTF8ToWString(adapter->AdapterName));
    }
  }
  return guids;
}

HANDLE OpenTAP(const std::wstring& device_guid, DWORD* error)
{
  const std::wstring device_path = USERMODEDEVICEDIR + device_guid + TAPSUFFIX;

  CREATEFILE2_EXTENDED_PARAMETERS parameters{};
  parameters.dwSize = sizeof(parameters);
  parameters.dwFileAttributes = FILE_ATTRIBUTE_SYSTEM;
  parameters.dwFileFlags = FILE_FLAG_OVERLAPPED;
  const HANDLE adapter =
      CreateFile2(device_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, &parameters);
  if (adapter == INVALID_HANDLE_VALUE)
    *error = GetLastError();
  return adapter;
}
}  // namespace

namespace ExpansionInterface
{
bool CEXIETHERNET::TAPNetworkInterface::Activate()
{
  if (IsActivated())
    return true;

  bool access_denied = false;
  for (const std::wstring& device_guid : GetTAPAdapterGuids())
  {
    DWORD error = ERROR_SUCCESS;
    mHAdapter = OpenTAP(device_guid, &error);
    if (mHAdapter != INVALID_HANDLE_VALUE)
    {
      INFO_LOG_FMT(SP1, "OPENED {}", WStringToUTF8(device_guid));
      break;
    }
    if (error == ERROR_ACCESS_DENIED)
    {
      access_denied = true;
      ERROR_LOG_FMT(SP1, "Access to TAP adapter {} was denied", WStringToUTF8(device_guid));
    }
  }

  if (mHAdapter == INVALID_HANDLE_VALUE)
  {
    if (access_denied)
    {
      PanicAlertFmtT("The TAP adapter could not be opened because Windows does not allow UWP "
                     "apps to access it.");
    }
    else
    {
      PanicAlertFmtT("No TAP adapter was found.");
    }
    return false;
  }

  /* get driver version info */
  DWORD len;
  ULONG info[3]{};
  if (DeviceIoControl(mHAdapter, TAP_IOCTL_GET_VERSION, &info, sizeof(info), &info, sizeof(info),
                      &len, nullptr))
  {
    INFO_LOG_FMT(SP1, "TAP-Win32 Driver Version {}.{} {}", info[0], info[1],
                 info[2] ? "(DEBUG)" : "");
  }
  if (!(info[0] > TAP_WIN32_MIN_MAJOR ||
        (info[0] == TAP_WIN32_MIN_MAJOR && info[1] >= TAP_WIN32_MIN_MINOR)))
  {
    PanicAlertFmtT("ERROR: This version of Dolphin requires a TAP-Win32 driver"
                   " that is at least version {0}.{1} -- If you recently upgraded your Dolphin"
                   " distribution, a reboot is probably required at this point to get"
                   " Windows to see the new driver.",
                   TAP_WIN32_MIN_MAJOR, TAP_WIN32_MIN_MINOR);
    Deactivate();
    return false;
  }

  /* set driver media status to 'connected' */
  ULONG status = TRUE;
  if (!DeviceIoControl(mHAdapter, TAP_IOCTL_SET_MEDIA_STATUS, &status, sizeof(status), &status,
                       sizeof(status), &len, nullptr))
  {
    ERROR_LOG_FMT(SP1, "WARNING: The TAP-Win32 driver rejected a"
                       "TAP_IOCTL_SET_MEDIA_STATUS DeviceIoControl call.");
    Deactivate();
    return false;
  }

  /* initialize read/write events */
  mReadOverlapped.hEvent =
      CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
  mWriteOverlapped.hEvent =
      CreateEventExW(nullptr, nullptr, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS);
  if (mReadOverlapped.hEvent == nullptr || mWriteOverlapped.hEvent == nullptr)
  {
    Deactivate();
    return false;
  }

  mWriteBuffer.reserve(1518);
  return RecvInit();
}

void CEXIETHERNET::TAPNetworkInterface::Deactivate()
{
  if (!IsActivated())
    return;

  // Signal read thread to exit.
  readEnabled.Clear();
  readThreadShutdown.Set();

  // Cancel any outstanding requests from both this thread (writes), and the read thread.
  CancelIoEx(mHAdapter, nullptr);

  // Wait for read thread to exit.
  if (readThread.joinable())
    readThread.join();

  // Clean-up handles
  if (mReadOverlapped.hEvent)
    CloseHandle(mReadOverlapped.hEvent);
  if (mWriteOverlapped.hEvent)
    CloseHandle(mWriteOverlapped.hEvent);
  CloseHandle(mHAdapter);
  mHAdapter = INVALID_HANDLE_VALUE;
  memset(&mReadOverlapped, 0, sizeof(mReadOverlapped));
  memset(&mWriteOverlapped, 0, sizeof(mWriteOverlapped));
  mWritePending = false;
}

bool CEXIETHERNET::TAPNetworkInterface::IsActivated()
{
  return mHAdapter != INVALID_HANDLE_VALUE;
}

void CEXIETHERNET::TAPNetworkInterface::ReadThreadHandler(TAPNetworkInterface* self)
{
  while (!self->readThreadShutdown.IsSet())
  {
    DWORD transferred;

    // Read from TAP into internal buffer.
    if (ReadFile(self->mHAdapter, self->m_eth_ref->mRecvBuffer.get(), BBA_RECV_SIZE, &transferred,
                 &self->mReadOverlapped))
    {
      // Returning immediately is not likely to happen, but if so, reset the event state manually.
      ResetEvent(self->mReadOverlapped.hEvent);
    }
    else
    {
      // IO should be pending.
      if (GetLastError() != ERROR_IO_PENDING)
      {
        ERROR_LOG_FMT(SP1, "ReadFile failed (err={:#x})", GetLastError());
        continue;
      }

      // Block until the read completes.
      if (!GetOverlappedResult(self->mHAdapter, &self->mReadOverlapped, &transferred, TRUE))
      {
        // If CancelIO was called, we should exit (the flag will be set).
        if (GetLastError() == ERROR_OPERATION_ABORTED)
          continue;

        // Something else went wrong.
        ERROR_LOG_FMT(SP1, "GetOverlappedResult failed (err={:#x})", GetLastError());
        continue;
      }
    }

    // Copy to BBA buffer, and fire interrupt if enabled.
    DEBUG_LOG_FMT(SP1, "Received {} bytes:\n {}", transferred,
                  ArrayToString(self->m_eth_ref->mRecvBuffer.get(), transferred, 0x10));
    if (self->readEnabled.IsSet())
    {
      self->m_eth_ref->mRecvBufferLength = transferred;
      self->m_eth_ref->RecvHandlePacket();
    }
  }
}

bool CEXIETHERNET::TAPNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  DEBUG_LOG_FMT(SP1, "SendFrame {} bytes:\n{}", size, ArrayToString(frame, size, 0x10));

  // Check for a background write. We can't issue another one until this one has completed.
  DWORD transferred;
  if (mWritePending)
  {
    // Wait for previous write to complete.
    if (!GetOverlappedResult(mHAdapter, &mWriteOverlapped, &transferred, TRUE))
      ERROR_LOG_FMT(SP1, "GetOverlappedResult failed (err={:#x})", GetLastError());
  }

  // Copy to write buffer.
  mWriteBuffer.assign(frame, frame + size);
  mWritePending = true;

  // Queue async write.
  if (WriteFile(mHAdapter, mWriteBuffer.data(), size, &transferred, &mWriteOverlapped))
  {
    // Returning immediately is not likely to happen, but if so, reset the event state manually.
    ResetEvent(mWriteOverlapped.hEvent);
  }
  else
  {
    // IO should be pending.
    if (GetLastError() != ERROR_IO_PENDING)
    {
      ERROR_LOG_FMT(SP1, "WriteFile failed (err={:#x})", GetLastError());
      ResetEvent(mWriteOverlapped.hEvent);
      mWritePending = false;
      return false;
    }
  }

  // Always report the packet as being sent successfully, even though it might be a lie
  m_eth_ref->SendComplete();
  return true;
}

bool CEXIETHERNET::TAPNetworkInterface::RecvInit()
{
  readThreadShutdown.Clear();
  readThread = std::thread(ReadThreadHandler, this);
  return true;
}

void CEXIETHERNET::TAPNetworkInterface::RecvStart()
{
  readEnabled.Set();
}

void CEXIETHERNET::TAPNetworkInterface::RecvStop()
{
  readEnabled.Clear();
}
}  // namespace ExpansionInterface

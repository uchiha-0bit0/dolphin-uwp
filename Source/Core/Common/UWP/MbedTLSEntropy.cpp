// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>

#include <windows.h>

#include <bcrypt.h>

extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t length,
                                     size_t* output_length)
{
  *output_length = 0;
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, output, static_cast<ULONG>(length),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
  {
    return -1;
  }
  *output_length = length;
  return 0;
}

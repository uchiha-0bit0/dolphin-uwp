// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/UWP/RootCertificates.h"

#include <array>
#include <utility>

#include <windows.h>
#include <wincrypt.h>

namespace Common::UWP
{
namespace
{
std::string LoadRootCertificates()
{
  constexpr std::array<std::pair<DWORD, const wchar_t*>, 3> stores{{
      {CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT"},
      {CERT_SYSTEM_STORE_LOCAL_MACHINE, L"AuthRoot"},
  }};

  std::string bundle;
  for (const auto& [location, name] : stores)
  {
    const HCERTSTORE store =
        CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                      location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG, name);
    if (!store)
      continue;

    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate)) != nullptr)
    {
      if (certificate->dwCertEncodingType != X509_ASN_ENCODING)
        continue;
      constexpr DWORD format = CRYPT_STRING_BASE64HEADER | CRYPT_STRING_NOCR;
      DWORD length = 0;
      if (!CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, format,
                                nullptr, &length))
      {
        continue;
      }
      std::string pem(length, '\0');
      if (!CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, format,
                                pem.data(), &length))
      {
        continue;
      }
      pem.resize(length);
      bundle += pem;
    }
    CertCloseStore(store, 0);
  }
  return bundle;
}
}  // namespace

const std::string& GetRootCertificatesPem()
{
  static const std::string bundle = LoadRootCertificates();
  return bundle;
}
}  // namespace Common::UWP

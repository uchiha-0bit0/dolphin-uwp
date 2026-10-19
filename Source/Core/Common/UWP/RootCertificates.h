// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace Common::UWP
{
// The trusted root certificates as PEM, for TLS libraries that can't read the certificate store.
const std::string& GetRootCertificatesPem();
}  // namespace Common::UWP

// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <UwpImGuiFrontend/GraphicsView.h>

#include "DiscIO/Blob.h"
#include "DiscIO/VolumeVerifier.h"
#include "DiscIO/WIABlob.h"

namespace UICommon
{
class GameFile;
}

namespace DolphinUWP
{
struct ShellServices;

class DiscToolsPage
{
public:
  explicit DiscToolsPage(const ShellServices& services) : m_services(services) {}

  void Open(std::shared_ptr<const UICommon::GameFile> game, std::string title);
  void Close();
  // Returns true when the page asks to close.
  bool Draw(float controller_scroll);

private:
  void DrawVerify();
  void DrawConvert();
  void Verify();
  void Convert();
  void StartConversion();

  const ShellServices& m_services;
  std::shared_ptr<const UICommon::GameFile> m_game;
  std::string m_title;
  UwpImGuiFrontend::GraphicsView m_view;

  bool m_calculate_crc32 = true;
  bool m_calculate_md5 = false;
  bool m_calculate_sha1 = true;
  bool m_verify_redump = true;
  std::optional<DiscIO::VolumeVerifier::Result> m_verify_result;

  DiscIO::BlobType m_format = DiscIO::BlobType::RVZ;
  int m_block_size = 0;
  DiscIO::WIARVZCompressionType m_compression = DiscIO::WIARVZCompressionType::Zstd;
  int m_compression_level = 5;
  bool m_remove_junk = false;
  bool m_delete_original = false;
};
}  // namespace DolphinUWP

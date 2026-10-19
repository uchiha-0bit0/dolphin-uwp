// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/Windows.UI.Core.h>

#include <UwpImGuiFrontend/Types.h>

#include "Common/CommonTypes.h"

namespace DolphinUWP
{
class FrontendRenderer
{
public:
  FrontendRenderer() = default;
  ~FrontendRenderer();

  FrontendRenderer(const FrontendRenderer&) = delete;
  FrontendRenderer& operator=(const FrontendRenderer&) = delete;

  bool Initialize(const winrt::Windows::UI::Core::CoreWindow& window, u32 width, u32 height);
  void Shutdown();
  bool IsInitialized() const { return m_swap_chain != nullptr; }

  void Resize(u32 width, u32 height);
  void BeginFrame(float delta_seconds);
  void EndFrame();

  UwpImGuiFrontend::TextureHandle LoadTexture(const std::filesystem::path& path);
  UwpImGuiFrontend::TextureHandle CreateTexture(const u8* rgba, u32 width, u32 height);
  void ReleaseTexture(UwpImGuiFrontend::TextureHandle texture);

  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }

private:
  bool CreateRenderTarget();

  Microsoft::WRL::ComPtr<ID3D11Device> m_device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap_chain;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_render_target;
  Microsoft::WRL::ComPtr<IWICImagingFactory> m_wic_factory;
  std::unordered_map<std::uintptr_t, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> m_textures;
  u32 m_width = 1;
  u32 m_height = 1;
  bool m_backend_initialized = false;
};
}  // namespace DolphinUWP

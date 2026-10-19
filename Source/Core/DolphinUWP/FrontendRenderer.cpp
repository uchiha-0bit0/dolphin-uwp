// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinUWP/FrontendRenderer.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx11.h>

#include "Common/HRWrap.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/UWP/FileSystem.h"

namespace DolphinUWP
{
using Microsoft::WRL::ComPtr;

FrontendRenderer::~FrontendRenderer()
{
  Shutdown();
}

bool FrontendRenderer::Initialize(const winrt::Windows::UI::Core::CoreWindow& window, u32 width,
                                  u32 height)
{
  if (IsInitialized())
    return true;

  m_width = std::max(1u, width);
  m_height = std::max(1u, height);

  static constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                             D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, feature_levels.data(),
                                 static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION,
                                 &m_device, nullptr, &m_context);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Frontend D3D11CreateDevice failed: {}", Common::HRWrap(hr));
    return false;
  }

  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(m_device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
  {
    Shutdown();
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = m_width;
  desc.Height = m_height;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

  hr = factory->CreateSwapChainForCoreWindow(m_device.Get(),
                                             static_cast<IUnknown*>(winrt::get_abi(window)), &desc,
                                             nullptr, &m_swap_chain);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Frontend CreateSwapChainForCoreWindow failed: {}", Common::HRWrap(hr));
    Shutdown();
    return false;
  }

  if (!CreateRenderTarget() || !ImGui_ImplDX11_Init(m_device.Get(), m_context.Get()))
  {
    Shutdown();
    return false;
  }
  m_backend_initialized = true;

  if (!m_wic_factory)
  {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&m_wic_factory));
  }

  return true;
}

void FrontendRenderer::Shutdown()
{
  if (m_backend_initialized)
  {
    ImGui_ImplDX11_Shutdown();
    m_backend_initialized = false;
  }

  m_textures.clear();
  m_render_target.Reset();
  m_swap_chain.Reset();
  if (m_context)
  {
    m_context->ClearState();
    m_context->Flush();
  }
  m_context.Reset();
  m_device.Reset();
}

bool FrontendRenderer::CreateRenderTarget()
{
  ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
    return false;
  return SUCCEEDED(m_device->CreateRenderTargetView(back_buffer.Get(), nullptr, &m_render_target));
}

void FrontendRenderer::Resize(u32 width, u32 height)
{
  width = std::max(1u, width);
  height = std::max(1u, height);
  if (!IsInitialized() || (width == m_width && height == m_height))
    return;

  m_context->OMSetRenderTargets(0, nullptr, nullptr);
  m_render_target.Reset();
  const HRESULT hr = m_swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  if (FAILED(hr))
  {
    ERROR_LOG_FMT(VIDEO, "Frontend ResizeBuffers failed: {}", Common::HRWrap(hr));
    return;
  }

  m_width = width;
  m_height = height;
  CreateRenderTarget();
}

void FrontendRenderer::BeginFrame(float delta_seconds)
{
  ImGui_ImplDX11_NewFrame();
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = {static_cast<float>(m_width), static_cast<float>(m_height)};
  io.DeltaTime = std::max(delta_seconds, 1.0f / 1000.0f);
  ImGui::NewFrame();
}

void FrontendRenderer::EndFrame()
{
  ImGui::Render();

  static constexpr std::array<float, 4> clear_color{0.025f, 0.028f, 0.035f, 1.0f};
  ID3D11RenderTargetView* const render_target = m_render_target.Get();
  m_context->OMSetRenderTargets(1, &render_target, nullptr);
  m_context->ClearRenderTargetView(render_target, clear_color.data());
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
  m_swap_chain->Present(1, 0);
}

UwpImGuiFrontend::TextureHandle FrontendRenderer::LoadTexture(const std::filesystem::path& path)
{
  if (!m_device || !m_wic_factory || path.empty())
    return {};

  const HANDLE file =
      Common::UWP::OpenFileHandle(PathToString(path), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
  if (file == INVALID_HANDLE_VALUE)
    return {};
  Common::ScopeGuard file_guard([file] { CloseHandle(file); });

  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(m_wic_factory->CreateDecoderFromFileHandle(
          reinterpret_cast<ULONG_PTR>(file), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(decoder->GetFrame(0, &frame)) ||
      FAILED(m_wic_factory->CreateFormatConverter(&converter)) ||
      FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)))
  {
    return {};
  }

  UINT width = 0;
  UINT height = 0;
  if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 ||
      width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
  {
    return {};
  }

  const UINT stride = width * 4;
  std::vector<u8> pixels(static_cast<size_t>(stride) * height);
  if (FAILED(
          converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data())))
  {
    return {};
  }
  return CreateTexture(pixels.data(), width, height);
}

UwpImGuiFrontend::TextureHandle FrontendRenderer::CreateTexture(const u8* rgba, u32 width,
                                                                u32 height)
{
  if (!m_device || !rgba || width == 0 || height == 0 ||
      width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
  {
    return {};
  }

  const UINT stride = width * 4;
  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = width;
  texture_desc.Height = height;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  const D3D11_SUBRESOURCE_DATA initial_data{rgba, stride, 0};
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> view;
  if (FAILED(m_device->CreateTexture2D(&texture_desc, &initial_data, &texture)) ||
      FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &view)))
  {
    return {};
  }

  const auto id = reinterpret_cast<std::uintptr_t>(view.Get());
  m_textures.emplace(id, std::move(view));
  return {id, width, height};
}

void FrontendRenderer::ReleaseTexture(UwpImGuiFrontend::TextureHandle texture)
{
  m_textures.erase(texture.id);
}
}  // namespace DolphinUWP

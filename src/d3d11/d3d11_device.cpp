// ID3D11Device and the D3D11CreateDevice* entry points.
#include "d3d11_internal.h"

#include "dxgi_formats.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

namespace phxd3d11
{

//////////////////////////////////////////////////////////////////////////////
// DeviceImpl

DeviceImpl::~DeviceImpl()
{
  TrackObjectDestroyed("ID3D11Device", this);
}

HRESULT DeviceImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11Device)))
  {
    *ppvObject = static_cast<ID3D11Device *>(this);
    AddRefImpl();
    return S_OK;
  }
  if (GuidEq(riid, __uuidof(ID3D11Debug)))
  {
    if (!debug)
    {
      debug = new DebugImpl();
      debug->device = this;
      TrackObjectCreated("ID3D11Debug", debug);
    }
    *ppvObject = static_cast<ID3D11Debug *>(debug);
    debug->AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG DeviceImpl::Release(void)
{
  ULONG n = ReleaseImpl();
  return n;
}

D3D_FEATURE_LEVEL DeviceImpl::GetFeatureLevel(void) { return featureLevel; }

void DeviceImpl::GetImmediateContext(ID3D11DeviceContext **ppImmediateContext)
{
  if (!ppImmediateContext) return;
  *ppImmediateContext = context;
  if (context) context->AddRefImpl();
}

HRESULT DeviceImpl::SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) { return SetPrivateDataImpl(Name, DataSize, pData); }

HRESULT DeviceImpl::CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport)
{
  if (!pFormatSupport) return E_POINTER;
  *pFormatSupport = 0;
  if (Format == DXGI_FORMAT_UNKNOWN) return E_INVALIDARG;

  VkFormat vkFormat = DXGIFormatToVk(Format);
  if (vkFormat == VK_FORMAT_UNDEFINED) return E_INVALIDARG;

  VkFormatProperties props{};
  vkGetPhysicalDeviceFormatProperties(phxvk::Vk().physical, vkFormat, &props);
  const VkFormatFeatureFlags f = props.optimalTilingFeatures;

  if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) *pFormatSupport |= D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
  if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) *pFormatSupport |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
  if (f & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) *pFormatSupport |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
  if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) *pFormatSupport |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
  if (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) *pFormatSupport |= D3D11_FORMAT_SUPPORT_BLENDABLE;
  if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) *pFormatSupport |= D3D11_FORMAT_SUPPORT_BLENDABLE;
  *pFormatSupport |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER | D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER | D3D11_FORMAT_SUPPORT_MIP | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
  return S_OK;
}

//////////////////////////////////////////////////////////////////////////////
// creation

namespace
{

HRESULT CreateDeviceInternal(UINT Flags, ID3D11Device **ppDevice, ID3D11DeviceContext **ppImmediateContext, D3D_FEATURE_LEVEL *pFeatureLevel)
{
  if (ppDevice) *ppDevice = nullptr;
  if (ppImmediateContext) *ppImmediateContext = nullptr;
  if (pFeatureLevel) *pFeatureLevel = D3D_FEATURE_LEVEL_11_0;
  if (!ppDevice) return E_POINTER;

  // Bring up the Vulkan core if the embedding application has not done it yet.
  if (phxvk::Vk().device == VK_NULL_HANDLE)
  {
    const bool validation = ((Flags & D3D11_CREATE_DEVICE_DEBUG) != 0) || phxvk::vkValidationRequested();
    phxvk::vkInitialize(validation, "Neo-apEx-Player");
  }

  auto *device = new DeviceImpl();
  device->creationFlags = Flags;
  device->featureLevel = D3D_FEATURE_LEVEL_11_0;

  auto *context = new ContextImpl();
  context->device = device;
  device->context = context;

  TrackObjectCreated("ID3D11Device", device);
  TrackObjectCreated("ID3D11DeviceContext", context);

  *ppDevice = device;
  if (ppImmediateContext) *ppImmediateContext = context;
  if (pFeatureLevel) *pFeatureLevel = D3D_FEATURE_LEVEL_11_0;

  phxvk::LogInfo("[d3d11] device created (Vulkan %s, geometryShader %s)", phxvk::Vk().props.deviceName, phxvk::Vk().features.geometryShader ? "yes" : "NO");
  return S_OK;
}

} // namespace

} // namespace phxd3d11

using namespace phxd3d11;

extern "C" HRESULT WINAPI D3D11CreateDevice(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
                                            UINT FeatureLevels, UINT SDKVersion, ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel,
                                            ID3D11DeviceContext **ppImmediateContext)
{
  (void)pAdapter;
  (void)DriverType;
  (void)Software;
  (void)pFeatureLevels;
  (void)FeatureLevels;
  (void)SDKVersion;
  return CreateDeviceInternal(Flags, ppDevice, ppImmediateContext, pFeatureLevel);
}

extern "C" HRESULT WINAPI D3D11CreateDeviceAndSwapChain(IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software, UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels,
                                                        UINT FeatureLevels, UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
                                                        ID3D11Device **ppDevice, D3D_FEATURE_LEVEL *pFeatureLevel, ID3D11DeviceContext **ppImmediateContext)
{
  (void)pAdapter;
  (void)DriverType;
  (void)Software;
  (void)pFeatureLevels;
  (void)FeatureLevels;
  (void)SDKVersion;

  if (ppSwapChain) *ppSwapChain = nullptr;
  if (!pSwapChainDesc) return E_INVALIDARG;

  // SDL's Vulkan instance extensions must be enabled *before* the instance is
  // created, so register them here and only then bring up the core.
  SwapchainAddInstanceExtensions();
  if (phxvk::Vk().instance != VK_NULL_HANDLE && !phxvk::vkInstanceExtensionRequested(VK_KHR_SURFACE_EXTENSION_NAME))
    phxvk::LogWarn("[d3d11] the Vulkan instance was created before D3D11CreateDeviceAndSwapChain; if surface creation fails, call "
                   "phxd3d11::SwapchainAddInstanceExtensions() (or SDL_Vulkan_GetInstanceExtensions) before vkInitialize()");

  HRESULT hr = CreateDeviceInternal(Flags, ppDevice, ppImmediateContext, pFeatureLevel);
  if (FAILED(hr) || !ppDevice || !*ppDevice) return hr;

  auto *device = static_cast<DeviceImpl *>(*ppDevice);
  auto *swapChain = new SwapChainImpl();
  const uint32_t width = pSwapChainDesc->BufferDesc.Width;
  const uint32_t height = pSwapChainDesc->BufferDesc.Height;
  if (!swapChain->Initialize(device, pSwapChainDesc->OutputWindow, *pSwapChainDesc, device->context))
  {
    swapChain->Release();
    (*ppDevice)->Release();
    if (ppDevice) *ppDevice = nullptr;
    if (ppImmediateContext) *ppImmediateContext = nullptr;
    return E_FAIL;
  }
  TrackObjectCreated("IDXGISwapChain", swapChain);
  device->swapChain = swapChain;
  device->context->swapChain = swapChain;

  if (ppSwapChain) *ppSwapChain = swapChain;
  else swapChain->Release();

  phxvk::LogInfo("[d3d11] device and swap chain created (%ux%u)", width, height);
  return S_OK;
}

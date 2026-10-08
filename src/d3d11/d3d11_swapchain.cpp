// IDXGISwapChain on top of VkSwapchainKHR.
//
// D3D11 code treats the back buffer as a stable texture for the lifetime of the
// swap chain; Vulkan hands out a different image every frame.  The
// Texture2DImpl we return from GetBuffer() therefore re-resolves its VkImage
// (and the RTV its VkImageView) from the swap chain every time they are used.
#include "d3d11_internal.h"
#include "platform/window_sdl.h"

#include "dxgi_formats.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace phxd3d11
{

SwapChainImpl::~SwapChainImpl()
{
  Shutdown();
  TrackObjectDestroyed("IDXGISwapChain", this);
}

bool SwapChainImpl::Initialize(DeviceImpl *device, HWND outputWindow, const DXGI_SWAP_CHAIN_DESC &desc, ContextImpl *context)
{
  m_device = device;
  m_context = context;
  m_window = outputWindow;

  uint32_t width = desc.BufferDesc.Width;
  uint32_t height = desc.BufferDesc.Height;
  if (outputWindow && (!width || !height))
  {
    SDL_Window *window = PhxWindowFromEngineHandle((TU32)(uintptr_t)outputWindow);
    int w = 0, h = 0;
    if (SDL_GetWindowSizeInPixels(window, &w, &h))
    {
      width = (uint32_t)w;
      height = (uint32_t)h;
    }
  }
  if (!m_chain.Create(outputWindow, desc, width, height)) return false;
  if (!CreateBackBufferObjects()) return false;
  return true;
}

void SwapChainImpl::Shutdown()
{
  DestroyBackBufferObjects();
  m_chain.Destroy();
}

bool SwapChainImpl::CreateBackBufferObjects()
{
  auto *tex = new Texture2DImpl();
  tex->device = m_device;
  tex->tex.swapchain = &m_chain;
  tex->tex.width = m_chain.Width();
  tex->tex.height = m_chain.Height();
  tex->tex.depth = 1;
  tex->tex.arraySize = 1;
  tex->tex.mipLevels = 1;
  tex->tex.format = m_chain.Format();
  tex->tex.sampleCount = 1;
  tex->tex.usage = D3D11_USAGE_DEFAULT;
  tex->tex.bindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  tex->tex.miscFlags = 0;
  TrackObjectCreated("ID3D11Texture2D(backbuffer)", tex);
  m_backBuffer = tex;

  ID3D11RenderTargetView *rtv = nullptr;
  HRESULT hr = m_device->CreateRenderTargetView(m_backBuffer, nullptr, &rtv);
  if (FAILED(hr) || !rtv)
  {
    phxvk::LogError("swap chain: could not create the back buffer render target view");
    return false;
  }
  m_backBufferRTV = rtv;
  if (m_context) m_context->OnSwapchainRecreated();
  return true;
}

void SwapChainImpl::DestroyBackBufferObjects()
{
  if (m_backBufferRTV)
  {
    m_backBufferRTV->Release();
    m_backBufferRTV = nullptr;
  }
  if (m_backBuffer)
  {
    m_backBuffer->Release();
    m_backBuffer = nullptr;
  }
}

bool SwapChainImpl::BeginFrame()
{
  if (!m_chain.valid()) return false;
  return m_chain.AcquireNextImage();
}

HRESULT SwapChainImpl::PresentFrame(UINT, UINT)
{
  phxvk::Device &d = phxvk::Vk();
  if (!m_chain.valid()) return DXGI_ERROR_INVALID_CALL;
  if (!m_context) return E_FAIL;

  // Close the dynamic-rendering instance before touching the back buffer.
  m_context->EndRendering();

  // If nothing was recorded this frame there is still no command buffer, and
  // the acquire semaphore must be consumed by *some* submission.
  if (!d.frameActive) phxvk::FrameBegin();

  // The back buffer becomes presentable.
  TextureImpl *tex = &static_cast<Texture2DImpl *>(m_backBuffer)->tex;
  m_context->TransitionImage(tex, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

  VkCommandBuffer cmd = d.frameCmd;
  PHXVK_CHECK(vkEndCommandBuffer(cmd));

  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphore acquire = m_chain.SubmitWaitSemaphore();
  VkSemaphore renderFinished = m_chain.RenderFinishedSemaphore();
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  if (m_chain.TakeSubmitWaitsOnAcquire())
  {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquire;
    si.pWaitDstStageMask = &waitStage;
  }
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &renderFinished;
  VkResult submit = vkQueueSubmit(d.queue, 1, &si, d.frameFence);
  if (submit != VK_SUCCESS)
  {
    phxvk::LogError("vkQueueSubmit failed: %s", phxvk::VkResultName(submit));
    d.frameActive = false;
    return DXGI_ERROR_DEVICE_REMOVED;
  }
  d.frameActive = false;
  d.frameIndex++;

  VkResult res = m_chain.Present();
  if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
  {
    phxvk::LogError("vkQueuePresentKHR failed: %s", phxvk::VkResultName(res));
    return DXGI_ERROR_DEVICE_REMOVED;
  }

  // Acquire the next image now so the following frame renders into it.
  m_chain.AcquireNextImage();

  // Descriptor pools may only be recycled once the previous frame's fence has
  // been waited on, which happens in FrameBegin().
  m_context->poolsCleared = false;

  return S_OK;
}

HRESULT SwapChainImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(IDXGIObject)) || GuidEq(riid, __uuidof(IDXGIDeviceSubObject)) || GuidEq(riid, __uuidof(IDXGISwapChain)))
  {
    *ppvObject = static_cast<IDXGISwapChain *>(this);
    AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT SwapChainImpl::Present(UINT SyncInterval, UINT Flags) { return PresentFrame(SyncInterval, Flags); }

HRESULT SwapChainImpl::GetBuffer(UINT Buffer, REFIID riid, void **ppSurface)
{
  if (!ppSurface) return E_POINTER;
  *ppSurface = nullptr;
  if (Buffer != 0)
  {
    phxvk::LogWarn("IDXGISwapChain::GetBuffer(%u) is not supported; only buffer 0 exists in this port", Buffer);
    return E_INVALIDARG;
  }
  if (!m_backBuffer) return DXGI_ERROR_INVALID_CALL;
  if (GuidEq(riid, __uuidof(ID3D11Texture2D)) || GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11Resource)))
  {
    *ppSurface = static_cast<ID3D11Texture2D *>(m_backBuffer);
    m_backBuffer->AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT SwapChainImpl::SetFullscreenState(BOOL Fullscreen, IDXGIOutput *)
{
  // SDL owns the window: fullscreen is requested through SDL_SetWindowFullscreen
  // by the windowing layer, so this only records the state.
  if (OnceFullscreen())
    phxvk::LogWarn("[d3d11] SetFullscreenState is a no-op in this port; SDL owns the window's fullscreen state");
  m_chain.fullscreen = Fullscreen != FALSE;
  return S_OK;
}

HRESULT SwapChainImpl::GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget)
{
  if (pFullscreen) *pFullscreen = m_chain.fullscreen ? TRUE : FALSE;
  if (ppTarget) *ppTarget = nullptr;
  return S_OK;
}

HRESULT SwapChainImpl::GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc)
{
  if (!pDesc) return E_INVALIDARG;
  *pDesc = m_chain.Desc();
  pDesc->BufferDesc.Width = m_chain.Width();
  pDesc->BufferDesc.Height = m_chain.Height();
  pDesc->BufferDesc.Format = m_chain.Format();
  pDesc->BufferCount = m_chain.ImageCount();
  return S_OK;
}

HRESULT SwapChainImpl::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT)
{
  (void)BufferCount;
  phxvk::Device &d = phxvk::Vk();
  if (!m_chain.valid()) return DXGI_ERROR_INVALID_CALL;

  // The engine passes 0/0/UNKNOWN and then re-reads the back buffer.
  if (!Width || !Height)
  {
    if (m_window)
    {
      SDL_Window *window = PhxWindowFromEngineHandle((TU32)(uintptr_t)m_window);
      int w = 0, h = 0;
      if (SDL_GetWindowSizeInPixels(window, &w, &h) && w > 0 && h > 0)
      {
        Width = (uint32_t)w;
        Height = (uint32_t)h;
      }
    }
    if (!Width) Width = m_chain.Width();
    if (!Height) Height = m_chain.Height();
  }

  m_context->EndRendering();
  if (d.frameActive)
  {
    phxvk::FrameEndAndSubmit();
    vkWaitForFences(d.device, 1, &d.frameFence, VK_TRUE, UINT64_MAX);
    phxvk::FrameWaitIdle();
  }

  // Drop every view/back buffer object that references the old images first.
  DestroyBackBufferObjects();
  if (!m_chain.Recreate(Width, Height, NewFormat))
  {
    phxvk::LogError("swap chain: ResizeBuffers failed");
    return E_FAIL;
  }
  if (!CreateBackBufferObjects())
  {
    phxvk::LogError("swap chain: could not recreate the back buffer after resizing");
    return E_FAIL;
  }
  m_context->OnSwapchainRecreated();
  return S_OK;
}

HRESULT SwapChainImpl::ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters)
{
  (void)pNewTargetParameters;
  return S_OK;
}

} // namespace phxd3d11

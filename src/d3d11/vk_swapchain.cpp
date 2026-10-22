#include "vk_swapchain.h"
#include "platform/window_sdl.h"

#include "dxgi_formats.h"

#include "../rhi/vk_core.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstring>

namespace phxd3d11
{

// The shim has exactly one swap chain in practice, and the acquire-wait hook is
// a plain function pointer, so it needs a single well-known target.
static Swapchain *s_activeSwapchain = nullptr;

namespace
{
bool g_loggedFullscreen = false;
bool g_loggedFormatFallback = false;
} // namespace

void SwapchainAddInstanceExtensions()
{
  Uint32 count = 0;
  const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&count);
  if (!exts || !count)
  {
    phxvk::LogWarn("SDL_Vulkan_GetInstanceExtensions() returned nothing; the swap chain will not be able to create a surface");
    return;
  }
  phxvk::vkAddInstanceExtensions(exts, count);
}

Swapchain::~Swapchain() { Destroy(); }

bool Swapchain::CreateSurface(HWND outputWindow)
{
  if (m_surface != VK_NULL_HANDLE) return true;
  if (!outputWindow)
  {
    phxvk::LogError("DXGI_SWAP_CHAIN_DESC::OutputWindow is null; cannot create a Vulkan surface");
    return false;
  }

  SDL_Window *window = PhxWindowFromEngineHandle((TU32)(uintptr_t)outputWindow);
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  if (!SDL_Vulkan_CreateSurface(window, phxvk::Vk().instance, nullptr, &surface))
  {
    phxvk::LogError("SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
    phxvk::LogError("(were the SDL Vulkan instance extensions enabled? call SwapchainAddInstanceExtensions() before vkInitialize())");
    return false;
  }
  m_surface = surface;
  return true;
}

bool Swapchain::Create(HWND outputWindow, const DXGI_SWAP_CHAIN_DESC &desc, uint32_t width, uint32_t height)
{
  m_desc = desc;
  s_activeSwapchain = this;
  if (!CreateSurface(outputWindow)) return false;

  VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  if (m_acquireSemaphores.empty())
  {
    m_acquireSemaphores.resize((size_t)phxvk::Device::kFramesInFlight, VK_NULL_HANDLE);
    for (VkSemaphore &sem : m_acquireSemaphores)
      PHXVK_CHECK(vkCreateSemaphore(phxvk::Vk().device, &sci, nullptr, &sem));
    m_acquireIndex = m_nextAcquireIndex = 0;
  }
  if (!CreateSwapchain(width, height, desc.BufferDesc.Format)) return false;

  // One render-finished semaphore per image, created once the image count is
  // known.  Reusing a single one across frames is what the validation layers
  // complained about.
  if (m_renderFinishedSemaphores.empty())
  {
    m_renderFinishedSemaphores.resize(m_images.size(), VK_NULL_HANDLE);
    for (VkSemaphore &sem : m_renderFinishedSemaphores)
      PHXVK_CHECK(vkCreateSemaphore(phxvk::Vk().device, &sci, nullptr, &sem));
  }

  // The acquire wait has to reach whichever submission happens first in a frame,
  // and that is not necessarily the present's - see vkSetAcquireWaitHook().
  phxvk::vkSetAcquireWaitHook([]() -> VkSemaphore { return s_activeSwapchain ? s_activeSwapchain->TakeAcquireWait() : VK_NULL_HANDLE; });

  return AcquireNextImage();
}

bool Swapchain::CreateSwapchain(uint32_t width, uint32_t height, DXGI_FORMAT format)
{
  phxvk::Device &d = phxvk::Vk();

  // Nothing may still be using the images we are about to destroy.
  //
  // This is the one place a swap chain is rebuilt, and it is reached from the
  // acquire and present paths - i.e. in the middle of a frame that has just been
  // submitted and whose command buffer still references these images (a layout
  // transition to PRESENT_SRC, for one).  Without the wait, destroying them
  // leaves the GPU holding freed MVKImages, and the next barrier over one of
  // them crashed inside MVKImage::applyImageMemoryBarrier.  Waiting here is what
  // Recreate() already did before calling this.
  vkDeviceWaitIdle(d.device);

  // The old images and semaphores are about to go away, so no wait can still be
  // outstanding against them.
  m_submitWaitsOnAcquire = false;

  VkSurfaceCapabilitiesKHR caps{};
  PHXVK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d.physical, m_surface, &caps));

  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, m_surface, &formatCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  if (formatCount) vkGetPhysicalDeviceSurfaceFormatsKHR(d.physical, m_surface, &formatCount, formats.data());

  // Pick the surface format that matches the requested DXGI format, falling
  // back to the first B8G8R8A8/R8G8B8A8 UNORM entry, then to formats[0].
  VkFormat want = DXGIFormatToVk(format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : format);
  if (want == VK_FORMAT_B8G8R8A8_SRGB) want = VK_FORMAT_B8G8R8A8_UNORM;
  if (want == VK_FORMAT_R8G8B8A8_SRGB) want = VK_FORMAT_R8G8B8A8_UNORM;

  VkSurfaceFormatKHR chosen{};
  bool found = false;
  if (formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
  {
    chosen.format = want;
    chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    found = true;
  }
  for (uint32_t i = 0; i < formatCount && !found; i++)
  {
    if (formats[i].format == want && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
    {
      chosen = formats[i];
      found = true;
    }
  }
  for (uint32_t i = 0; i < formatCount && !found; i++)
  {
    if (formats[i].format == want)
    {
      chosen = formats[i];
      found = true;
    }
  }
  for (uint32_t i = 0; i < formatCount && !found; i++)
  {
    if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM)
    {
      chosen = formats[i];
      found = true;
    }
  }
  if (!found)
  {
    if (!formatCount)
    {
      phxvk::LogError("surface advertises no formats");
      return false;
    }
    chosen = formats[0];
    if (!g_loggedFormatFallback)
    {
      g_loggedFormatFallback = true;
      phxvk::LogWarn("requested swap chain format %d is unsupported by the surface; using VkFormat %d instead", (int)format, (int)chosen.format);
    }
  }

  // Present mode: FIFO (vsync) always exists; prefer MAILBOX when we are not
  // asked to sync.  The D3D11 layer selects via Present(SyncInterval).
  uint32_t modeCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, m_surface, &modeCount, nullptr);
  std::vector<VkPresentModeKHR> modes(modeCount);
  if (modeCount) vkGetPhysicalDeviceSurfacePresentModesKHR(d.physical, m_surface, &modeCount, modes.data());
  m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  for (auto m : modes)
    if (m == VK_PRESENT_MODE_MAILBOX_KHR) m_presentMode = m;

  VkExtent2D extent;
  if (caps.currentExtent.width != UINT32_MAX)
  {
    extent = caps.currentExtent;
    if (width && height && (extent.width != width || extent.height != height))
    {
      // A resizable SDL window reports the real extent; trust it.
      phxvk::LogInfo("swap chain extent %ux%u (requested %ux%u)", extent.width, extent.height, width, height);
    }
  }
  else
  {
    extent.width = width ? width : 1280;
    extent.height = height ? height : 720;
    if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
    if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
  }
  if (!extent.width || !extent.height)
  {
    extent.width = 1280;
    extent.height = 720;
  }

  uint32_t imageCount = caps.minImageCount + 1;
  if (imageCount < 2) imageCount = 2;
  if (caps.maxImageCount && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

  VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
  {
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
      alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
      alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    else
      alpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
  }

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = m_surface;
  ci.minImageCount = imageCount;
  ci.imageFormat = chosen.format;
  ci.imageColorSpace = chosen.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = alpha;
  ci.presentMode = m_presentMode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = m_swapchain;

  VkSwapchainKHR newChain = VK_NULL_HANDLE;
  VkResult res = vkCreateSwapchainKHR(d.device, &ci, nullptr, &newChain);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkCreateSwapchainKHR failed: %s", phxvk::VkResultName(res));
    return false;
  }

  // Tear down every view referencing the old images before dropping them.
  DestroySwapchainObjects();
  m_swapchain = newChain;

  uint32_t count = 0;
  PHXVK_CHECK(vkGetSwapchainImagesKHR(d.device, m_swapchain, &count, nullptr));
  m_images.resize(count);
  PHXVK_CHECK(vkGetSwapchainImagesKHR(d.device, m_swapchain, &count, m_images.data()));
  m_views.assign(count, VK_NULL_HANDLE);
  m_layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);

  for (uint32_t i = 0; i < count; i++)
  {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = m_images[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = chosen.format;
    vi.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    res = vkCreateImageView(d.device, &vi, nullptr, &m_views[i]);
    if (res != VK_SUCCESS)
    {
      phxvk::LogError("could not create a view for swap chain image %u: %s", i, phxvk::VkResultName(res));
      return false;
    }
  }

  m_width = extent.width;
  m_height = extent.height;
  m_vkFormat = chosen.format;
  m_format = VkFormatToDXGI(chosen.format);
  if (m_format == DXGI_FORMAT_UNKNOWN) m_format = format;
  m_current = 0;
  m_submitWaitsOnAcquire = false;

  phxvk::LogInfo("swap chain: %ux%u, %u images, VkFormat %d, present mode %d", m_width, m_height, count, (int)chosen.format, (int)m_presentMode);
  return true;
}

void Swapchain::DestroySwapchainObjects()
{
  phxvk::Device &d = phxvk::Vk();
  if (!d.device) return;
  for (auto v : m_views)
    if (v != VK_NULL_HANDLE) vkDestroyImageView(d.device, v, nullptr);
  m_views.clear();
  m_images.clear();
  m_layouts.clear();
  if (m_swapchain != VK_NULL_HANDLE)
  {
    vkDestroySwapchainKHR(d.device, m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
  }
}

void Swapchain::Destroy()
{
  // The acquire-wait hook reaches this object through a static pointer; leaving
  // it set after destruction hands a freed object to the next submission.
  if (s_activeSwapchain == this) s_activeSwapchain = nullptr;
  phxvk::Device &d = phxvk::Vk();
  if (!d.device)
  {
    m_views.clear();
    m_images.clear();
    m_layouts.clear();
    m_swapchain = VK_NULL_HANDLE;
    m_surface = VK_NULL_HANDLE;
    return;
  }
  vkDeviceWaitIdle(d.device);
  DestroySwapchainObjects();
  for (VkSemaphore &sem : m_acquireSemaphores)
    if (sem) vkDestroySemaphore(d.device, sem, nullptr);
  m_acquireSemaphores.clear();
  m_acquireIndex = m_nextAcquireIndex = 0;
  for (VkSemaphore &sem : m_renderFinishedSemaphores)
    if (sem != VK_NULL_HANDLE) vkDestroySemaphore(d.device, sem, nullptr);
  m_renderFinishedSemaphores.clear();
  if (m_surface)
  {
    vkDestroySurfaceKHR(d.instance, m_surface, nullptr);
    m_surface = VK_NULL_HANDLE;
  }
}

bool Swapchain::Recreate(uint32_t width, uint32_t height, DXGI_FORMAT newFormat)
{
  phxvk::Device &d = phxvk::Vk();
  if (!m_surface) return false;
  vkDeviceWaitIdle(d.device);
  m_desc.BufferDesc.Width = width;
  m_desc.BufferDesc.Height = height;
  if (newFormat != DXGI_FORMAT_UNKNOWN) m_desc.BufferDesc.Format = newFormat;
  return CreateSwapchain(width, height, m_desc.BufferDesc.Format);
}

bool Swapchain::AcquireNextImage()
{
  if (m_swapchain == VK_NULL_HANDLE) return false;
  phxvk::Device &d = phxvk::Vk();
  uint32_t index = 0;
  const size_t acquireSlot = m_acquireSemaphores.empty() ? 0 : m_nextAcquireIndex % m_acquireSemaphores.size();
  VkSemaphore acquireSem = m_acquireSemaphores.empty() ? VK_NULL_HANDLE : m_acquireSemaphores[acquireSlot];
  VkResult res = vkAcquireNextImageKHR(d.device, m_swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &index);

  // VK_SUBOPTIMAL_KHR still hands back a usable image AND leaves the semaphore
  // signalled, so it must be accepted, not retried: acquiring again with the
  // same semaphore is exactly what the validation layers flag as "Semaphore must
  // not have any pending operations".  Only OUT_OF_DATE means no image.
  if (res == VK_SUBOPTIMAL_KHR)
  {
    m_suboptimal = true;
  }
  else if (res == VK_ERROR_OUT_OF_DATE_KHR)
  {
    phxvk::LogWarn("swap chain out of date on acquire; recreating");
    if (!CreateSwapchain(m_width, m_height, m_desc.BufferDesc.Format)) return false;
    res = vkAcquireNextImageKHR(d.device, m_swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &index);
  }

  if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
  {
    phxvk::LogError("vkAcquireNextImageKHR failed: %s", phxvk::VkResultName(res));
    return false;
  }
  // This acquire's semaphore is the one this frame's submission waits on; the
  // next acquire rotates to the following one so it can never be re-signalled
  // while a submit from an earlier frame is still waiting on it.
  if (!m_acquireSemaphores.empty())
  {
    m_acquireIndex = acquireSlot;
    m_nextAcquireIndex = (acquireSlot + 1) % m_acquireSemaphores.size();
  }
  m_current = index;
  m_submitWaitsOnAcquire = true;
  return true;
}

VkResult Swapchain::Present()
{
  if (m_swapchain == VK_NULL_HANDLE) return VK_ERROR_SURFACE_LOST_KHR;
  phxvk::Device &d = phxvk::Vk();
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  // The semaphore belonging to THIS image: the one the last submission before
  // this present signalled.
  VkSemaphore renderFinished = RenderFinishedSemaphore();
  if (renderFinished == VK_NULL_HANDLE)
  {
    phxvk::LogError("swap chain present without a render-finished semaphore");
    return VK_ERROR_UNKNOWN;
  }
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &renderFinished;
  pi.swapchainCount = 1;
  pi.pSwapchains = &m_swapchain;
  pi.pImageIndices = &m_current;
  VkResult res = vkQueuePresentKHR(d.queue, &pi);
  // Same distinction as on acquire: a SUBOPTIMAL present has still happened, and
  // recreating the swap chain here would destroy semaphores the present may
  // still be waiting on.  Note it and let the next acquire rebuild instead.
  if (res == VK_SUBOPTIMAL_KHR)
  {
    m_suboptimal = true;
    return VK_SUCCESS;
  }
  if (res == VK_ERROR_OUT_OF_DATE_KHR)
  {
    phxvk::LogWarn("swap chain out of date on present; recreating");
    if (CreateSwapchain(m_width, m_height, m_desc.BufferDesc.Format)) res = VK_SUCCESS;
  }
  return res;
}

} // namespace phxd3d11

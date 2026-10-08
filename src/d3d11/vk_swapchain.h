// Vulkan swapchain / surface plumbing for the D3D11 shim.
//
// This file deliberately knows nothing about D3D11 interface types:  it takes
// the `DXGI_SWAP_CHAIN_DESC` the vendored engine fills in (whose OutputWindow,
// in this port, is an SDL_Window* cast to HWND) and manages the VkSurfaceKHR,
// the VkSwapchainKHR and the acquired image.
#pragma once

#include <vulkan/vulkan.h>

#include <dxgi.h>

#include <cstdint>
#include <vector>

namespace phxd3d11
{

// Registers the SDL Vulkan instance extensions with phxvk::vkAddInstanceExtensions.
// Must be called before phxvk::vkInitialize(); safe to call repeatedly.
void SwapchainAddInstanceExtensions();

class Swapchain
{
public:
  ~Swapchain();

  // Creates the surface (from `desc.OutputWindow` == SDL_Window*) and the
  // swapchain.  `phxvk::vkInitialize()` must already have run.
  bool Create(HWND outputWindow, const DXGI_SWAP_CHAIN_DESC &desc, uint32_t width, uint32_t height);
  void Destroy();

  // Recreates the swapchain at a new size (ResizeBuffers).  Waits for the
  // device to go idle first.
  bool Recreate(uint32_t width, uint32_t height, DXGI_FORMAT newFormat);

  // Acquires the next image.  Must be called once per frame, before anything
  // renders into the back buffer.  Returns false on failure.
  bool AcquireNextImage();
  // Queues the present of the currently acquired image.  The caller records
  // the frame command buffer (ending with the image transitioned to
  // VK_IMAGE_LAYOUT_PRESENT_SRC_KHR) and submits it waiting on
  // `SubmitWaitSemaphore()` / signalling `RenderFinishedSemaphore()`.
  VkResult Present();

  VkSemaphore SubmitWaitSemaphore() const { return m_acquireSemaphore; }
  VkSemaphore RenderFinishedSemaphore() const { return m_renderFinishedSemaphore; }
  // Consumes the "an image was acquired with a semaphore wait pending" flag.
  bool TakeSubmitWaitsOnAcquire()
  {
    bool v = m_submitWaitsOnAcquire;
    m_submitWaitsOnAcquire = false;
    return v;
  }
  void SetSubmitWaitsOnAcquire(bool v) { m_submitWaitsOnAcquire = v; }

  bool valid() const { return m_swapchain != VK_NULL_HANDLE; }
  uint32_t Width() const { return m_width; }
  uint32_t Height() const { return m_height; }
  DXGI_FORMAT Format() const { return m_format; }
  VkFormat VkFormatForImage() const { return m_vkFormat; }
  uint32_t ImageCount() const { return (uint32_t)m_images.size(); }
  uint32_t CurrentIndex() const { return m_current; }
  VkImage CurrentImage() const { return m_current < m_images.size() ? m_images[m_current] : VK_NULL_HANDLE; }
  VkImageView CurrentView() const { return m_current < m_views.size() ? m_views[m_current] : VK_NULL_HANDLE; }
  // View for an arbitrary index (used to build one RTV per back buffer).
  VkImageView ViewAt(uint32_t i) const { return i < m_views.size() ? m_views[i] : VK_NULL_HANDLE; }

  VkImageLayout LayoutAt(uint32_t i) const { return i < m_layouts.size() ? m_layouts[i] : VK_IMAGE_LAYOUT_UNDEFINED; }
  void SetLayoutAt(uint32_t i, VkImageLayout l)
  {
    if (i < m_layouts.size()) m_layouts[i] = l;
  }

  const DXGI_SWAP_CHAIN_DESC &Desc() const { return m_desc; }
  void SetDesc(const DXGI_SWAP_CHAIN_DESC &d) { m_desc = d; }
  VkSurfaceKHR Surface() const { return m_surface; }

  bool fullscreen = false;

private:
  bool CreateSurface(HWND outputWindow);
  bool CreateSwapchain(uint32_t width, uint32_t height, DXGI_FORMAT format);
  void DestroySwapchainObjects();

  VkSurfaceKHR m_surface = VK_NULL_HANDLE;
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  std::vector<VkImage> m_images;
  std::vector<VkImageView> m_views;
  std::vector<VkImageLayout> m_layouts;

  VkSemaphore m_acquireSemaphore = VK_NULL_HANDLE;
  VkSemaphore m_renderFinishedSemaphore = VK_NULL_HANDLE;

  uint32_t m_width = 0;
  uint32_t m_height = 0;
  DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
  VkFormat m_vkFormat = VK_FORMAT_R8G8B8A8_UNORM;
  VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  uint32_t m_current = 0;
  bool m_submitWaitsOnAcquire = false;
  DXGI_SWAP_CHAIN_DESC m_desc{};
};

} // namespace phxd3d11

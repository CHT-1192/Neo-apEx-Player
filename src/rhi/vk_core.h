// Minimal Vulkan core for the apEx player: instance/device bring-up, memory
// helpers, a per-frame bump allocator for dynamic constant data and a small
// image/buffer toolkit.
//
// Everything the D3D11 shim needs beyond this lives in src/d3d11.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace phxvk
{

// Non-copyable handle wrapper for the Vulkan objects we own.
struct Device
{
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties memProps{};
  VkPhysicalDeviceFeatures features{};
  bool hasDynamicRendering = false;
  bool hasPortabilitySubset = false;
  bool hasDebugUtils = false;
  // True when VK_LAYER_KHRONOS_validation was actually requested *and* found.
  // The layers are not bundled with MoltenVK, so "validation is on" is a
  // property of the machine, not of the request.
  bool validationEnabled = false;
  PFN_vkCmdBeginRenderingKHR CmdBeginRendering = nullptr;
  PFN_vkCmdEndRenderingKHR CmdEndRendering = nullptr;
  PFN_vkSetDebugUtilsObjectNameEXT SetDebugUtilsObjectName = nullptr;
  VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;

  // Command buffer used for the current frame.  D3D11 is an immediate API;
  // we record everything into this buffer and submit at Present().
  VkCommandPool framePool = VK_NULL_HANDLE;
  VkCommandBuffer frameCmd = VK_NULL_HANDLE;
  bool frameActive = false;
  VkFence frameFence = VK_NULL_HANDLE;
  // Two frames in flight, so the CPU records frame N+1 while the GPU still draws
  // frame N.  Measured before this change: 17.99 ms per frame, 10.82 ms of it
  // blocked on the GPU fence and 7.17 ms CPU -- serialised, so overlap is worth
  // about 1.6x.  frameCmd/frameFence stay the CURRENT slot's handles so that
  // every existing caller keeps working; the arrays are the per-slot storage.
  static const int kFramesInFlight = 2;
  int frameSlot = 0;
  VkCommandBuffer frameCmds[kFramesInFlight] = {};
  VkFence frameFences[kFramesInFlight] = {};
  uint64_t lastFrameStartNs = 0; // [port] for the frame-cost breakdown
  uint64_t frameIndex = 0;

  // A single staging buffer queued for destruction once the frame completes.
  // One list per slot.  A slot's list is flushed when that slot's fence has just
  // been waited on: with two frames in flight that means frame F-2 has completed,
  // and list[F%2] holds exactly what frame F-2 released.
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> deferredFree[kFramesInFlight];
  // Images that were released while a frame command buffer might still
  // reference them (the engine churns texgen textures during generation).
  std::vector<std::pair<VkImage, VkDeviceMemory>> deferredFreeImages[kFramesInFlight];
  std::vector<VkImageView> deferredFreeViews[kFramesInFlight];
};

// Global accessor; initialized by vkInitialize().
Device &Vk();
void vkInitialize(bool enableValidation, const char *appName);

// The engine only asks for validation through D3D11_CREATE_DEVICE_DEBUG, which
// the vendored CoRE2 code passes under _DEBUG.  A release player therefore has
// no way to turn the layers on, so the application can request them here before
// the device is created (D3D11CreateDevice forwards this into vkInitialize).
void vkRequestValidation(bool enable);
bool vkValidationRequested();
void vkShutdown();

// Prints the accumulated frame cost, split into "blocked on the GPU" and "the
// CPU was working", then resets the counters.
void PrintFrameStats();

// Extra instance extensions (e.g. the VK_KHR_surface / VK_EXT_metal_surface
// pair SDL asks for).  Must be called *before* vkInitialize(); extensions that
// the loader does not advertise are dropped with a warning.  This exists so
// the windowing layer can be plugged in without vk_core knowing about SDL.
void vkAddInstanceExtensions(const char *const *names, uint32_t count);
// True when `name` was requested through vkAddInstanceExtensions() *and* the
// loader advertises it (i.e. it will actually be enabled at instance creation).
bool vkInstanceExtensionRequested(const char *name);

// ---------------------------------------------------------------------------
// error handling

const char *VkResultName(VkResult r);
void vkFail(const char *what, VkResult r);
#define PHXVK_CHECK(expr)                                                    \
  do                                                                         \
  {                                                                          \
    VkResult phxvk_r_ = (expr);                                              \
    if (phxvk_r_ != VK_SUCCESS) phxvk::vkFail(#expr, phxvk_r_);              \
  } while (0)

// ---------------------------------------------------------------------------
// memory

uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props);
// Allocates device memory and binds it. Returns the memory handle.
VkDeviceMemory AllocateAndBind(VkImage image, VkMemoryRequirements req, VkMemoryPropertyFlags props);
VkDeviceMemory AllocateAndBindBuffer(VkBuffer buffer, VkMemoryRequirements req, VkMemoryPropertyFlags props);

// ---------------------------------------------------------------------------
// frame lifecycle (the D3D11 immediate context records into one command buffer)

VkCommandBuffer FrameCommandBuffer();
void FrameBegin();
void FrameEndAndSubmit();
void FrameWaitIdle();

// A swap chain must be waited on before anything touches the image it just
// acquired.  This lets the presentation layer hand that semaphore to whichever
// submission happens FIRST in a frame, without src/rhi having to know what a
// swap chain is.
//
// Why it matters here: the engine submits the frame command buffer more than
// once - PresentFrame() does, but so do SubmitAndWait() (readbacks) and
// ClearState() in the middle of a frame.  Those mid-frame submits carry the
// draws that target the back buffer, so if they do not wait on the acquire
// semaphore the GPU starts writing an image the presentation engine has not
// released yet.  On screen that is an intermittent black (or stale) frame.
//
// The hook returns the semaphore the next submission must wait on, or
// VK_NULL_HANDLE, and is expected to consume it: a binary semaphore may only be
// waited on once.
using AcquireWaitHook = VkSemaphore (*)();
void vkSetAcquireWaitHook(AcquireWaitHook hook);
void DeferFree(VkBuffer buffer, VkDeviceMemory memory);
// Same idea for images: never vkDestroyImage while the current frame's command
// buffer can still reference the image (directly or through a view/descriptor).
void DeferFreeImage(VkImage image, VkDeviceMemory memory);
// Views are referenced by descriptor sets, so they need the same deferral.
void DeferFreeImageView(VkImageView view);

// ---------------------------------------------------------------------------
// per-frame bump allocator: D3D11 dynamic resources are mapped straight into
// this ring, so no copy is needed on Unmap.

struct RingAlloc
{
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint64_t offset = 0;
  uint64_t size = 0;
  void *mapped = nullptr;
  bool valid() const { return buffer != VK_NULL_HANDLE; }
};

void RingInitialize(uint64_t bytes);
void RingShutdown();
RingAlloc RingAllocate(uint64_t size, uint64_t alignment = 256);
uint64_t RingUsed();

// Starts a frame's slice of the ring.  With more than one frame in flight a
// frame's transients must stay valid while the other frame is still drawn.
void RingBeginFrame(int slot);
uint64_t RingCapacity();

// ---------------------------------------------------------------------------
// small helpers

VkBuffer CreateBuffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkDeviceMemory *outMemory);
VkImage CreateImage2D(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, bool mipmaps, VkDeviceMemory *outMemory);
VkImageView CreateImageView2D(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers, VkImageViewType viewType, uint32_t baseMip = 0, uint32_t mipCount = 1, uint32_t baseLayer = 0, uint32_t layerCount = 1);
VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect, uint32_t mipCount, uint32_t layerCount);
void PipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, const std::vector<VkImageMemoryBarrier> &barriers);

// Records an upload of tightly packed RGBA rows into an image (used by texture
// creation).  Queues a staging buffer for destruction after the frame.
void UploadImageData(VkImage image, uint32_t width, uint32_t height, uint32_t depth, VkDeviceSize bytesPerRow, VkDeviceSize bytesPerSlice, const void *data, bool generateMips, uint32_t mipCount, VkFormat format);

// ---------------------------------------------------------------------------
// logging

void LogInfo(const char *fmt, ...);
void LogWarn(const char *fmt, ...);
void LogError(const char *fmt, ...);
void SetLogFile(const char *path);

} // namespace phxvk

#include "vk_core.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <mutex>

namespace phxvk
{

namespace
{
Device g_device;
FILE *g_logFile = nullptr;
bool g_initialized = false;
bool g_wantValidation = false;

// ring allocator
VkBuffer g_ringBuffer = VK_NULL_HANDLE;
VkDeviceMemory g_ringMemory = VK_NULL_HANDLE;
uint64_t g_ringCapacity = 0;
uint64_t g_ringOffset = 0;
// Each frame slot owns a fixed slice of the ring, because a frame's transients
// must stay valid while the other slot's frame is still being drawn.
static uint64_t g_ringSlotLimit = 0;
AcquireWaitHook g_acquireWaitHook = nullptr;
uint8_t *g_ringMapped = nullptr;

const uint64_t kRingDefaultSize = 64ull * 1024ull * 1024ull;

// Instance extensions requested by the embedding application (SDL, ...).
std::vector<std::string> g_requestedInstanceExts;
} // namespace

Device &Vk() { return g_device; }

void vkAddInstanceExtensions(const char *const *names, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++)
  {
    if (!names[i]) continue;
    bool dup = false;
    for (auto &e : g_requestedInstanceExts)
      if (e == names[i]) dup = true;
    if (!dup) g_requestedInstanceExts.emplace_back(names[i]);
  }
}

bool vkInstanceExtensionRequested(const char *name)
{
  for (auto &e : g_requestedInstanceExts)
    if (e == name) return true;
  return false;
}

//////////////////////////////////////////////////////////////////////////////
// logging

void SetLogFile(const char *path) { g_logFile = fopen(path, "w"); }

static void LogV(const char *level, const char *fmt, va_list ap)
{
  char buf[4096];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  fprintf(stderr, "[phxvk %s] %s\n", level, buf);
  if (g_logFile)
  {
    fprintf(g_logFile, "[%s] %s\n", level, buf);
    fflush(g_logFile);
  }
}

void LogInfo(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  LogV("info", fmt, ap);
  va_end(ap);
}
void LogWarn(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  LogV("warn", fmt, ap);
  va_end(ap);
}
void LogError(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  LogV("error", fmt, ap);
  va_end(ap);
}

const char *VkResultName(VkResult r)
{
  switch (r)
  {
  case VK_SUCCESS: return "VK_SUCCESS";
  case VK_NOT_READY: return "VK_NOT_READY";
  case VK_TIMEOUT: return "VK_TIMEOUT";
  case VK_EVENT_SET: return "VK_EVENT_SET";
  case VK_EVENT_RESET: return "VK_EVENT_RESET";
  case VK_INCOMPLETE: return "VK_INCOMPLETE";
  case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
  case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
  case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
  case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
  case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
  default: return "VK_ERROR_<other>";
  }
}

void vkFail(const char *what, VkResult r)
{
  LogError("Vulkan call failed (%s): %s", VkResultName(r), what);
  fprintf(stderr, "FATAL: %s -> %s\n", what, VkResultName(r));
  abort();
}

//////////////////////////////////////////////////////////////////////////////
// instance / device

static bool HasInstanceExtension(const char *name)
{
  uint32_t n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
  for (auto &e : exts)
    if (!strcmp(e.extensionName, name)) return true;
  return false;
}

static bool HasLayer(const char *name)
{
  uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, nullptr);
  std::vector<VkLayerProperties> layers(n);
  vkEnumerateInstanceLayerProperties(&n, layers.data());
  for (auto &l : layers)
    if (!strcmp(l.layerName, name)) return true;
  return false;
}

static bool HasDeviceExtension(VkPhysicalDevice pd, const char *name)
{
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
  for (auto &e : exts)
    if (!strcmp(e.extensionName, name)) return true;
  return false;
}

void vkRequestValidation(bool enable) { g_wantValidation = enable; }
bool vkValidationRequested() { return g_wantValidation; }

void vkInitialize(bool enableValidation, const char *appName)
{
  if (g_initialized) return;
  enableValidation = enableValidation || g_wantValidation;
  Device &d = g_device;

  uint32_t loaderVersion = VK_API_VERSION_1_0;
  vkEnumerateInstanceVersion(&loaderVersion);

  std::vector<const char *> layers;
  std::vector<const char *> instExts;

  // MoltenVK is a portability driver: the loader refuses to enumerate it
  // unless we opt in.
  if (HasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
    instExts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
  if (HasInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
    instExts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
  if (HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
  {
    instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    d.hasDebugUtils = true;
  }
  // VK_KHR_swapchain (enabled below) depends on the surface extension, which
  // the loader requires to be enabled at instance level even when no surface
  // is ever created.
  if (HasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME))
    instExts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);

  // Application-requested extensions (SDL surface creation).  Drop anything
  // the loader does not advertise so instance creation cannot fail on them.
  for (auto &req : g_requestedInstanceExts)
  {
    if (HasInstanceExtension(req.c_str()))
    {
      bool dup = false;
      for (auto *e : instExts)
        if (req == e) dup = true;
      if (!dup) instExts.push_back(req.c_str());
    }
    else
    {
      LogWarn("requested instance extension %s is not available; ignoring", req.c_str());
    }
  }

  // Validation layers are not bundled with MoltenVK; a request for them must
  // not be silently ignored, or a "clean" validation run means nothing.
  if (enableValidation)
  {
    if (HasLayer("VK_LAYER_KHRONOS_validation"))
    {
      layers.push_back("VK_LAYER_KHRONOS_validation");
      d.validationEnabled = true;
    }
    else
    {
      LogWarn("validation was requested but VK_LAYER_KHRONOS_validation is not available; "
              "set VK_LAYER_PATH to the directory holding VkLayer_khronos_validation.json");
    }
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = appName ? appName : "Neo-apEx-Player";
  app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app.pEngineName = "Phoenix-VK";
  app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = (uint32_t)layers.size();
  ci.ppEnabledLayerNames = layers.data();
  ci.enabledExtensionCount = (uint32_t)instExts.size();
  ci.ppEnabledExtensionNames = instExts.data();
  ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

  PHXVK_CHECK(vkCreateInstance(&ci, nullptr, &d.instance));

  if (d.hasDebugUtils)
  {
    auto createFn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(d.instance, "vkCreateDebugUtilsMessengerEXT");
    if (createFn)
    {
      VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
      dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      dbg.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data, void *) -> VkBool32 {
        if (sev == VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
          LogError("validation: %s", data->pMessage);
        else
          LogWarn("validation: %s", data->pMessage);
        return VK_FALSE;
      };
      VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
      VkResult msgRes = createFn(d.instance, &dbg, nullptr, &messenger);
      d.debugMessenger = messenger;
      if (d.validationEnabled)
      {
        if (msgRes == VK_SUCCESS)
          LogInfo("validation layer enabled: VK_LAYER_KHRONOS_validation");
        else
          LogWarn("validation layer enabled but vkCreateDebugUtilsMessengerEXT failed (%s); messages are dropped", VkResultName(msgRes));
      }
    }
    else if (d.validationEnabled)
    {
      LogWarn("validation layer enabled but VK_EXT_debug_utils is missing; validation messages are dropped");
    }
    d.SetDebugUtilsObjectName = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(d.instance, "vkSetDebugUtilsObjectNameEXT");
  }

  uint32_t count = 0;
  PHXVK_CHECK(vkEnumeratePhysicalDevices(d.instance, &count, nullptr));
  if (!count)
  {
    LogError("no Vulkan physical devices found (is MoltenVK installed and its ICD registered?)");
    abort();
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(d.instance, &count, devices.data());
  d.physical = devices[0];
  for (auto pd : devices)
  {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
    {
      d.physical = pd;
      break;
    }
  }
  vkGetPhysicalDeviceProperties(d.physical, &d.props);
  vkGetPhysicalDeviceMemoryProperties(d.physical, &d.memProps);
  vkGetPhysicalDeviceFeatures(d.physical, &d.features);

  LogInfo("Vulkan device: %s (api %u.%u.%u)", d.props.deviceName, VK_VERSION_MAJOR(d.props.apiVersion), VK_VERSION_MINOR(d.props.apiVersion), VK_VERSION_PATCH(d.props.apiVersion));

  d.hasDynamicRendering = HasDeviceExtension(d.physical, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  d.hasPortabilitySubset = HasDeviceExtension(d.physical, "VK_KHR_portability_subset");

  // queue family: prefer one that supports graphics + compute + transfer
  uint32_t qCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &qCount, nullptr);
  std::vector<VkQueueFamilyProperties> qs(qCount);
  vkGetPhysicalDeviceQueueFamilyProperties(d.physical, &qCount, qs.data());
  d.queueFamily = UINT32_MAX;
  for (uint32_t i = 0; i < qCount; i++)
  {
    if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
    {
      d.queueFamily = i;
      break;
    }
  }
  if (d.queueFamily == UINT32_MAX)
    for (uint32_t i = 0; i < qCount; i++)
      if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      {
        d.queueFamily = i;
        break;
      }
  if (d.queueFamily == UINT32_MAX)
  {
    LogError("no graphics queue family");
    abort();
  }

  std::vector<const char *> devExts;
  devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  // VK_KHR_dynamic_rendering declares VK_KHR_depth_stencil_resolve as a
  // dependency (which in turn needs VK_KHR_create_renderpass2), so those have
  // to be enabled alongside it.
  if (d.hasDynamicRendering && HasDeviceExtension(d.physical, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME))
    devExts.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
  if (d.hasDynamicRendering && HasDeviceExtension(d.physical, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME))
    devExts.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
  if (d.hasDynamicRendering) devExts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  if (d.hasPortabilitySubset) devExts.push_back("VK_KHR_portability_subset");

  // enable everything the engine could want that the device actually offers
  VkPhysicalDeviceFeatures feats{};
  feats.fillModeNonSolid = d.features.fillModeNonSolid;
  feats.largePoints = d.features.largePoints;
  feats.independentBlend = d.features.independentBlend;
  feats.depthClamp = d.features.depthClamp;
  feats.depthBiasClamp = d.features.depthBiasClamp;
  feats.sampleRateShading = d.features.sampleRateShading;
  feats.multiViewport = d.features.multiViewport;
  feats.fragmentStoresAndAtomics = d.features.fragmentStoresAndAtomics;
  feats.vertexPipelineStoresAndAtomics = d.features.vertexPipelineStoresAndAtomics;
  feats.shaderStorageImageExtendedFormats = d.features.shaderStorageImageExtendedFormats;
  feats.textureCompressionBC = d.features.textureCompressionBC;
  feats.textureCompressionASTC_LDR = d.features.textureCompressionASTC_LDR;
  feats.textureCompressionETC2 = d.features.textureCompressionETC2;
  feats.occlusionQueryPrecise = d.features.occlusionQueryPrecise;
  feats.dualSrcBlend = d.features.dualSrcBlend;
  feats.shaderInt64 = d.features.shaderInt64;
  feats.shaderInt16 = d.features.shaderInt16;
  feats.shaderClipDistance = d.features.shaderClipDistance;
  feats.imageCubeArray = d.features.imageCubeArray;
  feats.fullDrawIndexUint32 = d.features.fullDrawIndexUint32;
  feats.robustBufferAccess = d.features.robustBufferAccess;
  // The engine creates D3D11_FILTER_ANISOTROPIC samplers with MaxAnisotropy 16
  // (Phoenix/phxEngine.cpp), and vkCreateSampler rejects anisotropyEnable
  // unless the feature is on.
  feats.samplerAnisotropy = d.features.samplerAnisotropy;
  feats.geometryShader = d.features.geometryShader; // false on MoltenVK

  VkPhysicalDeviceDynamicRenderingFeaturesKHR dyn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
  dyn.dynamicRendering = VK_TRUE;
  dyn.pNext = nullptr;

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = d.queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = (uint32_t)devExts.size();
  dci.ppEnabledExtensionNames = devExts.data();
  dci.pEnabledFeatures = &feats;
  if (d.hasDynamicRendering) dci.pNext = &dyn;

  VkResult res = vkCreateDevice(d.physical, &dci, nullptr, &d.device);
  if (res != VK_SUCCESS && d.hasDynamicRendering)
  {
    // retry without dynamic rendering; callers fall back to render passes
    LogWarn("device creation with VK_KHR_dynamic_rendering failed (%s), retrying without", VkResultName(res));
    d.hasDynamicRendering = false;
    dci.pNext = nullptr;
    devExts.erase(std::remove(devExts.begin(), devExts.end(), VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME), devExts.end());
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    res = vkCreateDevice(d.physical, &dci, nullptr, &d.device);
  }
  PHXVK_CHECK(res);

  vkGetDeviceQueue(d.device, d.queueFamily, 0, &d.queue);

  if (d.hasDynamicRendering)
  {
    d.CmdBeginRendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(d.device, "vkCmdBeginRenderingKHR");
    d.CmdEndRendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(d.device, "vkCmdEndRenderingKHR");
    if (!d.CmdBeginRendering || !d.CmdEndRendering)
    {
      LogWarn("VK_KHR_dynamic_rendering advertised but entry points missing; disabling");
      d.hasDynamicRendering = false;
    }
  }

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pci.queueFamilyIndex = d.queueFamily;
  PHXVK_CHECK(vkCreateCommandPool(d.device, &pci, nullptr, &d.framePool));

  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = d.framePool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = Device::kFramesInFlight;
  PHXVK_CHECK(vkAllocateCommandBuffers(d.device, &cbai, d.frameCmds));

  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so the first waits do not hang
  for (int i = 0; i < Device::kFramesInFlight; i++)
    PHXVK_CHECK(vkCreateFence(d.device, &fci, nullptr, &d.frameFences[i]));
  d.frameSlot = 0;
  d.frameCmd = d.frameCmds[0];
  d.frameFence = d.frameFences[0];

  RingInitialize(kRingDefaultSize);

  g_initialized = true;
  LogInfo("Vulkan core ready (dynamic rendering: %s)", d.hasDynamicRendering ? "yes" : "NO");
}

void vkShutdown()
{
  Device &d = g_device;
  if (!d.device) return;
  vkDeviceWaitIdle(d.device);
  // Flush the deferred-destruction lists: vkDestroyDevice would free them
  // anyway, but leaving them queued makes the validation layers report them as
  // leaked objects, and a "clean" validation run has to mean clean.
  for (int slot = 0; slot < Device::kFramesInFlight; slot++)
  {
    for (auto &p : d.deferredFree[slot])
    {
      vkDestroyBuffer(d.device, p.first, nullptr);
      vkFreeMemory(d.device, p.second, nullptr);
    }
    d.deferredFree[slot].clear();
    for (auto &p : d.deferredFreeImages[slot])
    {
      vkDestroyImage(d.device, p.first, nullptr);
      vkFreeMemory(d.device, p.second, nullptr);
    }
    d.deferredFreeImages[slot].clear();
    for (VkImageView v : d.deferredFreeViews[slot]) vkDestroyImageView(d.device, v, nullptr);
    d.deferredFreeViews[slot].clear();
  }
  RingShutdown();
  for (int i = 0; i < Device::kFramesInFlight; i++)
    if (d.frameFences[i]) vkDestroyFence(d.device, d.frameFences[i], nullptr);
  d.frameFence = VK_NULL_HANDLE;
  if (d.framePool) vkDestroyCommandPool(d.device, d.framePool, nullptr);
  vkDestroyDevice(d.device, nullptr);
  if (d.debugMessenger)
  {
    auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(d.instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroyFn) destroyFn(d.instance, d.debugMessenger, nullptr);
    d.debugMessenger = VK_NULL_HANDLE;
  }
  vkDestroyInstance(d.instance, nullptr);
  memset(&d, 0, sizeof(d));
  g_initialized = false;
}

//////////////////////////////////////////////////////////////////////////////
// memory

uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props)
{
  Device &d = Vk();
  for (uint32_t i = 0; i < d.memProps.memoryTypeCount; i++)
  {
    if ((typeBits & (1u << i)) && (d.memProps.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  // relax: drop the optional flags
  for (uint32_t i = 0; i < d.memProps.memoryTypeCount; i++)
    if (typeBits & (1u << i)) return i;
  LogError("no suitable memory type (bits 0x%x, props 0x%x)", typeBits, props);
  return 0;
}

VkDeviceMemory AllocateAndBind(VkImage image, VkMemoryRequirements req, VkMemoryPropertyFlags props)
{
  Device &d = Vk();
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
  VkDeviceMemory mem = VK_NULL_HANDLE;
  PHXVK_CHECK(vkAllocateMemory(d.device, &ai, nullptr, &mem));
  PHXVK_CHECK(vkBindImageMemory(d.device, image, mem, 0));
  return mem;
}

VkDeviceMemory AllocateAndBindBuffer(VkBuffer buffer, VkMemoryRequirements req, VkMemoryPropertyFlags props)
{
  Device &d = Vk();
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, props);
  VkDeviceMemory mem = VK_NULL_HANDLE;
  PHXVK_CHECK(vkAllocateMemory(d.device, &ai, nullptr, &mem));
  PHXVK_CHECK(vkBindBufferMemory(d.device, buffer, mem, 0));
  return mem;
}

//////////////////////////////////////////////////////////////////////////////
// frame lifecycle

VkCommandBuffer FrameCommandBuffer() { return g_device.frameCmd; }

static void FlushDeferred(Device &d, int slot); // defined below FrameWaitIdle()

// What a frame costs, split the only way that matters for the single-frame-in-
// flight design: time spent blocking on the GPU fence (the GPU was still
// drawing) versus everything else (the CPU was working).  If the former
// dominates, more frames in flight would not help; if they are comparable, it
// would roughly halve the frame time.
static uint64_t g_waitNs = 0, g_frameNs = 0;
static uint64_t g_framesTimed = 0;

static uint64_t NowNs()
{
  using clock = std::chrono::steady_clock;
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
}

void PrintFrameStats()
{
  if (!g_framesTimed) return;
  const double frames = (double)g_framesTimed;
  LogInfo("[frame] %llu frame(s): %.2f ms each, of which %.2f ms blocked on the GPU (%.0f%%), %.2f ms CPU",
          (unsigned long long)g_framesTimed, g_frameNs / 1e6 / frames, g_waitNs / 1e6 / frames,
          g_frameNs ? 100.0 * (double)g_waitNs / (double)g_frameNs : 0.0,
          (g_frameNs - g_waitNs) / 1e6 / frames);
  g_waitNs = g_frameNs = g_framesTimed = 0;
}

void FrameBegin()
{
  Device &d = g_device;
  if (d.frameActive) return;
  d.frameSlot = (int)(d.frameIndex % (uint64_t)Device::kFramesInFlight);
  d.frameCmd = d.frameCmds[d.frameSlot];
  d.frameFence = d.frameFences[d.frameSlot];
  const uint64_t beginNs = NowNs();
  vkWaitForFences(d.device, 1, &d.frameFence, VK_TRUE, UINT64_MAX);
  g_waitNs += NowNs() - beginNs;
  if (d.lastFrameStartNs) { g_frameNs += beginNs - d.lastFrameStartNs; g_framesTimed++; }
  d.lastFrameStartNs = beginNs;
  vkResetFences(d.device, 1, &d.frameFence);

  // Release what the engine freed during the previous frame.
  //
  // This is the ONLY point where it is both safe and guaranteed to happen.
  // Safe, because the fence above means every submission of the previous frame
  // has completed (one queue, in order).  Guaranteed, because FrameBegin() runs
  // once per frame - whereas FrameWaitIdle() is reached only from
  // SubmitAndWait()/ClearState(), which steady playback never calls.
  //
  // Without this the lists grew without bound: a material that is rebuilt every
  // frame - what shadertoy events do - destroys and recreates a render target
  // each time, and each one is a full-size RGBA16F image.  Measured cost of the
  // omission, 12 frames from a given start:
  //     frame 1000 -> 7.5 s      frame 3000 -> 7.5 s      frame 6000 -> 335 s
  // i.e. 28 seconds per frame once the leak had the machine swapping, which is
  // also what made the beachball appear and the whole desktop stall.
  FlushDeferred(d, d.frameSlot);

  vkResetCommandBuffer(d.frameCmd, 0);

  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(d.frameCmd, &bi);
  d.frameActive = true;
  RingBeginFrame(d.frameSlot);
}

void FrameEndAndSubmit()
{
  Device &d = g_device;
  if (!d.frameActive) return;
  vkEndCommandBuffer(d.frameCmd);

  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};

  // Wait on the back buffer's acquire semaphore if this is the first submission
  // of the frame; see vkSetAcquireWaitHook().  ALL_COMMANDS because the frame
  // command buffer both draws into the image and transitions its layout.
  VkSemaphore acquireWait = g_acquireWaitHook ? g_acquireWaitHook() : VK_NULL_HANDLE;
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  if (acquireWait != VK_NULL_HANDLE)
  {
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquireWait;
    si.pWaitDstStageMask = &waitStage;
  }

  si.commandBufferCount = 1;
  si.pCommandBuffers = &d.frameCmd;
  PHXVK_CHECK(vkQueueSubmit(d.queue, 1, &si, d.frameFence));
  d.frameActive = false;
  d.frameIndex++;
}

// Destroys everything the engine released earlier.  Only call once the GPU is
// known to be finished with them.
static void FlushDeferred(Device &d, int slot)
{
  for (auto &p : d.deferredFree[slot])
  {
    vkDestroyBuffer(d.device, p.first, nullptr);
    vkFreeMemory(d.device, p.second, nullptr);
  }
  d.deferredFree[slot].clear();
  for (auto &p : d.deferredFreeImages[slot])
  {
    vkDestroyImage(d.device, p.first, nullptr);
    if (p.second != VK_NULL_HANDLE) vkFreeMemory(d.device, p.second, nullptr);
  }
  d.deferredFreeImages[slot].clear();
  for (VkImageView v : d.deferredFreeViews[slot])
    vkDestroyImageView(d.device, v, nullptr);
  d.deferredFreeViews[slot].clear();
}

void FrameWaitIdle()
{
  Device &d = g_device;
  vkDeviceWaitIdle(d.device);
  // Idle: every slot's work has completed, so every list is safe to flush.
  for (int slot = 0; slot < Device::kFramesInFlight; slot++) FlushDeferred(d, slot);
}

// Not inside the anonymous namespace above: this is part of the API the
// presentation layer calls, so it needs external linkage.
void vkSetAcquireWaitHook(AcquireWaitHook hook) { g_acquireWaitHook = hook; }

void DeferFree(VkBuffer buffer, VkDeviceMemory memory)
{
  Vk().deferredFree[Vk().frameSlot].emplace_back(buffer, memory);
}

void DeferFreeImage(VkImage image, VkDeviceMemory memory)
{
  if (image == VK_NULL_HANDLE) return;
  Vk().deferredFreeImages[Vk().frameSlot].emplace_back(image, memory);
}

void DeferFreeImageView(VkImageView view)
{
  if (view == VK_NULL_HANDLE) return;
  Vk().deferredFreeViews[Vk().frameSlot].push_back(view);
}

//////////////////////////////////////////////////////////////////////////////
// ring allocator

void RingInitialize(uint64_t bytes)
{
  Device &d = Vk();
  g_ringCapacity = bytes;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = bytes;
  bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  PHXVK_CHECK(vkCreateBuffer(d.device, &bi, nullptr, &g_ringBuffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d.device, g_ringBuffer, &req);
  g_ringCapacity = req.size;
  g_ringMemory = AllocateAndBindBuffer(g_ringBuffer, req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  PHXVK_CHECK(vkMapMemory(d.device, g_ringMemory, 0, VK_WHOLE_SIZE, 0, (void **)&g_ringMapped));
  g_ringOffset = 0;
  g_ringSlotLimit = g_ringCapacity;
  LogInfo("dynamic ring: %.1f MB, %.1f MB per frame slot", g_ringCapacity / 1048576.0,
          g_ringCapacity / 1048576.0 / (double)Device::kFramesInFlight);
}

void RingShutdown()
{
  Device &d = Vk();
  if (g_ringMapped)
  {
    vkUnmapMemory(d.device, g_ringMemory);
    g_ringMapped = nullptr;
  }
  if (g_ringBuffer) vkDestroyBuffer(d.device, g_ringBuffer, nullptr);
  if (g_ringMemory) vkFreeMemory(d.device, g_ringMemory, nullptr);
  g_ringBuffer = VK_NULL_HANDLE;
  g_ringMemory = VK_NULL_HANDLE;
}

void RingBeginFrame(int slot)
{
  const uint64_t slice = g_ringCapacity / (uint64_t)Device::kFramesInFlight;
  g_ringOffset = slice * (uint64_t)slot;
  g_ringSlotLimit = g_ringOffset + slice;
}

RingAlloc RingAllocate(uint64_t size, uint64_t alignment)
{
  RingAlloc a;
  if (!g_ringBuffer || size == 0) return a;
  uint64_t aligned = (g_ringOffset + alignment - 1) & ~(alignment - 1);
  if (aligned + size > g_ringSlotLimit)
  {
    // The ring is exhausted for this frame.  Growing it transparently would
    // invalidate every recorded binding, so fail loudly instead of corrupting.
    LogError("dynamic ring exhausted in this frame's slice (%llu + %llu > %llu); increase the ring size",
             (unsigned long long)aligned, (unsigned long long)size, (unsigned long long)g_ringSlotLimit);
    abort();
  }
  a.buffer = g_ringBuffer;
  a.memory = g_ringMemory;
  a.offset = aligned;
  a.size = size;
  a.mapped = g_ringMapped + aligned;
  g_ringOffset = aligned + size;
  return a;
}

uint64_t RingUsed() { return g_ringOffset; }
uint64_t RingCapacity() { return g_ringCapacity; }

//////////////////////////////////////////////////////////////////////////////
// helpers

VkBuffer CreateBuffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkDeviceMemory *outMemory)
{
  Device &d = Vk();
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size ? size : 4;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  PHXVK_CHECK(vkCreateBuffer(d.device, &bi, nullptr, &buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(d.device, buffer, &req);
  VkDeviceMemory mem = AllocateAndBindBuffer(buffer, req, props);
  if (outMemory) *outMemory = mem;
  return buffer;
}

VkImage CreateImage2D(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags, bool mipmaps, VkDeviceMemory *outMemory)
{
  Device &d = Vk();
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {w ? w : 1, h ? h : 1, 1};
  ci.mipLevels = mipmaps ? (uint32_t)std::floor(std::log2((double)(w > h ? w : h))) + 1 : 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  PHXVK_CHECK(vkCreateImage(d.device, &ci, nullptr, &image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(d.device, image, &req);
  VkDeviceMemory mem = AllocateAndBind(image, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (outMemory) *outMemory = mem;
  return image;
}

VkImageView CreateImageView2D(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers, VkImageViewType viewType, uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount)
{
  Device &d = Vk();
  VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ci.image = image;
  ci.viewType = viewType;
  ci.format = format;
  ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  ci.subresourceRange.aspectMask = aspect;
  ci.subresourceRange.baseMipLevel = baseMip;
  ci.subresourceRange.levelCount = mipCount ? mipCount : mips;
  ci.subresourceRange.baseArrayLayer = baseLayer;
  ci.subresourceRange.layerCount = layerCount ? layerCount : layers;
  VkImageView view = VK_NULL_HANDLE;
  VkResult r = vkCreateImageView(d.device, &ci, nullptr, &view);
  if (r != VK_SUCCESS)
  {
    LogError("vkCreateImageView failed (%s) format=%d viewType=%d", VkResultName(r), (int)format, (int)viewType);
    return VK_NULL_HANDLE;
  }
  return view;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspect, uint32_t mipCount, uint32_t layerCount)
{
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange.aspectMask = aspect;
  b.subresourceRange.baseMipLevel = 0;
  b.subresourceRange.levelCount = mipCount;
  b.subresourceRange.baseArrayLayer = 0;
  b.subresourceRange.layerCount = layerCount;
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  return b;
}

void PipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, const std::vector<VkImageMemoryBarrier> &barriers)
{
  if (barriers.empty()) return;
  // Defensive: a barrier over a null VkImage makes MoltenVK dereference a null
  // MVKImage and crash with no diagnostic, so drop those entries (and say so
  // once) rather than letting the driver die.
  std::vector<VkImageMemoryBarrier> valid;
  valid.reserve(barriers.size());
  for (const auto &b : barriers)
    if (b.image != VK_NULL_HANDLE) valid.push_back(b);
  if (valid.empty())
  {
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      LogWarn("PipelineBarrier: every barrier referenced a null image; skipped (further occurrences suppressed)");
    }
    return;
  }
  if (valid.size() != barriers.size())
  {
    static bool warnedSize = false;
    if (!warnedSize)
    {
      warnedSize = true;
      LogWarn("PipelineBarrier: dropped %zu barrier(s) over a null image (further occurrences suppressed)",
              barriers.size() - valid.size());
    }
  }
  vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, (uint32_t)valid.size(), valid.data());
}

void UploadImageData(VkImage image, uint32_t width, uint32_t height, uint32_t depth, VkDeviceSize bytesPerRow, VkDeviceSize bytesPerSlice, const void *data, bool generateMips, uint32_t mipCount, VkFormat)
{
  Device &d = Vk();
  FrameBegin();
  VkCommandBuffer cmd = d.frameCmd;

  const VkDeviceSize total = bytesPerSlice * depth;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  staging = CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingMem);
  void *mapped = nullptr;
  PHXVK_CHECK(vkMapMemory(d.device, stagingMem, 0, total, 0, &mapped));
  memcpy(mapped, data, (size_t)total);
  vkUnmapMemory(d.device, stagingMem);

  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageMemoryBarrier toDst = ImageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, mipCount, 1);
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);

  std::vector<VkBufferImageCopy> copies;
  if (depth > 1)
  {
    for (uint32_t z = 0; z < depth; z++)
    {
      VkBufferImageCopy c{};
      c.bufferOffset = bytesPerSlice * z;
      c.bufferRowLength = 0;
      c.bufferImageHeight = 0;
      c.imageSubresource = {aspect, 0, 0, 1};
      c.imageOffset = {0, 0, (int32_t)z};
      c.imageExtent = {width, height, 1};
      copies.push_back(c);
    }
  }
  else
  {
    VkBufferImageCopy c{};
    c.bufferOffset = 0;
    c.imageSubresource = {aspect, 0, 0, 1};
    c.imageExtent = {width, height, 1};
    copies.push_back(c);
  }
  vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(), copies.data());

  VkImageMemoryBarrier toRead = ImageBarrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect, mipCount, 1);
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);

  (void)generateMips;
  (void)bytesPerRow;
  DeferFree(staging, stagingMem);
}

} // namespace phxvk

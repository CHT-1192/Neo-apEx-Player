// ID3D11DeviceContext: the D3D11 immediate context mapped onto one Vulkan
// command buffer, dynamic rendering and lazily built pipelines.
#include "d3d11_internal.h"

#include "rhi/gs_emulation.h"

#include "dxgi_formats.h"

#include <algorithm>
#include <cstdio>

namespace phxd3d11
{

namespace
{

constexpr uint64_t kDummyBufferSize = 64 * 1024;

// --- layout transition helper -------------------------------------------------

struct TransitionSpec
{
  VkPipelineStageFlags srcStage;
  VkAccessFlags srcAccess;
  VkPipelineStageFlags dstStage;
  VkAccessFlags dstAccess;
};

TransitionSpec SpecFor(VkImageLayout oldLayout, VkImageLayout newLayout)
{
  TransitionSpec s{};
  switch (oldLayout)
  {
  case VK_IMAGE_LAYOUT_UNDEFINED:
  case VK_IMAGE_LAYOUT_PREINITIALIZED:
    s.srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    s.srcAccess = 0;
    break;
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    s.srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    s.srcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    s.srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    s.srcAccess = VK_ACCESS_TRANSFER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    s.srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    s.srcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
  case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    s.srcStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    s.srcAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    s.srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    s.srcAccess = VK_ACCESS_SHADER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    s.srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    s.srcAccess = 0;
    break;
  case VK_IMAGE_LAYOUT_GENERAL:
    s.srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s.srcAccess = VK_ACCESS_MEMORY_WRITE_BIT;
    break;
  default:
    s.srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s.srcAccess = VK_ACCESS_MEMORY_WRITE_BIT;
    break;
  }

  switch (newLayout)
  {
  case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    s.dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    s.dstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
    break;
  case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    s.dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    s.dstAccess = VK_ACCESS_TRANSFER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    s.dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    s.dstAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
  case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    s.dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    s.dstAccess = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    s.dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    s.dstAccess = VK_ACCESS_SHADER_READ_BIT;
    break;
  case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
    s.dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    s.dstAccess = 0;
    break;
  case VK_IMAGE_LAYOUT_GENERAL:
    s.dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s.dstAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    break;
  default:
    s.dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    s.dstAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    break;
  }
  return s;
}

VkImageAspectFlags AspectForTexture(const TextureImpl *tex)
{
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  if (DXGIFormatIsDepth(tex->format)) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (DXGIFormatIsStencil(tex->format)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
  return aspect;
}

VkExtent2D TextureExtent(const TextureImpl *tex)
{
  return {tex->width ? tex->width : 1, tex->height ? tex->height : 1};
}

// Defaults D3D11 applies when no state object is bound.
const D3D11_RASTERIZER_DESC kDefaultRasterizer = {D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE};
const D3D11_DEPTH_STENCIL_DESC kDefaultDepthStencil = {
    TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE, 0xff, 0xff,
    {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS},
    {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS}};

D3D11_BLEND_DESC DefaultBlendDesc()
{
  D3D11_BLEND_DESC d;
  memset(&d, 0, sizeof(d));
  for (int i = 0; i < 8; i++)
  {
    d.RenderTarget[i].BlendEnable = FALSE;
    d.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
    d.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
    d.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
    d.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    d.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  }
  return d;
}

const D3D11_BLEND_DESC kDefaultBlend = DefaultBlendDesc();

} // namespace

namespace
{

// D3D11 semantics: the device context holds a reference to every object bound
// to it, so an application may Release() its own reference immediately after
// binding - the engine does exactly that with one-shot state objects.  Storing
// raw pointers instead made the next draw read freed memory: a use-after-free
// whose symptom depends on which object the allocator handed that memory to
// afterwards (culled draws, wrong formats, crashes).  Everything bound goes
// through BindRef()/ClearRef() now, and ResetState() releases it all.
template <class T>
inline void BindRef(T *&slot, T *value)
{
  if (slot == value) return;
  if (value) value->AddRef();
  if (slot) slot->Release();
  slot = value;
}

// Bound shaders are stored as their ShaderCommon sub-object, so the reference
// is taken on the interface and released through the stage's own type (only
// the matching Set*Shader ever writes that slot).
template <class Iface, class Impl>
inline void BindShader(ShaderCommon *&slot, Iface *shader)
{
  Impl *impl = shader ? static_cast<Impl *>(shader) : nullptr;
  if (slot == static_cast<ShaderCommon *>(impl)) return;
  if (shader) shader->AddRef();
  if (slot) static_cast<Iface *>(static_cast<Impl *>(slot))->Release();
  slot = impl;
}

// Generic setter for the per-stage bind tables.
template <class T, size_t N>
void SetTable(T *(&table)[N], UINT start, UINT count, T *const *objects)
{
  if (start >= N) return;
  if (count > N - start) count = (UINT)(N - start);
  for (UINT i = 0; i < count; i++) BindRef(table[start + i], objects ? objects[i] : nullptr);
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// lifetime

ContextImpl::~ContextImpl()
{
  ResetState(); // drop the references the context holds, like D3D11 does
  DestroyVulkanObjects();
  TrackObjectDestroyed("ID3D11DeviceContext", this);
}

ULONG ContextImpl::Release(void) { return ReleaseImpl(); }

HRESULT ContextImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11DeviceContext)))
  {
    *ppvObject = static_cast<ID3D11DeviceContext *>(this);
    AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}

void ContextImpl::GetDevice(ID3D11Device **ppDevice)
{
  if (ppDevice)
  {
    *ppDevice = device;
    if (device) device->AddRefImpl();
  }
}
HRESULT ContextImpl::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); }
HRESULT ContextImpl::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); }
HRESULT ContextImpl::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); }

D3D11_DEVICE_CONTEXT_TYPE ContextImpl::GetType(void) { return D3D11_DEVICE_CONTEXT_IMMEDIATE; }

void ContextImpl::DestroyVulkanObjects()
{
  phxvk::Device &d = phxvk::Vk();
  if (!d.device) return;
  vkDeviceWaitIdle(d.device);
  for (auto &e : emulatedGeometryCache)
  {
    if (e.second.vbo) vkDestroyBuffer(d.device, e.second.vbo, nullptr);
    if (e.second.vboMemory) vkFreeMemory(d.device, e.second.vboMemory, nullptr);
    if (e.second.ibo) vkDestroyBuffer(d.device, e.second.ibo, nullptr);
    if (e.second.iboMemory) vkFreeMemory(d.device, e.second.iboMemory, nullptr);
  }
  emulatedGeometryCache.clear();
  for (auto &p : pipelines)
    if (p.second) vkDestroyPipeline(d.device, p.second, nullptr);
  pipelines.clear();
  for (auto &p : pipelineLayouts)
    if (p.second) vkDestroyPipelineLayout(d.device, p.second, nullptr);
  pipelineLayouts.clear();
  for (auto &p : setLayouts)
    if (p.second) vkDestroyDescriptorSetLayout(d.device, p.second, nullptr);
  setLayouts.clear();
  for (auto &pool : descriptorPools)
    if (pool) vkDestroyDescriptorPool(d.device, pool, nullptr);
  descriptorPools.clear();
  if (dummySampler) vkDestroySampler(d.device, dummySampler, nullptr);
  if (dummyImageView) vkDestroyImageView(d.device, dummyImageView, nullptr);
  if (dummyImage) vkDestroyImage(d.device, dummyImage, nullptr);
  if (dummyImageMemory) vkFreeMemory(d.device, dummyImageMemory, nullptr);
  if (dummyBuffer) vkDestroyBuffer(d.device, dummyBuffer, nullptr);
  if (dummyBufferMemory) vkFreeMemory(d.device, dummyBufferMemory, nullptr);
  dummySampler = VK_NULL_HANDLE;
  dummyImageView = VK_NULL_HANDLE;
  dummyImage = VK_NULL_HANDLE;
  dummyImageMemory = VK_NULL_HANDLE;
  dummyBuffer = VK_NULL_HANDLE;
  dummyBufferMemory = VK_NULL_HANDLE;
  dummiesReady = false;
}

//////////////////////////////////////////////////////////////////////////////
// frame handling

void ContextImpl::ResetDescriptorPools()
{
  phxvk::Device &d = phxvk::Vk();
  if (!d.device) return;
  // FrameBegin() waited on the previous frame's fence, so no descriptor set
  // from an earlier frame is still in use.
  for (auto &pool : descriptorPools)
    if (pool) vkResetDescriptorPool(d.device, pool, 0);
}

bool ContextImpl::BeginFrameIfNeeded()
{
  phxvk::Device &d = phxvk::Vk();
  if (!d.frameActive) phxvk::FrameBegin();
  if (!poolsCleared)
  {
    // Safe: FrameBegin() waited on the previous frame's fence, so no descriptor
    // set recorded earlier is still in use.
    ResetDescriptorPools();
    poolsCleared = true;
  }
  return true;
}

void ContextImpl::EndRendering()
{
  if (!renderingActive) return;
  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();
  renderingActive = false;
  phxvk::Vk().CmdEndRendering(cmd);
}

void ContextImpl::SubmitAndWait()
{
  phxvk::Device &d = phxvk::Vk();
  if (!d.frameActive) return;
  EndRendering();
  phxvk::FrameEndAndSubmit();
  vkWaitForFences(d.device, 1, &d.frameFence, VK_TRUE, UINT64_MAX);
  phxvk::FrameWaitIdle();
}

void ContextImpl::OnSwapchainRecreated()
{
  rtvCount = 0;
  dsv = nullptr;
  memset(rtv, 0, sizeof(rtv));
}

//////////////////////////////////////////////////////////////////////////////
// dummy descriptors

void ContextImpl::EnsureDummies()
{
  if (dummiesReady) return;
  phxvk::Device &d = phxvk::Vk();

  dummyBuffer = phxvk::CreateBuffer(kDummyBufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &dummyBufferMemory);
  if (dummyBuffer)
  {
    void *mapped = nullptr;
    if (vkMapMemory(d.device, dummyBufferMemory, 0, kDummyBufferSize, 0, &mapped) == VK_SUCCESS) memset(mapped, 0, kDummyBufferSize);
  }

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8B8A8_UNORM;
  ci.extent = {1, 1, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(d.device, &ci, nullptr, &dummyImage) == VK_SUCCESS)
  {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d.device, dummyImage, &req);
    dummyImageMemory = phxvk::AllocateAndBind(dummyImage, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = dummyImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(d.device, &vi, nullptr, &dummyImageView) != VK_SUCCESS) dummyImageView = VK_NULL_HANDLE;
  }

  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = VK_FILTER_NEAREST;
  si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 0.0f;
  vkCreateSampler(d.device, &si, nullptr, &dummySampler);

  dummiesReady = true;
}

//////////////////////////////////////////////////////////////////////////////
// image layout tracking

void ContextImpl::TransitionImage(TextureImpl *tex, VkImageLayout newLayout)
{
  if (!tex) return;
  if (tex->swapchain)
  {
    if (tex->currentLayout() == newLayout) return;
  }
  else if (tex->layout == newLayout)
  {
    return;
  }

  const VkImageLayout oldLayout = tex->currentLayout();
  const VkImageAspectFlags aspect = AspectForTexture(tex);
  const uint32_t mips = tex->mipLevels ? tex->mipLevels : 1;
  const uint32_t layers = (tex->dimension == D3D11_RESOURCE_DIMENSION_TEXTURE3D) ? 1 : (tex->arraySize ? tex->arraySize : 1);

  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = tex->vkImage();
  if (barrier.image == VK_NULL_HANDLE)
  {
    // A texture whose VkImage was never created (creation failed, or the
    // resource is a placeholder).  Submitting the barrier anyway makes MoltenVK
    // dereference a null MVKImage inside applyImageMemoryBarrier, which is a
    // hard crash with no diagnostic; skip it instead and say so once per site.
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      phxvk::LogWarn("TransitionImage: texture has no VkImage; skipping the layout transition (further occurrences suppressed)");
    }
    tex->setCurrentLayout(newLayout);
    return;
  }
  barrier.subresourceRange = {aspect, 0, mips, 0, layers};
  TransitionSpec spec = SpecFor(oldLayout, newLayout);
  barrier.srcAccessMask = spec.srcAccess;
  barrier.dstAccessMask = spec.dstAccess;

  BeginFrameIfNeeded();
  vkCmdPipelineBarrier(phxvk::FrameCommandBuffer(), spec.srcStage, spec.dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  tex->setCurrentLayout(newLayout);
}

//////////////////////////////////////////////////////////////////////////////
// render pass instance

void ContextImpl::EnsureRenderingStarted()
{
  if (renderingActive) return;
  BeginFrameIfNeeded();

  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();

  VkRenderingAttachmentInfoKHR colorAttachments[kMaxRenderTargets]{};
  UINT colorCount = 0;
  VkExtent2D extent{1, 1};
  for (UINT i = 0; i < rtvCount && i < kMaxRenderTargets; i++)
  {
    auto *v = static_cast<RTVImpl *>(rtv[i]);
    if (!v || !v->v.texture) continue;
    TextureImpl *tex = v->v.texture;
    TransitionImage(tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (colorCount == 0) extent = TextureExtent(tex);
    colorAttachments[colorCount].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
    colorAttachments[colorCount].imageView = v->v.vkView();
    colorAttachments[colorCount].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachments[colorCount].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachments[colorCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorCount++;
  }

  VkRenderingAttachmentInfoKHR depthAttachment{};
  bool hasDepth = false;
  if (dsv)
  {
    auto *v = static_cast<DSVImpl *>(dsv);
    if (v && v->v.texture)
    {
      TransitionImage(v->v.texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
      if (colorCount == 0) extent = TextureExtent(v->v.texture);
      depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
      depthAttachment.imageView = v->v.view;
      depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      hasDepth = true;
    }
  }

  VkRenderingInfoKHR info{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
  info.renderArea = {{0, 0}, extent};
  info.layerCount = 1;
  info.colorAttachmentCount = colorCount;
  info.pColorAttachments = colorCount ? colorAttachments : nullptr;
  info.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;
  phxvk::Vk().CmdBeginRendering(cmd, &info);
  renderingActive = true;

  ApplyGraphicsState(cmd);
}

// Applies the dynamic state D3D11 keeps in the context.
namespace
{
// rtv[] comes straight from the engine and may contain null entries (it binds
// sparse target lists).  Every accessor goes through this rather than casting
// and dereferencing: doing that crashed in ApplyGraphicsState and PrepareDraw.
inline TextureImpl *RtvTexture(ID3D11RenderTargetView *v)
{
  return v ? static_cast<RTVImpl *>(v)->v.texture : nullptr;
}
} // namespace

bool ContextImpl::ApplyGraphicsState(VkCommandBuffer cmd)
{
  D3D11_VIEWPORT vp = viewport;
  if (!viewportSet)
  {
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    TextureImpl *firstTarget = rtvCount ? RtvTexture(rtv[0]) : nullptr;
    vp.Width = (FLOAT)(firstTarget ? TextureExtent(firstTarget).width : 1);
    vp.Height = (FLOAT)(firstTarget ? TextureExtent(firstTarget).height : 1);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
  }
  if (vp.Width <= 0 || vp.Height <= 0) return false;

  // D3D11's NDC has +y up while Vulkan's has +y down; a negative viewport
  // height reproduces the D3D mapping (and the winding flip is compensated in
  // the pipeline's frontFace).
  VkViewport vkViewport{};
  vkViewport.x = vp.TopLeftX;
  vkViewport.y = vp.TopLeftY + vp.Height;
  vkViewport.width = vp.Width;
  vkViewport.height = -vp.Height;
  vkViewport.minDepth = vp.MinDepth;
  vkViewport.maxDepth = vp.MaxDepth;
  vkCmdSetViewport(cmd, 0, 1, &vkViewport);

  VkExtent2D extent{1, 1};
  TextureImpl *firstBound = rtvCount ? RtvTexture(rtv[0]) : nullptr;
  if (firstBound) extent = TextureExtent(firstBound);
  else if (dsv && static_cast<DSVImpl *>(dsv)->v.texture) extent = TextureExtent(static_cast<DSVImpl *>(dsv)->v.texture);

  VkRect2D vkScissor{};
  if (scissorSet)
  {
    vkScissor.offset = {scissor.left, scissor.top};
    vkScissor.extent = {(uint32_t)(scissor.right > scissor.left ? scissor.right - scissor.left : 0),
                        (uint32_t)(scissor.bottom > scissor.top ? scissor.bottom - scissor.top : 0)};
  }
  else
  {
    vkScissor.offset = {0, 0};
    vkScissor.extent = extent;
  }
  // Vulkan requires the scissor to be inside the render area.
  if (vkScissor.offset.x < 0) vkScissor.offset.x = 0;
  if (vkScissor.offset.y < 0) vkScissor.offset.y = 0;
  if (vkScissor.offset.x > (int32_t)extent.width) vkScissor.offset.x = extent.width;
  if (vkScissor.offset.y > (int32_t)extent.height) vkScissor.offset.y = extent.height;
  if (vkScissor.offset.x + (int32_t)vkScissor.extent.width > (int32_t)extent.width) vkScissor.extent.width = extent.width - vkScissor.offset.x;
  if (vkScissor.offset.y + (int32_t)vkScissor.extent.height > (int32_t)extent.height) vkScissor.extent.height = extent.height - vkScissor.offset.y;
  if (vkScissor.extent.width == 0) vkScissor.extent.width = 1;
  if (vkScissor.extent.height == 0) vkScissor.extent.height = 1;
  vkCmdSetScissor(cmd, 0, 1, &vkScissor);

  vkCmdSetBlendConstants(cmd, blendFactor);
  vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, stencilRef);
  return true;
}

//////////////////////////////////////////////////////////////////////////////
// descriptor sets

VkDescriptorSet ContextImpl::AllocateDescriptorSet(VkDescriptorSetLayout layout)
{
  phxvk::Device &d = phxvk::Vk();
  if (descriptorPools.empty())
  {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},     {VK_DESCRIPTOR_TYPE_SAMPLER, 2048},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048},   {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 8192;
    pci.poolSizeCount = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
    pci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult r = vkCreateDescriptorPool(d.device, &pci, nullptr, &pool);
    if (r != VK_SUCCESS)
    {
      phxvk::LogError("vkCreateDescriptorPool failed: %s", phxvk::VkResultName(r));
      return VK_NULL_HANDLE;
    }
    descriptorPools.push_back(pool);
  }

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = descriptorPools.back();
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkResult r = vkAllocateDescriptorSets(d.device, &ai, &set);
  if (r != VK_SUCCESS)
  {
    // Grow: a second pool starts fresh.
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4096}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},   {VK_DESCRIPTOR_TYPE_SAMPLER, 2048},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048},          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1024},
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 8192;
    pci.poolSizeCount = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
    pci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(d.device, &pci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    descriptorPools.push_back(pool);
    ai.descriptorPool = pool;
    if (vkAllocateDescriptorSets(d.device, &ai, &set) != VK_SUCCESS)
    {
      phxvk::LogError("vkAllocateDescriptorSets failed even after growing the pool");
      return VK_NULL_HANDLE;
    }
  }
  return set;
}

std::vector<MergedBinding> ContextImpl::MergeBindings() const
{
  std::vector<MergedBinding> merged;
  auto findOrAdd = [&merged](uint32_t binding) -> MergedBinding * {
    for (auto &m : merged)
      if (m.desc.binding == binding) return &m;
    return nullptr;
  };

  for (int s = 0; s < kStageCount; s++)
  {
    if (s == kStageCS && (stages[kStageVS].shader || stages[kStagePS].shader)) continue;
    const ShaderCommon *shader = stages[s].shader;
    if (!shader) continue;
    VkShaderStageFlags stageFlag = VK_SHADER_STAGE_VERTEX_BIT;
    switch (s)
    {
    case kStageVS: stageFlag = VK_SHADER_STAGE_VERTEX_BIT; break;
    case kStagePS: stageFlag = VK_SHADER_STAGE_FRAGMENT_BIT; break;
    case kStageGS: stageFlag = VK_SHADER_STAGE_GEOMETRY_BIT; break;
    case kStageHS: stageFlag = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT; break;
    case kStageDS: stageFlag = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT; break;
    case kStageCS: stageFlag = VK_SHADER_STAGE_COMPUTE_BIT; break;
    default: break;
    }

    for (auto &b : shader->reflection.bindings)
    {
      phxvk::DescriptorBinding d = b;
      // A UAV band binding is always read-write in SPIR-V terms.
      if (d.regClass == phxvk::RegisterClass::UAV)
      {
        if (d.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) d.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        else if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) d.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) d.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      }
      else if (d.regClass == phxvk::RegisterClass::ConstantBuffer && d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
      {
        d.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      }

      MergedBinding *existing = findOrAdd(d.binding);
      if (existing)
      {
        if (existing->desc.type != d.type)
        {
          phxvk::LogWarn("descriptor binding %u is declared with two different types (%d vs %d); keeping the first", d.binding, (int)existing->desc.type, (int)d.type);
        }
        existing->stages |= stageFlag;
        continue;
      }
      MergedBinding m;
      m.desc = d;
      m.stages = stageFlag;
      merged.push_back(m);
    }
  }

  // Dynamic offsets must be supplied in ascending binding order.
  std::sort(merged.begin(), merged.end(), [](const MergedBinding &a, const MergedBinding &b) { return a.desc.binding < b.desc.binding; });
  return merged;
}

VkDescriptorSetLayout ContextImpl::GetSetLayout(const std::vector<MergedBinding> &bindings)
{
  std::vector<uint32_t> key;
  key.reserve(bindings.size() * 3);
  for (auto &b : bindings)
  {
    key.push_back(b.desc.binding);
    key.push_back((uint32_t)b.desc.type);
    key.push_back(b.desc.arraySize ? b.desc.arraySize : 1);
    key.push_back((uint32_t)b.stages);
  }

  auto it = setLayouts.find(key);
  if (it != setLayouts.end()) return it->second;

  std::vector<VkDescriptorSetLayoutBinding> vkBindings;
  vkBindings.reserve(bindings.size());
  for (auto &b : bindings)
  {
    VkDescriptorSetLayoutBinding vb{};
    vb.binding = b.desc.binding;
    vb.descriptorType = b.desc.type;
    vb.descriptorCount = b.desc.arraySize ? b.desc.arraySize : 1;
    vb.stageFlags = b.stages;
    vb.pImmutableSamplers = nullptr;
    vkBindings.push_back(vb);
  }

  VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ci.bindingCount = (uint32_t)vkBindings.size();
  ci.pBindings = vkBindings.empty() ? nullptr : vkBindings.data();
  VkDescriptorSetLayout layout = VK_NULL_HANDLE;
  VkResult res = vkCreateDescriptorSetLayout(phxvk::Vk().device, &ci, nullptr, &layout);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkCreateDescriptorSetLayout failed: %s", phxvk::VkResultName(res));
    return VK_NULL_HANDLE;
  }
  setLayouts[key] = layout;
  return layout;
}

VkPipelineLayout ContextImpl::GetPipelineLayout(VkDescriptorSetLayout setLayout, const std::vector<MergedBinding> &)
{
  // Pipeline layouts are cached alongside their (single) descriptor set layout.
  auto it = pipelineLayouts.find(setLayout);
  if (it != pipelineLayouts.end()) return it->second;

  VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ci.setLayoutCount = 1;
  ci.pSetLayouts = &setLayout;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkResult res = vkCreatePipelineLayout(phxvk::Vk().device, &ci, nullptr, &layout);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkCreatePipelineLayout failed: %s", phxvk::VkResultName(res));
    return VK_NULL_HANDLE;
  }
  pipelineLayouts[setLayout] = layout;
  return layout;
}

// Moves every sampled image a draw is about to read into
// SHADER_READ_ONLY_OPTIMAL.  This replaces the transitions that used to happen
// inside WriteDescriptorSet, which is called *after* vkCmdBeginRendering -
// where an image layout transition is illegal
// (VUID-vkCmdPipelineBarrier-None-09553).  D3D11 makes that easy to hit: the
// engine renders into a target and then samples it.
//
// TransitionImage() is a no-op when the layout already matches, so calling this
// for every draw costs nothing in the common case.
void ContextImpl::TransitionBindings(const std::vector<MergedBinding> &bindings)
{
  std::vector<TextureImpl *> pending;
  for (auto &b : bindings)
  {
    if (b.desc.regClass != phxvk::RegisterClass::Texture) continue;
    if (b.desc.type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && b.desc.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;

    const UINT count = b.desc.arraySize ? b.desc.arraySize : 1;
    const uint32_t reg = b.desc.regIndex;
    for (UINT i = 0; i < count; i++)
    {
      for (int s = 0; s < kStageCount; s++)
      {
        ID3D11ShaderResourceView *srv = stages[s].srv[reg + i];
        if (!srv) continue;
        TextureImpl *tex = static_cast<SRVImpl *>(srv)->v.texture;
        if (!tex) continue;
        if (tex->currentLayout() == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) continue;
        if (std::find(pending.begin(), pending.end(), tex) == pending.end()) pending.push_back(tex);
      }
    }
  }
  if (pending.empty()) return;

  // Nothing to do in the common case (which is why this is cheap); when there
  // is, the pass has to be split because the barrier may not be recorded
  // inside it.  EnsureRenderingStarted() re-opens it with loadOp = LOAD, so no
  // attachment contents are lost.
  if (renderingActive) EndRendering();
  for (TextureImpl *tex : pending) TransitionImage(tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// A descriptor write inside the render pass must not change a layout; if it
// would, TransitionBindings() was not called (see DrawInternal).
void ContextImpl::CheckSampledLayout(TextureImpl *tex)
{
  if (!tex) return;
  if (tex->currentLayout() != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
  {
    static Once once;
    if (once())
      phxvk::LogError("[d3d11] a sampled image is not in SHADER_READ_ONLY_OPTIMAL at draw time "
                      "(layout %d); TransitionBindings() missed it", (int)tex->currentLayout());
  }
}

bool ContextImpl::WriteDescriptorSet(VkDescriptorSet set, const std::vector<MergedBinding> &bindings, std::vector<uint32_t> &dynamicOffsets)
{
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorBufferInfo> bufferInfos;
  std::vector<VkDescriptorImageInfo> imageInfos;
  writes.reserve(bindings.size());
  bufferInfos.reserve(bindings.size() * 2);
  imageInfos.reserve(bindings.size() * 2);
  dynamicOffsets.clear();

  for (auto &b : bindings)
  {
    const UINT count = b.desc.arraySize ? b.desc.arraySize : 1;
    const uint32_t reg = b.desc.regIndex;

    // Pick the D3D11 object this binding reads from.
    ID3D11Buffer *cbBuffer = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    ID3D11SamplerState *sampler = nullptr;
    ID3D11UnorderedAccessView *uav = nullptr;
    int ownerStage = -1;
    for (int s = 0; s < kStageCount; s++)
    {
      if (!(b.stages & (s == kStageVS   ? VK_SHADER_STAGE_VERTEX_BIT
                        : s == kStagePS ? VK_SHADER_STAGE_FRAGMENT_BIT
                        : s == kStageGS ? VK_SHADER_STAGE_GEOMETRY_BIT
                        : s == kStageHS ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT
                        : s == kStageDS ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                                        : VK_SHADER_STAGE_COMPUTE_BIT)))
        continue;
      ownerStage = s;
      break;
    }
    if (ownerStage < 0) ownerStage = kStageVS;

    switch (b.desc.regClass)
    {
    case phxvk::RegisterClass::ConstantBuffer: cbBuffer = stages[ownerStage].cb[reg]; break;
    case phxvk::RegisterClass::Texture: srv = stages[ownerStage].srv[reg]; break;
    case phxvk::RegisterClass::Sampler: sampler = stages[ownerStage].samplers[reg]; break;
    case phxvk::RegisterClass::UAV: uav = stages[ownerStage].uav[reg]; break;
    default: break;
    }

    const uint32_t infoBase = (uint32_t)bufferInfos.size();

    auto pushDummyBuffer = [&]() -> uint32_t {
      VkDescriptorBufferInfo bi{};
      bi.buffer = dummyBuffer;
      bi.offset = 0;
      bi.range = kDummyBufferSize;
      bufferInfos.push_back(bi);
      return (uint32_t)bufferInfos.size() - 1;
    };

    switch (b.desc.type)
    {
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
    {
      for (UINT i = 0; i < count; i++)
      {
        BufferImpl *buf = cbBuffer ? static_cast<BufferImpl *>(cbBuffer) : nullptr;
        if (buf && buf->boundBuffer() != VK_NULL_HANDLE)
        {
          VkDescriptorBufferInfo bi{};
          bi.buffer = buf->boundBuffer();
          bi.offset = 0;
          uint64_t range = b.desc.blockSize ? b.desc.blockSize : buf->size();
          if (range == 0) range = 16;
          bi.range = range;
          bufferInfos.push_back(bi);
          dynamicOffsets.push_back((uint32_t)buf->boundOffset());
        }
        else
        {
          pushDummyBuffer();
          dynamicOffsets.push_back(0);
        }
      }
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = b.desc.binding;
      w.descriptorCount = count;
      w.descriptorType = b.desc.type;
      w.pBufferInfo = bufferInfos.empty() ? nullptr : &bufferInfos[infoBase];
      writes.push_back(w);
      break;
    }
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
    {
      for (UINT i = 0; i < count; i++)
      {
        BufferImpl *buf = nullptr;
        if (b.desc.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
          buf = cbBuffer ? static_cast<BufferImpl *>(cbBuffer) : nullptr;
        else if (uav)
          buf = static_cast<UAVImpl *>(uav)->v.buffer;
        else if (srv)
          buf = static_cast<SRVImpl *>(srv)->v.buffer;

        if (buf && buf->boundBuffer() != VK_NULL_HANDLE)
        {
          VkDescriptorBufferInfo bi{};
          bi.buffer = buf->boundBuffer();
          bi.offset = 0;
          bi.range = buf->size() ? buf->size() : VK_WHOLE_SIZE;
          bufferInfos.push_back(bi);
        }
        else
        {
          pushDummyBuffer();
        }
      }
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = b.desc.binding;
      w.descriptorCount = count;
      w.descriptorType = b.desc.type;
      w.pBufferInfo = &bufferInfos[infoBase];
      writes.push_back(w);
      break;
    }
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    {
      const uint32_t infoStart = (uint32_t)imageInfos.size();
      for (UINT i = 0; i < count; i++)
      {
        VkImageView view = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (b.desc.type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && srv)
        {
          auto *s = static_cast<SRVImpl *>(srv);
          view = s->v.vkView();
          // The layout transition already happened in TransitionBindings(),
          // before the render pass started: it is illegal in here.
          CheckSampledLayout(s->v.texture);
        }
        else if (b.desc.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && uav)
        {
          auto *u = static_cast<UAVImpl *>(uav);
          view = u->v.vkView();
          layout = VK_IMAGE_LAYOUT_GENERAL;
        }
        if (view == VK_NULL_HANDLE)
        {
          view = dummyImageView;
          layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          if (!view)
          {
            phxvk::LogError("draw: no dummy image view available for unbound binding %u", b.desc.binding);
            return false;
          }
        }
        VkDescriptorImageInfo ii{};
        ii.imageView = view;
        ii.imageLayout = layout;
        ii.sampler = VK_NULL_HANDLE;
        imageInfos.push_back(ii);
      }
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = b.desc.binding;
      w.descriptorCount = count;
      w.descriptorType = b.desc.type;
      w.pImageInfo = &imageInfos[infoStart];
      writes.push_back(w);
      break;
    }
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    {
      const uint32_t infoStart = (uint32_t)imageInfos.size();
      for (UINT i = 0; i < count; i++)
      {
        VkDescriptorImageInfo ii{};
        ii.imageView = dummyImageView;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.sampler = dummySampler;
        if (srv)
        {
          auto *s = static_cast<SRVImpl *>(srv);
          if (s->v.vkView() != VK_NULL_HANDLE)
          {
            ii.imageView = s->v.vkView();
            CheckSampledLayout(s->v.texture);
          }
        }
        if (sampler) ii.sampler = static_cast<SamplerStateImpl *>(sampler)->sampler;
        imageInfos.push_back(ii);
      }
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = b.desc.binding;
      w.descriptorCount = count;
      w.descriptorType = b.desc.type;
      w.pImageInfo = &imageInfos[infoStart];
      writes.push_back(w);
      break;
    }
    case VK_DESCRIPTOR_TYPE_SAMPLER:
    {
      const uint32_t infoStart = (uint32_t)imageInfos.size();
      for (UINT i = 0; i < count; i++)
      {
        VkDescriptorImageInfo ii{};
        ii.sampler = sampler ? static_cast<SamplerStateImpl *>(sampler)->sampler : dummySampler;
        ii.imageView = VK_NULL_HANDLE;
        ii.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfos.push_back(ii);
      }
      VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = b.desc.binding;
      w.descriptorCount = count;
      w.descriptorType = b.desc.type;
      w.pImageInfo = &imageInfos[infoStart];
      writes.push_back(w);
      break;
    }
    default:
      phxvk::LogWarn("draw: descriptor type %d for binding %u is not handled", (int)b.desc.type, b.desc.binding);
      break;
    }
  }

  if (!writes.empty()) vkUpdateDescriptorSets(phxvk::Vk().device, (uint32_t)writes.size(), writes.data(), 0, nullptr);
  return true;
}

//////////////////////////////////////////////////////////////////////////////
// pipelines

// ===========================================================================
// geometry shader emulation (src/rhi/gs_emulation.h)
// ===========================================================================

// The topology the emulated stream is drawn with: whatever the original draw
// asked for is irrelevant, because the emitted vertices are one per Append.
static VkPrimitiveTopology EmulatedTopology(const EmulatedGeometryShader *plan)
{
  if (!plan) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  if (plan->program.outputStream == "PointStream") return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  if (plan->program.outputStream == "LineStream") return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
}

// The geometry shader the pipeline is about to drop, if one can be reproduced
// instead.  Null whenever there is nothing to emulate, which is the common case.
const EmulatedGeometryShader *ContextImpl::EmulationPlanFor(const ShaderCommon *gs) const
{
  if (!gs || gs->spirv.empty()) return nullptr;
  if (phxvk::Vk().features.geometryShader != VK_FALSE) return nullptr;
  static EmulatedGeometryShader plan;
  if (!LookupEmulatedGeometryShader(gs->spirv.data(), gs->spirv.size() * sizeof(uint32_t), &plan)) return nullptr;
  return &plan;
}

// Creates (once per geometry shader) the vertex stage that replaces it.
ID3D11VertexShader *ContextImpl::EmulatedVertexShaderFor(const ShaderCommon *gs)
{
  const EmulatedGeometryShader *plan = EmulationPlanFor(gs);
  if (!plan || !device) return nullptr;
  auto it = emulatedVertexShaders.find(gs->contentHash);
  if (it != emulatedVertexShaders.end()) return it->second;

  ID3D11VertexShader *vs = nullptr;
  const HRESULT hr = device->CreateVertexShader(plan->vertexSpirv.data(), plan->vertexSpirv.size() * sizeof(uint32_t), nullptr, &vs);
  if (FAILED(hr) || !vs)
  {
    phxvk::LogError("[d3d11] could not create the emulated vertex shader for a geometry shader");
    return nullptr;
  }
  emulatedVertexShaders[gs->contentHash] = vs;
  return vs;
}

bool ContextImpl::PrepareDraw(VkPipeline *outPipeline, VkPipelineLayout *outLayout, std::vector<MergedBinding> *outBindings)
{
  EnsureDummies();
  std::vector<MergedBinding> bindings = MergeBindings();
  if (outBindings) *outBindings = bindings;

  VkDescriptorSetLayout setLayout = GetSetLayout(bindings);
  if (setLayout == VK_NULL_HANDLE) return false;
  lastSetLayout = setLayout;
  VkPipelineLayout pipelineLayout = GetPipelineLayout(setLayout, bindings);
  if (pipelineLayout == VK_NULL_HANDLE) return false;
  *outLayout = pipelineLayout;

  PipelineKey key;
  // Content hashes, not handles: see PipelineKey.
  key.vs = stages[kStageVS].shader ? stages[kStageVS].shader->contentHash : 0;

  // A geometry shader this device cannot run, but can reproduce, replaces the
  // vertex stage and draws a triangle strip with primitive restart (the
  // emulated stage rebuilds the strips the geometry shader would have streamed).
  emulatedPipeline = false;
  emulationPlan = nullptr;
  if (stages[kStageGS].shader && phxvk::Vk().features.geometryShader == VK_FALSE)
  {
    if (ID3D11VertexShader *vs = EmulatedVertexShaderFor(stages[kStageGS].shader))
    {
      auto *impl = static_cast<VertexShaderImpl *>(vs);
      key.vs = impl->contentHash;
      key.emulated = true;
      key.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
      emulatedPipeline = true;
      emulationPlan = EmulationPlanFor(stages[kStageGS].shader);
    }
  }
  key.ps = stages[kStagePS].shader ? stages[kStagePS].shader->contentHash : 0;
  key.hs = stages[kStageHS].shader ? stages[kStageHS].shader->contentHash : 0;
  key.ds = stages[kStageDS].shader ? stages[kStageDS].shader->contentHash : 0;
  key.layout = inputLayout ? static_cast<InputLayoutImpl *>(inputLayout)->contentHash : 0;
  key.blend = blendState ? static_cast<BlendStateImpl *>(blendState)->contentHash : 0;
  key.depthStencil = depthStencilState ? static_cast<DepthStencilStateImpl *>(depthStencilState)->contentHash : 0;
  key.rasterizer = rasterizerState ? static_cast<RasterizerStateImpl *>(rasterizerState)->contentHash : 0;
  key.topology = topology;
  key.sampleMask = sampleMask;
  // rtv[] may contain nulls (the engine binds sparse target lists), and the loop below
  // already guards for that - this line did not, and dereferencing a null RTVImpl
  // crashed in PrepareDraw.
  {
    TextureImpl *firstRtv = rtvCount ? RtvTexture(rtv[0]) : nullptr;
    if (firstRtv) key.samples = DXGISampleCountToVk(firstRtv->sampleCount);
  }
  for (UINT i = 0; i < rtvCount && i < 8; i++)
  {
    auto *rv = static_cast<RTVImpl *>(rtv[i]);
    key.color[i] = (rv && rv->v.texture) ? rv->v.format : VK_FORMAT_UNDEFINED;
    key.colorCount = i + 1;
  }
  if (dsv)
  {
    auto *dv = static_cast<DSVImpl *>(dsv);
    key.depth = dv->v.format;
  }

  auto it = pipelines.find(key);
  if (it != pipelines.end())
  {
    *outPipeline = it->second;
    return it->second != VK_NULL_HANDLE;
  }

  if (key.vs == 0)
  {
    phxvk::LogError("draw: no vertex shader is bound");
    return false;
  }

  // The vertex stage that stands in for a geometry shader this device cannot
  // run, and the shape its fat input has to match.
  const EmulatedGeometryShader *emuPlan = nullptr;
  VertexShaderImpl *emuVs = nullptr;
  if (key.emulated && stages[kStageGS].shader)
  {
    emuPlan = EmulationPlanFor(stages[kStageGS].shader);
    if (emuPlan)
    {
      if (ID3D11VertexShader *vs = EmulatedVertexShaderFor(stages[kStageGS].shader))
        emuVs = static_cast<VertexShaderImpl *>(vs);
    }
    if (!emuPlan || !emuVs)
    {
      phxvk::LogError("draw: the geometry shader was marked emulated but its replacement is gone");
      return false;
    }
  }

  std::vector<VkPipelineShaderStageCreateInfo> stagesCi;
  auto addStage = [&](VkShaderStageFlagBits bit, int stageIndex) {
    const ShaderCommon *shader = stages[stageIndex].shader;
    if (emuVs && stageIndex == kStageVS) shader = emuVs;
    if (!shader || shader->module == VK_NULL_HANDLE) return;
    VkPipelineShaderStageCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage = bit;
    ci.module = shader->module;
    ci.pName = shader->entryPoint.c_str();
    stagesCi.push_back(ci);
  };
  addStage(VK_SHADER_STAGE_VERTEX_BIT, kStageVS);
  // MoltenVK/Metal has no geometry shader stage, so the stage is dropped and
  // the pipeline runs vertex -> fragment only.  This is NOT cosmetic: in
  // Clean Slate 5 of the 34 render techniques carry a geometry shader
  // ([PBR Mixed Rendering] and its vertex-offset variant, Default Particle,
  // PointSprites and Wireframe), and they use it to build geometry that the
  // vertex shader cannot express -- screen-space quads per particle and per
  // point sprite, the double-sided copy of a triangle, screen-space line
  // quads for the wireframe grid.  Dropping the stage therefore loses those
  // primitives outright (the wireframe collapses to one-pixel hairlines, the
  // two-sided pass disappears) rather than merely shading them differently.
  // Only a pipeline that has no replacement is actually missing its geometry.
  // key.emulated is set when the stage was reproduced instead, and printing
  // this for those pipelines made the port look like it was dropping geometry
  // it was in fact drawing.
  if (stages[kStageGS].shader && !key.emulated && phxvk::Vk().features.geometryShader == VK_FALSE)
  {
    // The shader-creation site owns OnceGsUnsupported(), so counting here needs
    // its own limiter -- otherwise the count this message exists to report
    // would never print at all.
    ++g_gsSkippedPipelines;
    static int logged = 0;
    if (logged < 8)
      phxvk::LogWarn("[d3d11] geometry shader stage DROPPED for pipeline #%u (MoltenVK/Metal has no geometry "
                     "shaders); any primitive that shader builds is missing. Affected techniques in Clean "
                     "Slate: [PBR Mixed Rendering] (double-sided triangles), [PBR Mixed Rendering with "
                     "vertex offset] (flat normals + double-sided), Default Particle and PointSprites "
                     "(screen-space quads), Wireframe (the background grid). See docs/STATUS.md.",
                     ++logged);
  }
  if (phxvk::Vk().features.geometryShader != VK_FALSE) addStage(VK_SHADER_STAGE_GEOMETRY_BIT, kStageGS);
  if (key.hs != 0 && key.ds != 0)
  {
    addStage(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, kStageHS);
    addStage(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, kStageDS);
  }
  addStage(VK_SHADER_STAGE_FRAGMENT_BIT, kStagePS);

  // Vertex input: location i <-> D3D11_INPUT_ELEMENT_DESC[i].
  //
  // D3D11 input layouts routinely declare more elements than the bound vertex
  // shader reads (the engine's own vxdecl2 has six while a technique reads
  // three), and D3D11 simply never fetches the unused ones.  Declaring them as
  // Vulkan attributes as well is legal but makes the validation layers warn
  // "Vertex attribute at location N not consumed by vertex shader" for every
  // such draw, so they are dropped when the shader reflection is available.
  // Nothing is dropped that the shader reads: a missing attribute would be a
  // VUID-VkGraphicsPipelineCreateInfo-Input-07904 error instead.
  std::vector<VkVertexInputBindingDescription> bindingDescs;
  std::vector<VkVertexInputAttributeDescription> attrDescs;
  // The key holds a hash now, so take the object from the parameter.
  InputLayoutImpl *il = inputLayout ? static_cast<InputLayoutImpl *>(inputLayout) : nullptr;
  const bool haveReflection = il && il->reflectionValid && !il->vertexReflection.inputs.empty();
  auto shaderReadsLocation = [&](UINT location) {
    if (!haveReflection) return true;
    for (auto &in : il->vertexReflection.inputs)
      if (in.location == location) return true;
    return false;
  };

  // An emulated geometry shader reads every vertex of its input primitive in
  // one vertex, declared as `struct { VSIN a0; VSIN a1; ... }`, so the shader's
  // input locations are the original element list repeated `reps` times in
  // order, with the offsets moved into each repetition's slab of the fat vertex.
  const UINT reps = (key.emulated && emuPlan) ? (UINT)emuPlan->program.inputVertices : 1;

  // Stride of one *original* vertex, derived exactly as the binding description
  // below does it, so the fat attribute offsets and the fat binding stride
  // agree with the buffer DrawInternal binds.
  UINT baseStride = 0;
  if (il)
  {
    baseStride = vertexStrides[0];
    if (!baseStride)
      for (UINT j = 0; j < il->elements.size(); j++)
      {
        if (il->elements[j].InputSlot != 0) continue;
        const UINT bpp = DXGIFormatBitsPerPixel(il->elements[j].Format);
        const UINT end = il->elements[j].AlignedByteOffset + (bpp ? bpp / 8 : 0);
        if (end > baseStride) baseStride = end;
      }
  }

  if (il)
  {
    for (UINT rep = 0; rep < reps; rep++)
    for (UINT i = 0; i < il->elements.size(); i++)
    {
      const D3D11_INPUT_ELEMENT_DESC &e = il->elements[i];
      // Every repetition is read by the emulated stage, whatever the original
      // vertex shader consumed.
      if (reps == 1 && !shaderReadsLocation(i)) continue;
      VkVertexInputAttributeDescription a{};
      a.location = rep * (UINT)il->elements.size() + i;
      a.binding = e.InputSlot;
      a.format = DXGIFormatToVk(e.Format);
      a.offset = e.AlignedByteOffset;
      if (a.format == VK_FORMAT_UNDEFINED && reps == 1)
      {
        // Fall back to the type the SPIR-V declares for this location.
        for (auto &in : il->vertexReflection.inputs)
          if (in.location == i && in.format != VK_FORMAT_UNDEFINED) a.format = in.format;
      }
      if (reps > 1)
      {
        // The repetition's slab of the fat vertex.  The slab size is the
        // stride the draw will bind, which is not known until then; the
        // draw-time stride and this offset are derived from the same value.
        a.offset = (UINT)((uint64_t)rep * baseStride + e.AlignedByteOffset);
      }
      if (a.format == VK_FORMAT_UNDEFINED)
      {
        phxvk::LogError("input element %u (semantic %s) has no usable format", i, e.SemanticName ? e.SemanticName : "?");
        continue;
      }
      attrDescs.push_back(a);
      if (e.InputSlot < 16)
      {
        bool seen = false;
        for (auto &bd : bindingDescs)
          if (bd.binding == e.InputSlot) seen = true;
        if (!seen)
        {
          VkVertexInputBindingDescription bd{};
          bd.binding = e.InputSlot;
          bd.stride = (e.InputSlot < 16 && vertexStrides[e.InputSlot]) ? vertexStrides[e.InputSlot] : 0;
          // The emulated stage reads `reps` original vertices per vertex, so
          // the binding it is fed is that many times as wide.
          if (reps > 1 && e.InputSlot == 0) bd.stride = baseStride * reps;
          bd.inputRate = e.InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
          if (bd.stride == 0)
          {
            // Derive the stride from the element array when the context has
            // no vertex buffer bound at pipeline creation time.
            UINT maxEnd = 0;
            for (UINT j = 0; j < il->elements.size(); j++)
            {
              if (il->elements[j].InputSlot != e.InputSlot) continue;
              const UINT bpp = DXGIFormatBitsPerPixel(il->elements[j].Format);
              const UINT end = il->elements[j].AlignedByteOffset + (bpp ? bpp / 8 : 0);
              if (end > maxEnd) maxEnd = end;
            }
            bd.stride = maxEnd;
          }
          bindingDescs.push_back(bd);
        }
      }
    }
  }

  VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertexInput.vertexBindingDescriptionCount = (uint32_t)bindingDescs.size();
  vertexInput.pVertexBindingDescriptions = bindingDescs.empty() ? nullptr : bindingDescs.data();
  vertexInput.vertexAttributeDescriptionCount = (uint32_t)attrDescs.size();
  vertexInput.pVertexAttributeDescriptions = attrDescs.empty() ? nullptr : attrDescs.data();

  VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  uint32_t patchControlPoints = 0;
  inputAssembly.topology = key.emulated ? EmulatedTopology(emuPlan) : ToVkTopology(key.topology, &patchControlPoints);
  // The emulated geometry is rebuilt from the index buffer.  Its restart index
  // separates the strips the geometry shader streamed, so restart has to be on
  // whenever separators were generated -- which is always for a stream that is
  // joined into strips, not only when the body called RestartStrip itself.
  inputAssembly.primitiveRestartEnable = (key.emulated && emuPlan && emuPlan->program.strips) ? VK_TRUE : VK_FALSE;
  if (key.emulated) patchControlPoints = 0;
  if (inputAssembly.topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST && patchControlPoints == 0) patchControlPoints = 1;

  VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  const D3D11_RASTERIZER_DESC &rs = rasterizerState ? rasterizerState->desc : kDefaultRasterizer;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.depthClampEnable = VK_FALSE;
  raster.rasterizerDiscardEnable = VK_FALSE;
  raster.polygonMode = ToVkPolygonMode(rs.FillMode);
  raster.cullMode = ToVkCullMode(rs.CullMode);
  // The negative-height viewport makes framebuffer coordinates match D3D's
  // (y down), so D3D's FrontCounterClockwise flag maps directly onto Vulkan's
  // frontFace.  Verified by tests/vk_d3d11_smoke.cpp, which draws a clockwise
  // (in D3D terms) quad with D3D11_CULL_BACK and FrontCounterClockwise = FALSE.
  raster.frontFace = rs.FrontCounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
  raster.depthBiasEnable = (rs.DepthBias != 0 || rs.SlopeScaledDepthBias != 0.0f) ? VK_TRUE : VK_FALSE;
  raster.depthBiasConstantFactor = (float)rs.DepthBias;
  raster.depthBiasClamp = rs.DepthBiasClamp;
  raster.depthBiasSlopeFactor = rs.SlopeScaledDepthBias;
  raster.lineWidth = 1.0f;
  if (rs.FillMode == D3D11_FILL_WIREFRAME && phxvk::Vk().features.fillModeNonSolid == VK_FALSE)
    phxvk::LogWarn("CreateRasterizerState: wireframe fill requested but fillModeNonSolid is not supported; drawing solid");

  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = key.samples;
  multisample.sampleShadingEnable = VK_FALSE;
  multisample.pSampleMask = &sampleMask;
  multisample.alphaToCoverageEnable = (blendState && blendState->desc.AlphaToCoverageEnable) ? VK_TRUE : VK_FALSE;

  const D3D11_DEPTH_STENCIL_DESC &dsDesc = depthStencilState ? depthStencilState->desc : kDefaultDepthStencil;
  VkPipelineDepthStencilStateCreateInfo depthStencilCi{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencilCi.depthTestEnable = (dsDesc.DepthEnable && key.depth != VK_FORMAT_UNDEFINED) ? VK_TRUE : VK_FALSE;
  depthStencilCi.depthWriteEnable = (dsDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL) ? VK_TRUE : VK_FALSE;
  depthStencilCi.depthCompareOp = ToVkCompareOp(dsDesc.DepthFunc);
  depthStencilCi.depthBoundsTestEnable = VK_FALSE;
  depthStencilCi.stencilTestEnable = dsDesc.StencilEnable ? VK_TRUE : VK_FALSE;
  depthStencilCi.front.failOp = ToVkStencilOp(dsDesc.FrontFace.StencilFailOp);
  depthStencilCi.front.passOp = ToVkStencilOp(dsDesc.FrontFace.StencilPassOp);
  depthStencilCi.front.depthFailOp = ToVkStencilOp(dsDesc.FrontFace.StencilDepthFailOp);
  depthStencilCi.front.compareOp = ToVkCompareOp(dsDesc.FrontFace.StencilFunc);
  depthStencilCi.front.compareMask = dsDesc.StencilReadMask;
  depthStencilCi.front.writeMask = dsDesc.StencilWriteMask;
  depthStencilCi.front.reference = 0;
  depthStencilCi.back = depthStencilCi.front;
  depthStencilCi.back.failOp = ToVkStencilOp(dsDesc.BackFace.StencilFailOp);
  depthStencilCi.back.passOp = ToVkStencilOp(dsDesc.BackFace.StencilPassOp);
  depthStencilCi.back.depthFailOp = ToVkStencilOp(dsDesc.BackFace.StencilDepthFailOp);
  depthStencilCi.back.compareOp = ToVkCompareOp(dsDesc.BackFace.StencilFunc);
  depthStencilCi.minDepthBounds = 0.0f;
  depthStencilCi.maxDepthBounds = 1.0f;

  const D3D11_BLEND_DESC &bd = blendState ? blendState->desc : kDefaultBlend;
  VkPipelineColorBlendAttachmentState colorAttachments[8]{};
  const UINT attachmentCount = key.colorCount ? key.colorCount : 1;
  for (UINT i = 0; i < attachmentCount; i++)
  {
    const D3D11_RENDER_TARGET_BLEND_DESC &rt = bd.RenderTarget[bd.IndependentBlendEnable ? i : 0];
    colorAttachments[i].blendEnable = rt.BlendEnable ? VK_TRUE : VK_FALSE;
    colorAttachments[i].srcColorBlendFactor = ToVkBlendFactor(rt.SrcBlend);
    colorAttachments[i].dstColorBlendFactor = ToVkBlendFactor(rt.DestBlend);
    colorAttachments[i].colorBlendOp = ToVkBlendOp(rt.BlendOp);
    colorAttachments[i].srcAlphaBlendFactor = ToVkBlendFactor(rt.SrcBlendAlpha);
    colorAttachments[i].dstAlphaBlendFactor = ToVkBlendFactor(rt.DestBlendAlpha);
    colorAttachments[i].alphaBlendOp = ToVkBlendOp(rt.BlendOpAlpha);
    colorAttachments[i].colorWriteMask = rt.RenderTargetWriteMask;
  }

  VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  colorBlend.logicOpEnable = VK_FALSE;
  colorBlend.attachmentCount = attachmentCount;
  colorBlend.pAttachments = colorAttachments;

  const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = (uint32_t)(sizeof(dynamicStates) / sizeof(dynamicStates[0]));
  dynamic.pDynamicStates = dynamicStates;

  VkPipelineTessellationStateCreateInfo tessellation{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
  tessellation.patchControlPoints = patchControlPoints;

  VkPipelineRenderingCreateInfoKHR renderingInfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  renderingInfo.colorAttachmentCount = key.colorCount;
  renderingInfo.pColorAttachmentFormats = key.colorCount ? key.color : nullptr;
  renderingInfo.depthAttachmentFormat = key.depth;
  renderingInfo.stencilAttachmentFormat = (key.depth != VK_FORMAT_UNDEFINED && DXGIFormatIsStencil(static_cast<DSVImpl *>(dsv)->desc.Format)) ? key.depth : VK_FORMAT_UNDEFINED;

  VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &renderingInfo;
  ci.stageCount = (uint32_t)stagesCi.size();
  ci.pStages = stagesCi.data();
  ci.pVertexInputState = &vertexInput;
  ci.pInputAssemblyState = &inputAssembly;
  ci.pViewportState = &viewportState;
  ci.pRasterizationState = &raster;
  ci.pMultisampleState = &multisample;
  ci.pDepthStencilState = &depthStencilCi;
  ci.pColorBlendState = &colorBlend;
  ci.pDynamicState = &dynamic;
  ci.pTessellationState = (patchControlPoints > 0) ? &tessellation : nullptr;
  ci.layout = pipelineLayout;
  ci.renderPass = VK_NULL_HANDLE;

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult res = vkCreateGraphicsPipelines(phxvk::Vk().device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkCreateGraphicsPipelines failed: %s (topology %d, %u RTs, depth %d)", phxvk::VkResultName(res), (int)key.topology, key.colorCount, (int)key.depth);
    pipelines[key] = VK_NULL_HANDLE;
    return false;
  }
  pipelines[key] = pipeline;
  *outPipeline = pipeline;
  return true;
}

//////////////////////////////////////////////////////////////////////////////
// state setting

void ContextImpl::ResetState()
{
  // Every table holds a reference (see BindRef); releasing it here is what
  // makes ClearState() drop the shim's references the way D3D11 does.
  BindShader<ID3D11VertexShader, VertexShaderImpl>(stages[kStageVS].shader, (ID3D11VertexShader *)nullptr);
  BindShader<ID3D11PixelShader, PixelShaderImpl>(stages[kStagePS].shader, (ID3D11PixelShader *)nullptr);
  BindShader<ID3D11GeometryShader, GeometryShaderImpl>(stages[kStageGS].shader, (ID3D11GeometryShader *)nullptr);
  BindShader<ID3D11HullShader, HullShaderImpl>(stages[kStageHS].shader, (ID3D11HullShader *)nullptr);
  BindShader<ID3D11DomainShader, DomainShaderImpl>(stages[kStageDS].shader, (ID3D11DomainShader *)nullptr);
  BindShader<ID3D11ComputeShader, ComputeShaderImpl>(stages[kStageCS].shader, (ID3D11ComputeShader *)nullptr);
  for (int s = 0; s < kStageCount; s++)
  {
    SetTable(stages[s].cb, 0, kMaxConstantBuffers, (ID3D11Buffer *const *)nullptr);
    SetTable(stages[s].srv, 0, kMaxShaderResources, (ID3D11ShaderResourceView *const *)nullptr);
    SetTable(stages[s].samplers, 0, kMaxSamplers, (ID3D11SamplerState *const *)nullptr);
    SetTable(stages[s].uav, 0, kMaxUAVs, (ID3D11UnorderedAccessView *const *)nullptr);
  }
  for (UINT i = 0; i < kMaxRenderTargets; i++) BindRef(rtv[i], (ID3D11RenderTargetView *)nullptr);
  rtvCount = 0;
  BindRef(dsv, (ID3D11DepthStencilView *)nullptr);
  for (UINT i = 0; i < kMaxUAVs; i++) BindRef(omUav[i], (ID3D11UnorderedAccessView *)nullptr);
  omUavStart = omUavCount = 0;
  BindRef(inputLayout, (ID3D11InputLayout *)nullptr);
  for (int i = 0; i < 16; i++)
  {
    BindRef(vertexBuffers[i], (ID3D11Buffer *)nullptr);
    vertexStrides[i] = 0;
    vertexOffsets[i] = 0;
  }
  vertexBufferCount = 0;
  BindRef(indexBuffer, (ID3D11Buffer *)nullptr);
  indexFormat = DXGI_FORMAT_R16_UINT;
  indexOffset = 0;
  topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  viewportSet = false;
  scissorSet = false;
  BindRef(blendState, (BlendStateImpl *)nullptr);
  blendFactor[0] = blendFactor[1] = blendFactor[2] = blendFactor[3] = 1.0f;
  sampleMask = 0xffffffff;
  BindRef(depthStencilState, (DepthStencilStateImpl *)nullptr);
  stencilRef = 0;
  BindRef(rasterizerState, (RasterizerStateImpl *)nullptr);
  BindRef(predication, (ID3D11Predicate *)nullptr);
  predicationValue = FALSE;
}

void ContextImpl::ClearState(void)
{
  EndRendering();
  ResetState();
  phxvk::Device &d = phxvk::Vk();
  if (d.frameActive)
  {
    phxvk::FrameEndAndSubmit();
    vkWaitForFences(d.device, 1, &d.frameFence, VK_TRUE, UINT64_MAX);
    phxvk::FrameWaitIdle();
  }
}

void ContextImpl::Flush(void)
{
  // D3D11's Flush submits whatever is pending; the shim's frame boundary is
  // Present(), so flush what we have and wait for it.
  phxvk::Device &d = phxvk::Vk();
  if (!d.frameActive) return;
  SubmitAndWait();
}

void ContextImpl::IASetInputLayout(ID3D11InputLayout *pInputLayout) { BindRef(inputLayout, pInputLayout); }

void ContextImpl::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppVertexBuffers, const UINT *pStrides, const UINT *pOffsets)
{
  if (StartSlot >= 16) return;
  if (NumBuffers > 16 - StartSlot) NumBuffers = 16 - StartSlot;
  for (UINT i = 0; i < NumBuffers; i++)
  {
    BindRef(vertexBuffers[StartSlot + i], ppVertexBuffers ? ppVertexBuffers[i] : nullptr);
    // The engine passes NULL for both strides and offsets when unbinding.
    vertexStrides[StartSlot + i] = pStrides ? pStrides[i] : 0;
    vertexOffsets[StartSlot + i] = pOffsets ? pOffsets[i] : 0;
  }
  UINT highest = 0;
  for (UINT i = 0; i < 16; i++)
    if (vertexBuffers[i]) highest = i + 1;
  vertexBufferCount = highest;
}

void ContextImpl::IASetIndexBuffer(ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset)
{
  BindRef(indexBuffer, pIndexBuffer);
  indexFormat = (Format == DXGI_FORMAT_R32_UINT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
  indexOffset = Offset;
}

void ContextImpl::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) { topology = Topology; }
void ContextImpl::VSSetShader(ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *, UINT)
{
  BindShader<ID3D11VertexShader, VertexShaderImpl>(stages[kStageVS].shader, pVertexShader);
}
void ContextImpl::PSSetShader(ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *, UINT)
{
  BindShader<ID3D11PixelShader, PixelShaderImpl>(stages[kStagePS].shader, pPixelShader);
}
void ContextImpl::GSSetShader(ID3D11GeometryShader *pShader, ID3D11ClassInstance *const *, UINT)
{
  BindShader<ID3D11GeometryShader, GeometryShaderImpl>(stages[kStageGS].shader, pShader);
}
void ContextImpl::HSSetShader(ID3D11HullShader *pShader, ID3D11ClassInstance *const *, UINT)
{
  BindShader<ID3D11HullShader, HullShaderImpl>(stages[kStageHS].shader, pShader);
}
void ContextImpl::DSSetShader(ID3D11DomainShader *pShader, ID3D11ClassInstance *const *, UINT)
{
  BindShader<ID3D11DomainShader, DomainShaderImpl>(stages[kStageDS].shader, pShader);
}
void ContextImpl::CSSetShader(ID3D11ComputeShader *pShader, ID3D11ClassInstance *const *, UINT)
{
  BindShader<ID3D11ComputeShader, ComputeShaderImpl>(stages[kStageCS].shader, pShader);
}

void ContextImpl::VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers)
{
  SetTable(stages[kStageVS].cb, StartSlot, NumBuffers, ppConstantBuffers);
}
void ContextImpl::PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers)
{
  SetTable(stages[kStagePS].cb, StartSlot, NumBuffers, ppConstantBuffers);
}
void ContextImpl::GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers)
{
  SetTable(stages[kStageGS].cb, StartSlot, NumBuffers, ppConstantBuffers);
}
void ContextImpl::HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers)
{
  SetTable(stages[kStageHS].cb, StartSlot, NumBuffers, ppConstantBuffers);
}
void ContextImpl::DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers)
{
  SetTable(stages[kStageDS].cb, StartSlot, NumBuffers, ppConstantBuffers);
}
void ContextImpl::CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers)
{
  SetTable(stages[kStageCS].cb, StartSlot, NumBuffers, ppConstantBuffers);
}

void ContextImpl::VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
  SetTable(stages[kStageVS].srv, StartSlot, NumViews, ppShaderResourceViews);
}
void ContextImpl::PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
  SetTable(stages[kStagePS].srv, StartSlot, NumViews, ppShaderResourceViews);
}
void ContextImpl::GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
  SetTable(stages[kStageGS].srv, StartSlot, NumViews, ppShaderResourceViews);
}
void ContextImpl::HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
  SetTable(stages[kStageHS].srv, StartSlot, NumViews, ppShaderResourceViews);
}
void ContextImpl::DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
  SetTable(stages[kStageDS].srv, StartSlot, NumViews, ppShaderResourceViews);
}
void ContextImpl::CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews)
{
  SetTable(stages[kStageCS].srv, StartSlot, NumViews, ppShaderResourceViews);
}

void ContextImpl::VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers)
{
  SetTable(stages[kStageVS].samplers, StartSlot, NumSamplers, ppSamplers);
}
void ContextImpl::PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers)
{
  SetTable(stages[kStagePS].samplers, StartSlot, NumSamplers, ppSamplers);
}
void ContextImpl::GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers)
{
  SetTable(stages[kStageGS].samplers, StartSlot, NumSamplers, ppSamplers);
}
void ContextImpl::HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers)
{
  SetTable(stages[kStageHS].samplers, StartSlot, NumSamplers, ppSamplers);
}
void ContextImpl::DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers)
{
  SetTable(stages[kStageDS].samplers, StartSlot, NumSamplers, ppSamplers);
}
void ContextImpl::CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers)
{
  SetTable(stages[kStageCS].samplers, StartSlot, NumSamplers, ppSamplers);
}

void ContextImpl::VSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *)
{
  SetTable(stages[kStageVS].uav, StartSlot, NumUAVs, ppUnorderedAccessViews);
}
void ContextImpl::PSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *)
{
  SetTable(stages[kStagePS].uav, StartSlot, NumUAVs, ppUnorderedAccessViews);
}
void ContextImpl::CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *)
{
  SetTable(stages[kStageCS].uav, StartSlot, NumUAVs, ppUnorderedAccessViews);
}

void ContextImpl::SOSetTargets(UINT, ID3D11Buffer *const *, const UINT *)
{
  // Stream output is not used by the engine; make the omission loud.
  static Once once;
  if (once()) phxvk::LogWarn("[d3d11] SOSetTargets is not implemented (stream output is unused by the apEx engine)");
}

void ContextImpl::OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView)
{
  bool changed = false;
  if (NumViews != rtvCount) changed = true;
  for (UINT i = 0; i < NumViews && i < kMaxRenderTargets; i++)
    if (rtv[i] != (ppRenderTargetViews ? ppRenderTargetViews[i] : nullptr)) changed = true;
  if (dsv != pDepthStencilView) changed = true;
  if (!changed) return;

  EndRendering();
  EndRendering(); // no-op, kept for symmetry
  rtvCount = NumViews > kMaxRenderTargets ? kMaxRenderTargets : NumViews;
  for (UINT i = 0; i < kMaxRenderTargets; i++) BindRef(rtv[i], (i < rtvCount && ppRenderTargetViews) ? ppRenderTargetViews[i] : nullptr);
  BindRef(dsv, pDepthStencilView);
  omUavCount = 0;
  omUavStart = 0;
}

void ContextImpl::OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView, UINT UAVStartSlot,
                                                           UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *)
{
  // 0xffffffff means "leave the current render targets alone".
  if (NumRTVs != 0xffffffff)
  {
    EndRendering();
    rtvCount = NumRTVs > kMaxRenderTargets ? kMaxRenderTargets : NumRTVs;
    for (UINT i = 0; i < kMaxRenderTargets; i++) BindRef(rtv[i], (i < rtvCount && ppRenderTargetViews) ? ppRenderTargetViews[i] : nullptr);
    BindRef(dsv, pDepthStencilView);
  }
  omUavStart = UAVStartSlot;
  omUavCount = NumUAVs > kMaxUAVs ? kMaxUAVs : NumUAVs;
  for (UINT i = 0; i < kMaxUAVs; i++) BindRef(omUav[i], (i < omUavCount && ppUnorderedAccessViews) ? ppUnorderedAccessViews[i] : nullptr);
}

void ContextImpl::OMSetBlendState(ID3D11BlendState *pBlendState, const FLOAT BlendFactor[4], UINT SampleMask)
{
  BindRef(blendState, pBlendState ? static_cast<BlendStateImpl *>(pBlendState) : nullptr);
  if (BlendFactor)
    for (int i = 0; i < 4; i++) blendFactor[i] = BlendFactor[i];
  sampleMask = SampleMask;
  if (renderingActive) vkCmdSetBlendConstants(phxvk::FrameCommandBuffer(), blendFactor);
}

void ContextImpl::OMSetDepthStencilState(ID3D11DepthStencilState *pDepthStencilState, UINT StencilRef)
{
  BindRef(depthStencilState, pDepthStencilState ? static_cast<DepthStencilStateImpl *>(pDepthStencilState) : nullptr);
  stencilRef = StencilRef;
  if (renderingActive) vkCmdSetStencilReference(phxvk::FrameCommandBuffer(), VK_STENCIL_FACE_FRONT_AND_BACK, stencilRef);
}

void ContextImpl::RSSetState(ID3D11RasterizerState *pRasterizerState)
{
  BindRef(rasterizerState, pRasterizerState ? static_cast<RasterizerStateImpl *>(pRasterizerState) : nullptr);
}

// State getters.
//
// The engine never reads its own state back, so these existed only as an
// unimplemented corner of the API until Dear ImGui's DX11 backend needed them:
// it saves and restores the pipeline state around its own drawing, so a UI can
// be drawn in the middle of a frame without disturbing the caller.
// The per-stage getters the same backend needs.  Each returns the object the
// setter recorded, with the reference D3D11 promises the caller.
namespace
{
inline ID3D11VertexShader *AsVertexShader(ShaderCommon *s)
{
  return s ? static_cast<ID3D11VertexShader *>(static_cast<VertexShaderImpl *>(s)) : nullptr;
}
inline ID3D11PixelShader *AsPixelShader(ShaderCommon *s)
{
  return s ? static_cast<ID3D11PixelShader *>(static_cast<PixelShaderImpl *>(s)) : nullptr;
}
inline ID3D11GeometryShader *AsGeometryShader(ShaderCommon *s)
{
  return s ? static_cast<ID3D11GeometryShader *>(static_cast<GeometryShaderImpl *>(s)) : nullptr;
}
} // namespace

void ContextImpl::IAGetInputLayout(ID3D11InputLayout **ppInputLayout)
{
  if (!ppInputLayout) return;
  *ppInputLayout = inputLayout;
  if (inputLayout) static_cast<InputLayoutImpl *>(inputLayout)->AddRefImpl();
}

void ContextImpl::IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppVertexBuffers, UINT *pStrides, UINT *pOffsets)
{
  for (UINT i = 0; i < NumBuffers; i++)
  {
    const UINT slot = StartSlot + i;
    if (slot >= 16) break;
    if (ppVertexBuffers)
    {
      ppVertexBuffers[i] = vertexBuffers[slot];
      if (vertexBuffers[slot]) static_cast<BufferImpl *>(vertexBuffers[slot])->AddRefImpl();
    }
    if (pStrides) pStrides[i] = vertexStrides[slot];
    if (pOffsets) pOffsets[i] = vertexOffsets[slot];
  }
}

void ContextImpl::IAGetIndexBuffer(ID3D11Buffer **pIndexBuffer, DXGI_FORMAT *Format, UINT *Offset)
{
  if (pIndexBuffer)
  {
    *pIndexBuffer = indexBuffer;
    if (indexBuffer) static_cast<BufferImpl *>(indexBuffer)->AddRefImpl();
  }
  if (Format) *Format = indexFormat;
  if (Offset) *Offset = indexOffset;
}

void ContextImpl::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY *pTopology)
{
  if (pTopology) *pTopology = topology;
}

void ContextImpl::VSGetShader(ID3D11VertexShader **ppVertexShader, ID3D11ClassInstance *const *, UINT *pNumClassInstances)
{
  if (pNumClassInstances) *pNumClassInstances = 0;
  if (!ppVertexShader) return;
  *ppVertexShader = AsVertexShader(stages[kStageVS].shader);
  if (*ppVertexShader) static_cast<VertexShaderImpl *>(stages[kStageVS].shader)->AddRefImpl();
}

void ContextImpl::PSGetShader(ID3D11PixelShader **ppPixelShader, ID3D11ClassInstance *const *, UINT *pNumClassInstances)
{
  if (pNumClassInstances) *pNumClassInstances = 0;
  if (!ppPixelShader) return;
  *ppPixelShader = AsPixelShader(stages[kStagePS].shader);
  if (*ppPixelShader) static_cast<PixelShaderImpl *>(stages[kStagePS].shader)->AddRefImpl();
}

void ContextImpl::GSGetShader(ID3D11GeometryShader **ppGeometryShader, ID3D11ClassInstance *const *, UINT *pNumClassInstances)
{
  if (pNumClassInstances) *pNumClassInstances = 0;
  if (!ppGeometryShader) return;
  *ppGeometryShader = AsGeometryShader(stages[kStageGS].shader);
  if (*ppGeometryShader) static_cast<GeometryShaderImpl *>(stages[kStageGS].shader)->AddRefImpl();
}

void ContextImpl::VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers)
{
  if (!ppConstantBuffers) return;
  for (UINT i = 0; i < NumBuffers; i++)
  {
    const UINT slot = StartSlot + i;
    ID3D11Buffer *b = slot < kMaxConstantBuffers ? stages[kStageVS].cb[slot] : nullptr;
    ppConstantBuffers[i] = b;
    if (b) static_cast<BufferImpl *>(b)->AddRefImpl();
  }
}

void ContextImpl::PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews)
{
  if (!ppShaderResourceViews) return;
  for (UINT i = 0; i < NumViews; i++)
  {
    const UINT slot = StartSlot + i;
    ID3D11ShaderResourceView *v = slot < kMaxShaderResources ? stages[kStagePS].srv[slot] : nullptr;
    ppShaderResourceViews[i] = v;
    if (v) static_cast<SRVImpl *>(v)->AddRefImpl();
  }
}

void ContextImpl::PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers)
{
  if (!ppSamplers) return;
  for (UINT i = 0; i < NumSamplers; i++)
  {
    const UINT slot = StartSlot + i;
    ID3D11SamplerState *smp = slot < kMaxSamplers ? stages[kStagePS].samplers[slot] : nullptr;
    ppSamplers[i] = smp;
    if (smp) static_cast<SamplerStateImpl *>(smp)->AddRefImpl();
  }
}

void ContextImpl::RSGetViewports(UINT *pNumViewports, D3D11_VIEWPORT *pViewports)
{
  if (pNumViewports) *pNumViewports = viewportSet ? 1 : 0;
  if (pViewports && viewportSet) pViewports[0] = viewport;
}

void ContextImpl::RSGetScissorRects(UINT *pNumRects, D3D11_RECT *pRects)
{
  if (pNumRects) *pNumRects = scissorSet ? 1 : 0;
  if (pRects && scissorSet) pRects[0] = scissor;
}

void ContextImpl::RSGetState(ID3D11RasterizerState **ppRasterizerState)
{
  if (!ppRasterizerState) return;
  *ppRasterizerState = rasterizerState;
  if (rasterizerState) rasterizerState->AddRefImpl();
}

void ContextImpl::OMGetBlendState(ID3D11BlendState **ppBlendState, FLOAT BlendFactor[4], UINT *pSampleMask)
{
  if (ppBlendState)
  {
    *ppBlendState = blendState;
    if (blendState) blendState->AddRefImpl();
  }
  if (BlendFactor)
    for (int i = 0; i < 4; i++) BlendFactor[i] = blendFactor[i];
  if (pSampleMask) *pSampleMask = sampleMask;
}

void ContextImpl::OMGetDepthStencilState(ID3D11DepthStencilState **ppDepthStencilState, UINT *pStencilRef)
{
  if (ppDepthStencilState)
  {
    *ppDepthStencilState = depthStencilState;
    if (depthStencilState) depthStencilState->AddRefImpl();
  }
  if (pStencilRef) *pStencilRef = stencilRef;
}

void ContextImpl::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT *pViewports)
{
  if (!NumViewports || !pViewports) return;
  viewport = pViewports[0];
  viewportSet = true;
  if (renderingActive) ApplyGraphicsState(phxvk::FrameCommandBuffer());
}

void ContextImpl::RSSetScissorRects(UINT NumRects, const D3D11_RECT *pRects)
{
  if (!NumRects || !pRects)
  {
    scissorSet = false;
    if (renderingActive) ApplyGraphicsState(phxvk::FrameCommandBuffer());
    return;
  }
  scissor = pRects[0];
  scissorSet = true;
  if (renderingActive) ApplyGraphicsState(phxvk::FrameCommandBuffer());
}

//////////////////////////////////////////////////////////////////////////////
// draws

bool ContextImpl::DrawInternal(bool indexed, UINT count, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startVertex, UINT startInstance)
{
  if (!rtvCount)
  {
    static Once once;
    if (once()) phxvk::LogWarn("[d3d11] draw issued with no render target bound; ignoring");
    return false;
  }
  if (count == 0) return true;

  BeginFrameIfNeeded();

  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::vector<MergedBinding> bindings;
  if (!PrepareDraw(&pipeline, &layout, &bindings)) return false;
  if (pipeline == VK_NULL_HANDLE) return false;

  // Every image barrier has to be recorded *before* the render pass instance
  // starts (VUID-vkCmdPipelineBarrier-None-09553): a sampled image that was a
  // render target one draw ago still has to move to SHADER_READ_ONLY_OPTIMAL.
  TransitionBindings(bindings);
  EnsureRenderingStarted();

  // The pipeline may have been created before the render pass started (it was
  // not, but PrepareDraw can also be reached from a clear), so make sure the
  // bake-free dynamic state is current.
  ApplyGraphicsState(phxvk::FrameCommandBuffer());

  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  // ---------------------------------------------------------------------
  // Geometry shader emulation.  The draw's own geometry is replaced by the fat
  // expansion the emulated vertex stage reads: one vertex per emitted vertex,
  // each carrying every vertex of the input primitive, indexed as the triangle
  // strip the geometry shader would have streamed.  See gs_emulation.h.
  // ---------------------------------------------------------------------
  VkBuffer emuVb = VK_NULL_HANDLE;
  VkDeviceSize emuVbOffset = 0;
  uint32_t emuVbStride = 0;
  VkBuffer emuIb = VK_NULL_HANDLE;
  VkDeviceSize emuIbOffset = 0;
  uint32_t emuIndexCount = 0;
  if (emulatedPipeline && emulationPlan)
  {
    auto *vb = vertexBuffers[0] ? static_cast<BufferImpl *>(vertexBuffers[0]) : nullptr;
    auto *ib = (indexed && indexBuffer) ? static_cast<BufferImpl *>(indexBuffer) : nullptr;
    // A non-indexed draw (the engine's particle path) has no index buffer at
    // all; its vertices are taken sequentially.
    if (!vb || vb->shadow.empty() || vertexStrides[0] == 0 || (indexed && (!ib || ib->shadow.empty())))
    {
      static Once once;
      if (once())
        phxvk::LogWarn("[d3d11] cannot emulate the geometry shader for a draw without readable vertex data");
      return false;
    }
    if (baseVertex != 0 || instanceCount > 1)
    {
      static Once once;
      if (once())
        phxvk::LogWarn("[d3d11] geometry shader emulation ignores a non-zero base vertex or instancing");
      return false;
    }

    phxvk::FatGeometry fat;
    std::string err;
    const void *indexData = ib ? ib->shadow.data() : nullptr;
    const size_t firstIndex = indexed ? startIndex : 0;
    if (!phxvk::BuildFatGeometry(emulationPlan->program, vb->shadow.data(), vb->shadow.size(), vertexStrides[0],
                                 indexFormat == DXGI_FORMAT_R32_UINT, indexData, count, firstIndex, fat, &err))
    {
      static Once once;
      if (once()) phxvk::LogWarn("[d3d11] geometry shader emulation could not build the fat geometry: %s", err.c_str());
      return false;
    }

    // Built once per distinct draw: the source buffers are load-time meshes and
    // do not change, and the expansion is orders of magnitude too large to
    // rebuild into the per-frame ring on every draw (it exhausted it).
    const uint64_t cacheKey = PhxHashCombine(
        PhxHashCombine(stages[kStageGS].shader->contentHash, (uint64_t)(uintptr_t)vb),
        PhxHashCombine((uint64_t)(uintptr_t)ib, ((uint64_t)count << 32) ^ (uint64_t)startIndex));
    auto cached = emulatedGeometryCache.find(cacheKey);
    if (cached == emulatedGeometryCache.end())
    {
      EmulatedGeometry entry;
      entry.vbo = phxvk::CreateBuffer(fat.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &entry.vboMemory);
      entry.ibo = phxvk::CreateBuffer(fat.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &entry.iboMemory);
      if (!entry.vbo || !entry.ibo)
      {
        static Once once;
        if (once()) phxvk::LogWarn("[d3d11] geometry shader emulation could not allocate its buffers");
        return false;
      }
      void *vmap = MapStagingMemoryRange(entry.vboMemory, 0, fat.vertices.size());
      void *imap = MapStagingMemoryRange(entry.iboMemory, 0, fat.indices.size() * sizeof(uint32_t));
      if (!vmap || !imap)
      {
        static Once once;
        if (once()) phxvk::LogWarn("[d3d11] geometry shader emulation could not map its buffers");
        return false;
      }
      memcpy(vmap, fat.vertices.data(), fat.vertices.size());
      memcpy(imap, fat.indices.data(), fat.indices.size() * sizeof(uint32_t));
      vkUnmapMemory(phxvk::Vk().device, entry.vboMemory);
      vkUnmapMemory(phxvk::Vk().device, entry.iboMemory);
      entry.indexCount = (uint32_t)fat.indices.size();
      entry.vertexStride = (uint32_t)(vertexStrides[0] * (uint32_t)emulationPlan->program.inputVertices);
      cached = emulatedGeometryCache.emplace(cacheKey, entry).first;
    }
    const EmulatedGeometry &geo = cached->second;
    emuVb = geo.vbo;
    emuIb = geo.ibo;
    emuIndexCount = geo.indexCount;
    emuVbStride = geo.vertexStride;

    static int logged = 0;
    if (logged++ < 4)
      phxvk::LogWarn("[d3d11] geometry shader emulated at draw: %u -> %u vertices, %u indices (%s in x%d)",
                     count, fat.vertexCount, emuIndexCount, emulationPlan->program.inputVertices == 2 ? "line" : "prim",
                     emulationPlan->program.inputVertices);
  }

  VkDescriptorSet set = AllocateDescriptorSet(lastSetLayout);
  if (set == VK_NULL_HANDLE) return false;
  std::vector<uint32_t> dynamicOffsets;
  if (!WriteDescriptorSet(set, bindings, dynamicOffsets)) return false;
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, (uint32_t)dynamicOffsets.size(), dynamicOffsets.empty() ? nullptr : dynamicOffsets.data());

  // Vertex buffers: only the slots the pipeline actually consumes matter, but
  // binding all of them keeps dynamic vertex input simple.
  for (UINT i = 0; i < 16; i++)
  {
    if (emuVb != VK_NULL_HANDLE)
    {
      // The fat buffer replaces every slot of the original geometry.
      if (i == 0) vkCmdBindVertexBuffers(cmd, 0, 1, &emuVb, &emuVbOffset);
      continue;
    }
    if (!vertexBuffers[i]) continue;
    auto *buf = static_cast<BufferImpl *>(vertexBuffers[i]);
    VkBuffer vkBuf = buf->boundBuffer();
    if (vkBuf == VK_NULL_HANDLE) continue;
    VkDeviceSize offset = buf->boundOffset() + vertexOffsets[i];
    vkCmdBindVertexBuffers(cmd, i, 1, &vkBuf, &offset);
  }

  if (emuIb != VK_NULL_HANDLE)
  {
    // Emulated geometry is always drawn indexed: the strip layout and the
    // primitive restarts that separate the geometry shader's strips live in the
    // generated index buffer, even when the original draw had none.
    vkCmdBindIndexBuffer(cmd, emuIb, emuIbOffset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, emuIndexCount, 1, 0, 0, 0);
    return true;
  }
  if (indexed)
  {
    if (!indexBuffer)
    {
      static Once once;
      if (once()) phxvk::LogWarn("[d3d11] DrawIndexed with no index buffer bound; ignoring");
      return false;
    }
    auto *buf = static_cast<BufferImpl *>(indexBuffer);
    VkBuffer vkBuf = buf->boundBuffer();
    if (vkBuf == VK_NULL_HANDLE) return false;
    const VkIndexType type = (indexFormat == DXGI_FORMAT_R32_UINT) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    vkCmdBindIndexBuffer(cmd, vkBuf, buf->boundOffset() + indexOffset, type);
    vkCmdDrawIndexed(cmd, count, instanceCount ? instanceCount : 1, startIndex, baseVertex, startInstance);
  }
  else
  {
    vkCmdDraw(cmd, count, instanceCount ? instanceCount : 1, startVertex, startInstance);
  }
  return true;
}

void ContextImpl::Draw(UINT VertexCount, UINT StartVertexLocation) { DrawInternal(false, VertexCount, 1, 0, 0, StartVertexLocation, 0); }
void ContextImpl::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
#ifdef APEX_TRACE_UI_DRAWS
  {
    static int n = 0;
    if (++n <= 12 || (n % 200) == 0)
      phxvk::LogInfo("[trace] DrawIndexed #%d: %u indices, vs=%p ps=%p", n, IndexCount,
                     (void *)stages[kStageVS].shader, (void *)stages[kStagePS].shader);
  }
#endif
  DrawInternal(true, IndexCount, 1, StartIndexLocation, BaseVertexLocation, 0, 0);
}
void ContextImpl::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
  DrawInternal(false, VertexCountPerInstance, InstanceCount, 0, 0, StartVertexLocation, StartInstanceLocation);
}
void ContextImpl::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
  DrawInternal(true, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, 0, StartInstanceLocation);
}
void ContextImpl::DrawAuto(void)
{
  static Once once;
  if (once()) phxvk::LogWarn("[d3d11] DrawAuto is not supported (stream output is unused by the engine)");
}

void ContextImpl::Dispatch(UINT, UINT, UINT)
{
  static Once once;
  if (once()) phxvk::LogWarn("[d3d11] compute Dispatch is not implemented in this port (PHX_VOLUMETRIC_RENDERTARGETS is off in PhoenixConfig.h)");
}

//////////////////////////////////////////////////////////////////////////////
// clears

void ContextImpl::ClearRenderTargetView(ID3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4])
{
  if (!pRenderTargetView) return;
  auto *view = static_cast<RTVImpl *>(pRenderTargetView);
  TextureImpl *tex = view->v.texture;
  if (!tex) return;

  VkClearColorValue color{};
  for (int i = 0; i < 4; i++) color.float32[i] = ColorRGBA ? ColorRGBA[i] : 0.0f;

  // If this is one of the currently bound targets and a render pass instance is
  // open, clear via the attachment (cheaper and layout-correct).
  bool bound = false;
  for (UINT i = 0; i < rtvCount; i++)
    if (rtv[i] == pRenderTargetView) bound = true;

  if (bound)
  {
    EnsureRenderingStarted();
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    for (UINT i = 0; i < rtvCount; i++)
      if (rtv[i] == pRenderTargetView) attachment.colorAttachment = i;
    attachment.clearValue.color = color;
    VkClearRect rect{};
    rect.rect = {{0, 0}, TextureExtent(tex)};
    rect.layerCount = 1;
    vkCmdClearAttachments(phxvk::FrameCommandBuffer(), 1, &attachment, 1, &rect);
    return;
  }

  EndRendering();
  BeginFrameIfNeeded();
  TransitionImage(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, tex->mipLevels ? tex->mipLevels : 1, 0, tex->arraySize ? tex->arraySize : 1};
  vkCmdClearColorImage(phxvk::FrameCommandBuffer(), tex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
}

void ContextImpl::ClearDepthStencilView(ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
  if (!pDepthStencilView) return;
  auto *view = static_cast<DSVImpl *>(pDepthStencilView);
  TextureImpl *tex = view->v.texture;
  if (!tex) return;

  VkClearDepthStencilValue value{};
  value.depth = Depth;
  value.stencil = Stencil;
  VkImageAspectFlags clearAspect = 0;
  if (ClearFlags & D3D11_CLEAR_DEPTH) clearAspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
  if (ClearFlags & D3D11_CLEAR_STENCIL) clearAspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
  if (!clearAspect) return;

  bool bound = (dsv == pDepthStencilView);
  if (bound && renderingActive)
  {
    VkClearAttachment attachment{};
    attachment.aspectMask = clearAspect;
    attachment.clearValue.depthStencil = value;
    VkClearRect rect{};
    rect.rect = {{0, 0}, TextureExtent(tex)};
    rect.layerCount = 1;
    vkCmdClearAttachments(phxvk::FrameCommandBuffer(), 1, &attachment, 1, &rect);
    return;
  }

  if (bound)
  {
    // A pending clear before the render pass starts: open the pass, clear it.
    EnsureRenderingStarted();
    VkClearAttachment attachment{};
    attachment.aspectMask = clearAspect;
    attachment.clearValue.depthStencil = value;
    VkClearRect rect{};
    rect.rect = {{0, 0}, TextureExtent(tex)};
    rect.layerCount = 1;
    vkCmdClearAttachments(phxvk::FrameCommandBuffer(), 1, &attachment, 1, &rect);
    return;
  }

  EndRendering();
  BeginFrameIfNeeded();
  TransitionImage(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageSubresourceRange range{clearAspect, view->v.baseMip, 1, view->v.baseLayer, view->v.layerCount ? view->v.layerCount : 1};
  vkCmdClearDepthStencilImage(phxvk::FrameCommandBuffer(), tex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

void ContextImpl::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *pUnorderedAccessView, const UINT Values[4])
{
  if (!pUnorderedAccessView) return;
  auto *uav = static_cast<UAVImpl *>(pUnorderedAccessView);
  VkClearColorValue color{};
  for (int i = 0; i < 4; i++) color.uint32[i] = Values ? Values[i] : 0;
  if (!uav->v.texture)
  {
    phxvk::LogWarn("ClearUnorderedAccessViewUint on a buffer is not implemented");
    return;
  }
  EndRendering();
  BeginFrameIfNeeded();
  TransitionImage(uav->v.texture, VK_IMAGE_LAYOUT_GENERAL);
  VkImageSubresourceRange range{uav->v.aspect, uav->v.baseMip, 1, uav->v.baseLayer, uav->v.layerCount ? uav->v.layerCount : 1};
  vkCmdClearColorImage(phxvk::FrameCommandBuffer(), uav->v.texture->vkImage(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

void ContextImpl::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *pUnorderedAccessView, const FLOAT Values[4])
{
  if (!pUnorderedAccessView) return;
  auto *uav = static_cast<UAVImpl *>(pUnorderedAccessView);
  VkClearColorValue color{};
  for (int i = 0; i < 4; i++) color.float32[i] = Values ? Values[i] : 0.0f;
  if (!uav->v.texture)
  {
    phxvk::LogWarn("ClearUnorderedAccessViewFloat on a buffer is not implemented");
    return;
  }
  EndRendering();
  BeginFrameIfNeeded();
  TransitionImage(uav->v.texture, VK_IMAGE_LAYOUT_GENERAL);
  VkImageSubresourceRange range{uav->v.aspect, uav->v.baseMip, 1, uav->v.baseLayer, uav->v.layerCount ? uav->v.layerCount : 1};
  vkCmdClearColorImage(phxvk::FrameCommandBuffer(), uav->v.texture->vkImage(), VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
}

//////////////////////////////////////////////////////////////////////////////
// resource copies / updates

void ContextImpl::CopyResource(ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource)
{
  if (!pDstResource || !pSrcResource) return;
  EndRendering();
  BeginFrameIfNeeded();
  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();

  BufferImpl *dstBuf = AsBuffer(pDstResource);
  BufferImpl *srcBuf = AsBuffer(pSrcResource);
  TextureImpl *dstTex = AsTexture(pDstResource);
  TextureImpl *srcTex = AsTexture(pSrcResource);

  if (dstBuf && srcBuf)
  {
    if (dstBuf->dynamic || srcBuf->dynamic)
    {
      phxvk::LogWarn("CopyResource on a dynamic buffer is not supported (Map() renames those)");
      return;
    }
    VkBufferCopy region{};
    region.size = dstBuf->size() < srcBuf->size() ? dstBuf->size() : srcBuf->size();
    vkCmdCopyBuffer(cmd, srcBuf->buffer, dstBuf->buffer, 1, &region);
    return;
  }
  if (!dstTex || !srcTex) return;

  const VkImageAspectFlags dstAspect = AspectForTexture(dstTex);
  const uint32_t dstMips = dstTex->mipLevels ? dstTex->mipLevels : 1;
  const uint32_t srcMips = srcTex->mipLevels ? srcTex->mipLevels : 1;
  const uint32_t dstLayers = dstTex->arraySize ? dstTex->arraySize : 1;

  if (dstTex->isStaging() && !srcTex->isStaging())
  {
    TransitionImage(srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {dstAspect, 0, 0, 1};
    copy.imageExtent = {srcTex->width, srcTex->height, 1};
    vkCmdCopyImageToBuffer(cmd, srcTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTex->staging, 1, &copy);
    return;
  }
  if (!dstTex->isStaging() && srcTex->isStaging())
  {
    TransitionImage(dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {dstAspect, 0, 0, 1};
    copy.imageExtent = {dstTex->width, dstTex->height, 1};
    vkCmdCopyBufferToImage(cmd, srcTex->staging, dstTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    return;
  }
  if (dstTex->isStaging() && srcTex->isStaging())
  {
    VkBufferCopy region{};
    region.size = dstTex->stagingSize < srcTex->stagingSize ? dstTex->stagingSize : srcTex->stagingSize;
    vkCmdCopyBuffer(cmd, srcTex->staging, dstTex->staging, 1, &region);
    return;
  }

  TransitionImage(srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  TransitionImage(dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageCopy copy{};
  copy.srcSubresource = {dstAspect, 0, 0, 1};
  copy.dstSubresource = {dstAspect, 0, 0, 1};
  copy.extent = {srcTex->width < dstTex->width ? srcTex->width : dstTex->width, srcTex->height < dstTex->height ? srcTex->height : dstTex->height, 1};
  vkCmdCopyImage(cmd, srcTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  (void)srcMips;
  (void)dstMips;
  (void)dstLayers;
}

void ContextImpl::CopySubresourceRegion(ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource,
                                       const D3D11_BOX *pSrcBox)
{
  if (!pDstResource || !pSrcResource) return;
  EndRendering();
  BeginFrameIfNeeded();
  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();

  BufferImpl *dstBuf = AsBuffer(pDstResource);
  BufferImpl *srcBuf = AsBuffer(pSrcResource);
  if (dstBuf && srcBuf)
  {
    VkBufferCopy region{};
    region.srcOffset = SrcSubresource;
    region.dstOffset = DstSubresource;
    region.size = pSrcBox ? (pSrcBox->right - pSrcBox->left) : (srcBuf->size() < dstBuf->size() ? srcBuf->size() : dstBuf->size());
    vkCmdCopyBuffer(cmd, srcBuf->boundBuffer(), dstBuf->boundBuffer(), 1, &region);
    return;
  }

  TextureImpl *dstTex = AsTexture(pDstResource);
  TextureImpl *srcTex = AsTexture(pSrcResource);
  if (!dstTex || !srcTex) return;

  const uint32_t dstMips = dstTex->mipLevels ? dstTex->mipLevels : 1;
  const uint32_t srcMips = srcTex->mipLevels ? srcTex->mipLevels : 1;
  const uint32_t dstMip = DstSubresource % dstMips;
  const uint32_t dstLayer = DstSubresource / dstMips;
  const uint32_t srcMip = SrcSubresource % srcMips;
  const uint32_t srcLayer = SrcSubresource / srcMips;
  const VkImageAspectFlags aspect = AspectForTexture(srcTex);

  // A staging texture is a host-visible *buffer*, not an image: copying into one
  // is the D3D11 readback idiom.  CopyResource() has always handled that; this
  // function did not, and handed MoltenVK a null VkImage, which crashes inside
  // vkCmdCopyImage's validation instead of returning an error.
  const uint32_t stagingBpp = (uint32_t)DXGIFormatBitsPerPixel(dstTex->format) / 8;
  if (dstTex->isStaging() && srcTex->isStaging())
  {
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = dstTex->stagingSize < srcTex->stagingSize ? dstTex->stagingSize : srcTex->stagingSize;
    vkCmdCopyBuffer(cmd, srcTex->staging, dstTex->staging, 1, &region);
    return;
  }
  if (dstTex->isStaging())
  {
    if (srcTex->vkImage() == VK_NULL_HANDLE || dstTex->staging == VK_NULL_HANDLE)
    {
      phxvk::LogWarn("CopySubresourceRegion: a staging texture has no backing memory; copy skipped");
      return;
    }
    TransitionImage(srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = (uint64_t)DstZ * dstTex->stagingRowPitch * dstTex->height + (uint64_t)DstY * dstTex->stagingRowPitch +
                          (uint64_t)DstX * stagingBpp;
    region.bufferRowLength = stagingBpp ? dstTex->stagingRowPitch / stagingBpp : 0;
    region.bufferImageHeight = dstTex->height;
    region.imageSubresource = {aspect, srcMip, srcLayer, 1};
    region.imageOffset = {pSrcBox ? (int32_t)pSrcBox->left : 0, pSrcBox ? (int32_t)pSrcBox->top : 0, pSrcBox ? (int32_t)pSrcBox->front : 0};
    region.imageExtent = pSrcBox ? VkExtent3D{pSrcBox->right - pSrcBox->left, pSrcBox->bottom - pSrcBox->top, pSrcBox->back - pSrcBox->front}
                                 : VkExtent3D{srcTex->width >> srcMip, srcTex->height >> srcMip, 1};
    vkCmdCopyImageToBuffer(cmd, srcTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTex->staging, 1, &region);
    return;
  }
  if (srcTex->isStaging())
  {
    if (dstTex->vkImage() == VK_NULL_HANDLE || srcTex->staging == VK_NULL_HANDLE)
    {
      phxvk::LogWarn("CopySubresourceRegion: a staging texture has no backing memory; copy skipped");
      return;
    }
    TransitionImage(dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = stagingBpp ? srcTex->stagingRowPitch / stagingBpp : 0;
    region.bufferImageHeight = srcTex->height;
    region.imageSubresource = {aspect, dstMip, dstLayer, 1};
    region.imageOffset = {(int32_t)DstX, (int32_t)DstY, (int32_t)DstZ};
    region.imageExtent = pSrcBox ? VkExtent3D{pSrcBox->right - pSrcBox->left, pSrcBox->bottom - pSrcBox->top, pSrcBox->back - pSrcBox->front}
                                 : VkExtent3D{dstTex->width >> dstMip, dstTex->height >> dstMip, 1};
    vkCmdCopyBufferToImage(cmd, srcTex->staging, dstTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    return;
  }
  if (srcTex->vkImage() == VK_NULL_HANDLE || dstTex->vkImage() == VK_NULL_HANDLE)
  {
    phxvk::LogWarn("CopySubresourceRegion: a texture has no VkImage; copy skipped");
    return;
  }

  VkImageCopy copy{};
  copy.srcSubresource = {aspect, srcMip, srcLayer, 1};
  copy.dstSubresource = {aspect, dstMip, dstLayer, 1};
  copy.srcOffset = {pSrcBox ? (int32_t)pSrcBox->left : 0, pSrcBox ? (int32_t)pSrcBox->top : 0, pSrcBox ? (int32_t)pSrcBox->front : 0};
  copy.dstOffset = {(int32_t)DstX, (int32_t)DstY, (int32_t)DstZ};
  if (pSrcBox)
    copy.extent = {pSrcBox->right - pSrcBox->left, pSrcBox->bottom - pSrcBox->top, pSrcBox->back - pSrcBox->front};
  else
    copy.extent = {srcTex->width >> srcMip, srcTex->height >> srcMip, 1};

  TransitionImage(srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  TransitionImage(dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyImage(cmd, srcTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

void ContextImpl::UpdateSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
  if (!pDstResource || !pSrcData) return;
  EndRendering();
  BeginFrameIfNeeded();

  if (BufferImpl *buf = AsBuffer(pDstResource))
  {
    if (buf->dynamic)
    {
      // D3D11 forbids UpdateSubresource on dynamic buffers, but be helpful.
      if (!buf->ring.valid()) buf->ring = phxvk::RingAllocate(buf->desc.ByteWidth);
      if (buf->ring.mapped)
      {
        const uint64_t offset = pDstBox ? pDstBox->left : 0;
        const uint64_t size = pDstBox ? (pDstBox->right - pDstBox->left) : buf->desc.ByteWidth;
        if (offset + size <= buf->desc.ByteWidth) memcpy((uint8_t *)buf->ring.mapped + offset, pSrcData, (size_t)size);
      }
      return;
    }
    const uint64_t offset = pDstBox ? pDstBox->left : 0;
    const uint64_t size = pDstBox ? (pDstBox->right - pDstBox->left) : buf->desc.ByteWidth;
    if (offset + size > buf->desc.ByteWidth)
    {
      phxvk::LogWarn("UpdateSubresource: box exceeds the buffer size");
      return;
    }
    UploadBufferData(buf->buffer, offset, pSrcData, size);
    if (buf->shadow.size() >= offset + size) memcpy(buf->shadow.data() + offset, pSrcData, (size_t)size);
    return;
  }

  TextureImpl *tex = AsTexture(pDstResource);
  if (!tex) return;
  if (tex->isStaging())
  {
    const uint32_t mips = tex->mipLevels ? tex->mipLevels : 1;
    const uint32_t layer = DstSubresource / mips;
    uint64_t offset = (uint64_t)layer * tex->stagingRowPitch * tex->height;
    const uint64_t size = (uint64_t)SrcRowPitch * tex->height;
    if (offset + size > tex->stagingSize)
    {
      phxvk::LogWarn("UpdateSubresource: staging texture is too small");
      return;
    }
    void *mapped = MapStagingMemoryRange(tex->stagingMemory, offset, size);
    if (mapped)
    {
      memcpy(mapped, pSrcData, (size_t)size);
      vkUnmapMemory(phxvk::Vk().device, tex->stagingMemory);
    }
    return;
  }

  const uint32_t mips = tex->mipLevels ? tex->mipLevels : 1;
  const uint32_t mip = DstSubresource % mips;
  const uint32_t layer = DstSubresource / mips;
  if (pDstBox && (pDstBox->left || pDstBox->top || pDstBox->right != tex->width || pDstBox->bottom != tex->height))
    phxvk::LogWarn("UpdateSubresource: a partial destination box is not supported for textures; updating the whole subresource");

  UpdateTextureSubresource(tex, mip, layer, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void ContextImpl::ResolveSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, ID3D11Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format)
{
  (void)Format;
  TextureImpl *dstTex = AsTexture(pDstResource);
  TextureImpl *srcTex = AsTexture(pSrcResource);
  if (!dstTex || !srcTex) return;
  if (srcTex->sampleCount <= 1)
  {
    // Already single-sampled: a plain copy is the D3D11 behaviour.
    CopyResource(pDstResource, pSrcResource);
    return;
  }
  EndRendering();
  BeginFrameIfNeeded();
  const uint32_t dstMips = dstTex->mipLevels ? dstTex->mipLevels : 1;
  const uint32_t srcMips = srcTex->mipLevels ? srcTex->mipLevels : 1;
  const VkImageAspectFlags aspect = AspectForTexture(dstTex);

  TransitionImage(srcTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  TransitionImage(dstTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

  VkImageResolve region{};
  region.srcSubresource = {aspect, SrcSubresource % srcMips, SrcSubresource / srcMips, 1};
  region.dstSubresource = {aspect, DstSubresource % dstMips, DstSubresource / dstMips, 1};
  region.extent = {dstTex->width, dstTex->height, 1};
  vkCmdResolveImage(phxvk::FrameCommandBuffer(), srcTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstTex->vkImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

void ContextImpl::GenerateMips(ID3D11ShaderResourceView *pShaderResourceView)
{
  if (!pShaderResourceView) return;
  auto *srv = static_cast<SRVImpl *>(pShaderResourceView);
  TextureImpl *tex = srv->v.texture;
  if (!tex) return;
  // A texture whose VkImage was never created must not reach the barrier/blit
  // calls below: submitting a barrier with a null image makes MoltenVK
  // dereference a null MVKImage inside applyImageMemoryBarrier and crash with
  // no diagnostic at all.
  if (tex->vkImage() == VK_NULL_HANDLE)
  {
    static bool warned = false;
    if (!warned)
    {
      warned = true;
      phxvk::LogWarn("GenerateMips: texture has no VkImage; skipping mip generation (further occurrences suppressed)");
    }
    return;
  }
  const uint32_t mips = tex->mipLevels ? tex->mipLevels : 1;
  if (mips <= 1) return;

  // R8's mitigation: vkCmdBlitImage needs BLIT_SRC/BLIT_DST for the format, and
  // the linear filter needs SAMPLED_IMAGE_FILTER_LINEAR.  Check before use and
  // fall back to a nearest-neighbour blit rather than silently producing
  // garbage (the engine's R16G16B16A16_FLOAT and R32_FLOAT targets support
  // both on this device).
  const VkFormat fmt = tex->storageFormat != VK_FORMAT_UNDEFINED ? tex->storageFormat : DXGIFormatToVk(tex->format);
  VkFormatProperties fmtProps{};
  vkGetPhysicalDeviceFormatProperties(phxvk::Vk().physical, fmt, &fmtProps);
  if ((fmtProps.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) !=
      (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT))
  {
    static Once once;
    if (once())
      phxvk::LogError("[d3d11] GenerateMips: format %d has no BLIT_SRC/BLIT_DST support; the mip chain is left ungenerated "
                      "(a compute/draw downsample would be required)", (int)fmt);
    return;
  }
  VkFilter filter = VK_FILTER_LINEAR;
  if (!(fmtProps.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
  {
    static Once onceLinear;
    if (onceLinear())
      phxvk::LogWarn("[d3d11] GenerateMips: format %d cannot be linear-filtered; using a nearest-neighbour blit", (int)fmt);
    filter = VK_FILTER_NEAREST;
  }

  EndRendering();
  BeginFrameIfNeeded();
  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();
  const VkImageAspectFlags aspect = AspectForTexture(tex);
  const uint32_t layers = tex->arraySize ? tex->arraySize : 1;

  int32_t w = (int32_t)tex->width;
  int32_t h = (int32_t)tex->height;
  for (uint32_t i = 1; i < mips; i++)
  {
    VkImageMemoryBarrier src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    // Only mip 0 starts in the layout the image had before this call; every
    // later source level was left in TRANSFER_DST_OPTIMAL by the previous
    // iteration's blit, and claiming otherwise is a layout mismatch.
    src.oldLayout = (i == 1) ? tex->currentLayout() : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    src.srcQueueFamilyIndex = src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    src.image = tex->image;
    src.subresourceRange = {aspect, i - 1, 1, 0, layers};
    src.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    VkImageMemoryBarrier dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    // Each destination level is written exactly once, here.
    dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    dst.srcQueueFamilyIndex = dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    dst.image = tex->image;
    dst.subresourceRange = {aspect, i, 1, 0, layers};
    dst.srcAccessMask = 0;
    dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    VkImageMemoryBarrier barriers[2] = {src, dst};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    VkImageBlit blit{};
    blit.srcSubresource = {aspect, i - 1, 0, layers};
    blit.srcOffsets[1] = {w, h, 1};
    blit.dstSubresource = {aspect, i, 0, layers};
    blit.dstOffsets[1] = {w > 1 ? w / 2 : 1, h > 1 ? h / 2 : 1, 1};
    vkCmdBlitImage(cmd, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);

    w = w > 1 ? w / 2 : 1;
    h = h > 1 ? h / 2 : 1;
  }

  // D3D11 leaves the texture ready to be sampled, and the layout tracker is
  // per image rather than per subresource, so every level is moved to
  // SHADER_READ_ONLY_OPTIMAL before recording that as the image's layout:
  // levels 0..mips-2 are in TRANSFER_SRC_OPTIMAL, the last one in
  // TRANSFER_DST_OPTIMAL.
  VkImageMemoryBarrier finalBarriers[2]{};
  uint32_t count = 0;
  if (mips >= 2)
  {
    VkImageMemoryBarrier &first = finalBarriers[count++];
    first.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    first.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    first.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    first.srcQueueFamilyIndex = first.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    first.image = tex->image;
    first.subresourceRange = {aspect, 0, mips - 1, 0, layers};
    first.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    first.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkImageMemoryBarrier &last = finalBarriers[count++];
    last.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    last.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    last.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    last.srcQueueFamilyIndex = last.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    last.image = tex->image;
    last.subresourceRange = {aspect, mips - 1, 1, 0, layers};
    last.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    last.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  }
  if (count)
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                         count, finalBarriers);
  tex->setCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}




//////////////////////////////////////////////////////////////////////////////
// memory mapping

HRESULT ContextImpl::Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT, D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
  if (!pResource || !pMappedResource) return E_INVALIDARG;
  memset(pMappedResource, 0, sizeof(*pMappedResource));

  if (BufferImpl *buf = AsBuffer(pResource))
  {
    if (buf->dynamic)
    {
      if (MapType == D3D11_MAP_READ)
      {
        phxvk::LogWarn("Map(D3D11_MAP_READ) on a dynamic buffer returns undefined contents");
      }
      // Renaming: take a fresh slice of the ring so the GPU can keep reading
      // the old one.
      BeginFrameIfNeeded();
      buf->ring = phxvk::RingAllocate(buf->desc.ByteWidth);
      if (!buf->ring.valid())
      {
        phxvk::LogError("Map: could not allocate %u bytes from the dynamic ring", buf->desc.ByteWidth);
        return E_OUTOFMEMORY;
      }
      buf->mappedPtr = buf->ring.mapped;
      buf->mapped = true;
      if (buf->shadow.size() == buf->desc.ByteWidth && MapType != D3D11_MAP_WRITE_DISCARD)
        memcpy(buf->ring.mapped, buf->shadow.data(), buf->shadow.size());
      pMappedResource->pData = buf->ring.mapped;
      pMappedResource->RowPitch = buf->desc.ByteWidth;
      pMappedResource->DepthPitch = buf->desc.ByteWidth;
      return S_OK;
    }
    if (buf->isStaging)
    {
      if (!buf->mappedPtr)
      {
        buf->mappedPtr = MapStagingMemoryRange(buf->memory, 0, buf->desc.ByteWidth);
        if (!buf->mappedPtr) return E_FAIL;
      }
      pMappedResource->pData = buf->mappedPtr;
      pMappedResource->RowPitch = buf->desc.ByteWidth;
      pMappedResource->DepthPitch = buf->desc.ByteWidth;
      return S_OK;
    }
    phxvk::LogError("Map: only DYNAMIC and STAGING resources can be mapped (this buffer is usage %d)", (int)buf->desc.Usage);
    return E_INVALIDARG;
  }

  TextureImpl *tex = AsTexture(pResource);
  if (!tex) return E_INVALIDARG;
  if (!tex->isStaging())
  {
    phxvk::LogError("Map: only STAGING textures can be mapped; use CopyResource into a staging texture first");
    return E_INVALIDARG;
  }

  // Make sure the copy that filled this staging texture has completed.
  SubmitAndWait();

  const uint32_t mips = tex->mipLevels ? tex->mipLevels : 1;
  const uint32_t layer = Subresource / mips;
  const uint64_t offset = (uint64_t)layer * tex->stagingRowPitch * tex->height;
  void *mapped = MapStagingMemoryRange(tex->stagingMemory, offset, tex->stagingSize - offset);
  if (!mapped)
  {
    phxvk::LogError("Map: vkMapMemory on the staging texture failed");
    return E_FAIL;
  }
  tex->mapped = true;
  pMappedResource->pData = mapped;
  pMappedResource->RowPitch = tex->stagingRowPitch;
  pMappedResource->DepthPitch = tex->stagingRowPitch * tex->height;
  return S_OK;
}

void ContextImpl::Unmap(ID3D11Resource *pResource, UINT)
{
  if (!pResource) return;
  if (BufferImpl *buf = AsBuffer(pResource))
  {
    if (buf->isStaging && buf->mappedPtr)
    {
      // Keep the mapping for the lifetime of the resource: D3D11 semantics
      // allow repeated Map/Unmap without a cost.
      VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
      range.memory = buf->memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      vkFlushMappedMemoryRanges(phxvk::Vk().device, 1, &range);
    }
    if (buf->dynamic && buf->mappedPtr && buf->shadow.size() == buf->desc.ByteWidth)
      memcpy(buf->shadow.data(), buf->mappedPtr, buf->shadow.size());
    buf->mapped = false;
    return;
  }
  if (TextureImpl *tex = AsTexture(pResource))
  {
    if (tex->mapped && tex->stagingMemory)
    {
      vkUnmapMemory(phxvk::Vk().device, tex->stagingMemory);
      tex->mapped = false;
    }
  }
}

//////////////////////////////////////////////////////////////////////////////
// queries

void ContextImpl::Begin(ID3D11Asynchronous *pAsync)
{
  (void)pAsync;
  // Occlusion queries are created and "used" by the engine but never read in a
  // way that affects rendering (see the design doc, section 8).
}

void ContextImpl::End(ID3D11Asynchronous *pAsync) { (void)pAsync; }

HRESULT ContextImpl::GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT)
{
  if (!pAsync || !pData) return E_INVALIDARG;
  // Report "not ready" (S_FALSE) like D3D11 does for an unfinished query when
  // no data is available; the engine's EndOcclusionQuery() loop would spin
  // forever on S_FALSE, so report success with a trivially non-empty result.
  memset(pData, 0, DataSize);
  if (DataSize >= sizeof(UINT64))
  {
    UINT64 *out = (UINT64 *)pData;
    *out = 1; // "something was drawn"
  }
  return S_OK;
}


//////////////////////////////////////////////////////////////////////////////
// remaining shims

} // namespace phxd3d11

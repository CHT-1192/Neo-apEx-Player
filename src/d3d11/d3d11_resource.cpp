// Buffers, textures and views.
//
// Model (docs/DESIGN-d3d11-shim.md section 1):
//   IMMUTABLE/DEFAULT buffer -> device local VkBuffer + a staging copy recorded
//                               into the frame command buffer
//   DYNAMIC buffer           -> suballocation from the frame ring; Map() renames
//   STAGING buffer/texture   -> host visible VkBuffer
//   texture                  -> VkImage (optimal tiling)
//   view                     -> VkImageView (+ the layout it wants)
#include "d3d11_internal.h"

#include "dxgi_formats.h"

#include <cstdio>

namespace phxd3d11
{

namespace
{

// Records a staging -> device copy inside the current frame command buffer.
void UploadBufferDataInternal(VkBuffer dst, uint64_t dstOffset, const void *data, uint64_t size)
{
  if (!data || !size) return;
  phxvk::FrameBegin();
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  VkBuffer staging = phxvk::CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingMem);
  void *mapped = nullptr;
  PHXVK_CHECK(vkMapMemory(phxvk::Vk().device, stagingMem, 0, size, 0, &mapped));
  memcpy(mapped, data, (size_t)size);
  vkUnmapMemory(phxvk::Vk().device, stagingMem);

  VkBufferCopy region{};
  region.srcOffset = 0;
  region.dstOffset = dstOffset;
  region.size = size;
  vkCmdCopyBuffer(phxvk::FrameCommandBuffer(), staging, dst, 1, &region);

  // The copy must be visible to later draws in the same command buffer.
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = dst;
  barrier.offset = dstOffset;
  barrier.size = size;
  vkCmdPipelineBarrier(phxvk::FrameCommandBuffer(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                       0, nullptr, 1, &barrier, 0, nullptr);

  phxvk::DeferFree(staging, stagingMem);
}

// Uploads one subresource of a texture.
void UploadTextureSubresource(TextureImpl *tex, uint32_t mip, uint32_t layer, const void *data, uint32_t rowPitch, uint32_t depthPitch)
{
  if (!data) return;
  const VkFormat format = DXGIFormatToVk(tex->format);
  if (format == VK_FORMAT_UNDEFINED) return;
  if (DXGIFormatIsDepth(tex->format) && tex->bindFlags & D3D11_BIND_DEPTH_STENCIL)
  {
    phxvk::LogWarn("initial data for depth/stencil textures is not uploaded (format %d)", (int)tex->format);
    return;
  }

  uint32_t w = tex->width >> mip;
  uint32_t h = tex->height >> mip;
  uint32_t d = tex->depth >> mip;
  if (!w) w = 1;
  if (!h) h = 1;
  if (!d) d = 1;

  const uint64_t sliceBytes = (uint64_t)(rowPitch ? rowPitch : DXGIFormatRowPitch(tex->format, w)) * ((h + DXGIFormatBlockSize(tex->format) - 1) / DXGIFormatBlockSize(tex->format));
  const uint64_t total = sliceBytes * d;
  if (!total) return;
  (void)depthPitch;

  phxvk::FrameBegin();
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  VkBuffer staging = phxvk::CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingMem);
  void *mapped = nullptr;
  PHXVK_CHECK(vkMapMemory(phxvk::Vk().device, stagingMem, 0, total, 0, &mapped));
  if (depthPitch && depthPitch != rowPitch * h && d > 1)
  {
    for (uint32_t z = 0; z < d; z++) memcpy((uint8_t *)mapped + (uint64_t)z * rowPitch * h, (const uint8_t *)data + (uint64_t)z * depthPitch, (size_t)(rowPitch * h));
  }
  else
  {
    memcpy(mapped, data, (size_t)total);
  }
  vkUnmapMemory(phxvk::Vk().device, stagingMem);

  VkCommandBuffer cmd = phxvk::FrameCommandBuffer();
  VkImageAspectFlags aspect = DXGIFormatIsDepth(tex->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

  VkImageMemoryBarrier toDst = phxvk::ImageBarrier(tex->image, tex->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, aspect, tex->mipLevels, tex->arraySize);
  toDst.srcAccessMask = 0;
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
  tex->layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

  VkBufferImageCopy copy{};
  copy.bufferOffset = 0;
  copy.bufferRowLength = 0;
  copy.bufferImageHeight = 0;
  copy.imageSubresource = {aspect, mip, layer, 1};
  copy.imageOffset = {0, 0, 0};
  copy.imageExtent = {w, h, d};
  vkCmdCopyBufferToImage(cmd, staging, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  VkImageMemoryBarrier toRead = phxvk::ImageBarrier(tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, aspect, tex->mipLevels, tex->arraySize);
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
  tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  phxvk::DeferFree(staging, stagingMem);
}

VkImageUsageFlags UsageForTexture(const TextureImpl *t)
{
  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (t->bindFlags & D3D11_BIND_SHADER_RESOURCE) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (t->bindFlags & D3D11_BIND_RENDER_TARGET) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (t->bindFlags & D3D11_BIND_DEPTH_STENCIL) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (t->bindFlags & D3D11_BIND_UNORDERED_ACCESS) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
  // Render targets that ask for mip generation are also sampled.
  if (t->miscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS) usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  return usage;
}

} // namespace

void UploadBufferData(VkBuffer dst, uint64_t dstOffset, const void *data, uint64_t size) { UploadBufferDataInternal(dst, dstOffset, data, size); }

void *MapStagingMemoryRange(VkDeviceMemory memory, uint64_t offset, uint64_t size)
{
  void *mapped = nullptr;
  VkResult res = vkMapMemory(phxvk::Vk().device, memory, offset, size, 0, &mapped);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkMapMemory failed: %s", phxvk::VkResultName(res));
    return nullptr;
  }
  return mapped;
}

void UpdateTextureSubresource(TextureImpl *tex, uint32_t mip, uint32_t layer, const void *data, uint32_t rowPitch, uint32_t depthPitch)
{
  if (!tex || tex->image == VK_NULL_HANDLE) return;
  UploadTextureSubresource(tex, mip, layer, data, rowPitch, depthPitch);
}

//////////////////////////////////////////////////////////////////////////////
// BufferImpl

void BufferImpl::GetDevice(ID3D11Device **ppDevice)
{
  if (ppDevice)
  {
    *ppDevice = device;
    if (device) device->AddRefImpl();
  }
}
HRESULT BufferImpl::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); }
HRESULT BufferImpl::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); }
HRESULT BufferImpl::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); }
void BufferImpl::GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension)
{
  if (pResourceDimension) *pResourceDimension = D3D11_RESOURCE_DIMENSION_BUFFER;
}
void BufferImpl::SetEvictionPriority(UINT) {}
UINT BufferImpl::GetEvictionPriority(void) { return 0; }
void BufferImpl::GetDesc(D3D11_BUFFER_DESC *pDesc)
{
  if (pDesc) *pDesc = desc;
}
HRESULT BufferImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11Resource)) || GuidEq(riid, __uuidof(ID3D11Buffer)))
  {
    *ppvObject = static_cast<ID3D11Buffer *>(this);
    AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}

BufferImpl::~BufferImpl()
{
  phxvk::Device &d = phxvk::Vk();
  if (d.device && !dynamic)
  {
    if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(d.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) vkFreeMemory(d.device, memory, nullptr);
  }
  buffer = VK_NULL_HANDLE;
  memory = VK_NULL_HANDLE;
  TrackObjectDestroyed("ID3D11Buffer", this);
}

//////////////////////////////////////////////////////////////////////////////
// texture objects

namespace
{

void ReleaseTexture(TextureImpl &tex)
{
  phxvk::Device &d = phxvk::Vk();
  if (!d.device) return;
  if (tex.staging != VK_NULL_HANDLE)
  {
    vkDestroyBuffer(d.device, tex.staging, nullptr);
    tex.staging = VK_NULL_HANDLE;
  }
  if (tex.stagingMemory != VK_NULL_HANDLE)
  {
    vkFreeMemory(d.device, tex.stagingMemory, nullptr);
    tex.stagingMemory = VK_NULL_HANDLE;
  }
  // Swap chain back buffers are owned by the swap chain.
  if (tex.swapchain) return;
  // The engine destroys textures while the immediate context is still
  // recording (its texgen pools churn through textures during resource
  // generation), and the frame command buffer references the image through
  // barriers and descriptor sets.  Destroying it here made MoltenVK walk a
  // freed MVKImage (EXC_BAD_ACCESS in applyImageMemoryBarrier) and made the
  // validation layers report "VkImage ... was destroyed, used by
  // VkDescriptorSet".  Defer it to FrameWaitIdle(), which already waits.
  if (tex.image != VK_NULL_HANDLE || tex.memory != VK_NULL_HANDLE)
  {
    phxvk::DeferFreeImage(tex.image, tex.memory);
    tex.image = VK_NULL_HANDLE;
    tex.memory = VK_NULL_HANDLE;
  }
}

} // namespace

#define PHX_TEXTURE_IMPL(Class, Iface, Kind)                                                \
  Class::Class()                                                                            \
  {                                                                                         \
    tex.self = static_cast<ID3D11Resource *>(static_cast<Iface *>(this));                    \
    tex.dimension = Kind;                                                                    \
  }                                                                                         \
  Class::~Class()                                                                            \
  {                                                                                         \
    ReleaseTexture(tex);                                                                     \
    TrackObjectDestroyed("ID3D11Texture", this);                                             \
  }                                                                                         \
  void Class::GetDevice(ID3D11Device **ppDevice)                                            \
  {                                                                                         \
    if (ppDevice)                                                                            \
    {                                                                                        \
      *ppDevice = device;                                                                    \
      if (device) device->AddRefImpl();                                                      \
    }                                                                                        \
  }                                                                                         \
  HRESULT Class::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); } \
  void Class::GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension)                        \
  {                                                                                         \
    if (pResourceDimension) *pResourceDimension = tex.dimension;                             \
  }                                                                                         \
  void Class::SetEvictionPriority(UINT) {}                                                   \
  UINT Class::GetEvictionPriority(void) { return 0; }                                        \
  HRESULT Class::QueryInterface(REFIID riid, void **ppvObject)                               \
  {                                                                                         \
    if (!ppvObject) return E_POINTER;                                                        \
    *ppvObject = nullptr;                                                                    \
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11Resource)) || GuidEq(riid, __uuidof(Iface))) \
    {                                                                                        \
      *ppvObject = static_cast<Iface *>(this);                                                \
      AddRefImpl();                                                                          \
      return S_OK;                                                                           \
    }                                                                                        \
    return E_NOINTERFACE;                                                                    \
  }

PHX_TEXTURE_IMPL(Texture1DImpl, ID3D11Texture1D, D3D11_RESOURCE_DIMENSION_TEXTURE1D);
PHX_TEXTURE_IMPL(Texture2DImpl, ID3D11Texture2D, D3D11_RESOURCE_DIMENSION_TEXTURE2D);
PHX_TEXTURE_IMPL(Texture3DImpl, ID3D11Texture3D, D3D11_RESOURCE_DIMENSION_TEXTURE3D);
#undef PHX_TEXTURE_IMPL

void Texture1DImpl::GetDesc(D3D11_TEXTURE1D_DESC *pDesc)
{
  if (!pDesc) return;
  memset(pDesc, 0, sizeof(*pDesc));
  pDesc->Width = tex.width;
  pDesc->MipLevels = tex.mipLevels;
  pDesc->ArraySize = tex.arraySize;
  pDesc->Format = tex.format;
  pDesc->Usage = tex.usage;
  pDesc->BindFlags = tex.bindFlags;
  pDesc->CPUAccessFlags = tex.cpuAccessFlags;
  pDesc->MiscFlags = tex.miscFlags;
}

void Texture2DImpl::GetDesc(D3D11_TEXTURE2D_DESC *pDesc)
{
  if (!pDesc) return;
  memset(pDesc, 0, sizeof(*pDesc));
  pDesc->Width = tex.width;
  pDesc->Height = tex.height;
  pDesc->MipLevels = tex.mipLevels;
  pDesc->ArraySize = tex.arraySize;
  pDesc->Format = tex.format;
  pDesc->SampleDesc.Count = tex.sampleCount;
  pDesc->SampleDesc.Quality = tex.sampleQuality;
  pDesc->Usage = tex.usage;
  pDesc->BindFlags = tex.bindFlags;
  pDesc->CPUAccessFlags = tex.cpuAccessFlags;
  pDesc->MiscFlags = tex.miscFlags;
}

void Texture3DImpl::GetDesc(D3D11_TEXTURE3D_DESC *pDesc)
{
  if (!pDesc) return;
  memset(pDesc, 0, sizeof(*pDesc));
  pDesc->Width = tex.width;
  pDesc->Height = tex.height;
  pDesc->Depth = tex.depth;
  pDesc->MipLevels = tex.mipLevels;
  pDesc->Format = tex.format;
  pDesc->Usage = tex.usage;
  pDesc->BindFlags = tex.bindFlags;
  pDesc->CPUAccessFlags = tex.cpuAccessFlags;
  pDesc->MiscFlags = tex.miscFlags;
}

//////////////////////////////////////////////////////////////////////////////
// views

namespace
{

void ReleaseView(ViewImpl &v)
{
  if (v.view == VK_NULL_HANDLE || !phxvk::Vk().device) return;
  // Same deferral as the image: the frame command buffer may still reference
  // this view through a descriptor set, and destroying it early is what turns
  // a texture release into a driver crash.
  if (v.dynamicSwapchainView)
  {
    // Swap chain views are owned and recreated by the swap chain.
    v.view = VK_NULL_HANDLE;
    return;
  }
  phxvk::DeferFreeImageView(v.view);
  v.view = VK_NULL_HANDLE;
}

} // namespace

#define PHX_VIEW_IMPL(Class, Iface)                                                          \
  Class::~Class()                                                                            \
  {                                                                                          \
    ReleaseView(v);                                                                          \
    TrackObjectDestroyed("ID3D11View", this);                                                \
  }                                                                                          \
  void Class::GetDevice(ID3D11Device **ppDevice)                                             \
  {                                                                                          \
    if (ppDevice)                                                                            \
    {                                                                                        \
      *ppDevice = device;                                                                    \
      if (device) device->AddRefImpl();                                                      \
    }                                                                                        \
  }                                                                                          \
  HRESULT Class::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); } \
  void Class::GetResource(ID3D11Resource **ppResource)                                       \
  {                                                                                          \
    if (!ppResource) return;                                                                 \
    *ppResource = ViewResource(v);                                                           \
    if (*ppResource) (*ppResource)->AddRef();                                                 \
  }                                                                                          \
  void Class::GetDesc(decltype(Class::desc) *pDesc) { if (pDesc) *pDesc = desc; }             \
  HRESULT Class::QueryInterface(REFIID riid, void **ppvObject)                                \
  {                                                                                          \
    if (!ppvObject) return E_POINTER;                                                        \
    *ppvObject = nullptr;                                                                    \
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11View)) || GuidEq(riid, __uuidof(Iface))) \
    {                                                                                        \
      *ppvObject = static_cast<Iface *>(this);                                                \
      AddRefImpl();                                                                          \
      return S_OK;                                                                           \
    }                                                                                        \
    return E_NOINTERFACE;                                                                    \
  }

PHX_VIEW_IMPL(SRVImpl, ID3D11ShaderResourceView);
PHX_VIEW_IMPL(RTVImpl, ID3D11RenderTargetView);
PHX_VIEW_IMPL(DSVImpl, ID3D11DepthStencilView);
PHX_VIEW_IMPL(UAVImpl, ID3D11UnorderedAccessView);
#undef PHX_VIEW_IMPL

//////////////////////////////////////////////////////////////////////////////
// device: buffers

HRESULT DeviceImpl::CreateBuffer(const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Buffer **ppBuffer)
{
  if (!ppBuffer) return E_POINTER;
  *ppBuffer = nullptr;
  if (!pDesc || !pDesc->ByteWidth) return E_INVALIDARG;

  auto *buf = new BufferImpl();
  buf->device = this;
  buf->desc = *pDesc;
  buf->dynamic = (pDesc->Usage == D3D11_USAGE_DYNAMIC);
  buf->isStaging = (pDesc->Usage == D3D11_USAGE_STAGING);

  VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (pDesc->BindFlags & D3D11_BIND_VERTEX_BUFFER) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (pDesc->BindFlags & D3D11_BIND_INDEX_BUFFER) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (pDesc->BindFlags & (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (pDesc->MiscFlags & D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

  if (buf->dynamic)
  {
    // Renaming allocator: Map(WRITE_DISCARD) takes a fresh slice each time.
    buf->ring = phxvk::RingAllocate(pDesc->ByteWidth);
    if (!buf->ring.valid())
    {
      phxvk::LogError("CreateBuffer: the dynamic ring is unavailable (was vkInitialize() called?)");
      delete buf;
      return E_FAIL;
    }
  }
  else if (buf->isStaging)
  {
    buf->buffer = phxvk::CreateBuffer(pDesc->ByteWidth, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buf->memory);
    if (!buf->buffer)
    {
      delete buf;
      return E_FAIL;
    }
    buf->mappedPtr = MapStagingMemoryRange(buf->memory, 0, pDesc->ByteWidth);
  }
  else
  {
    buf->buffer = phxvk::CreateBuffer(pDesc->ByteWidth, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buf->memory);
    if (!buf->buffer)
    {
      delete buf;
      return E_FAIL;
    }
    if (pInitialData && pInitialData->pSysMem) UploadBufferDataInternal(buf->buffer, 0, pInitialData->pSysMem, pDesc->ByteWidth);
  }

  if (buf->wantsShadow())
  {
    buf->shadow.assign(pDesc->ByteWidth, 0);
    if (pInitialData && pInitialData->pSysMem) memcpy(buf->shadow.data(), pInitialData->pSysMem, pDesc->ByteWidth);
  }

  TrackObjectCreated("ID3D11Buffer", buf);
  *ppBuffer = buf;
  return S_OK;
}

//////////////////////////////////////////////////////////////////////////////
// device: textures

namespace
{

// Fills in the common part of a TextureImpl from a description and creates the
// Vulkan image (or the staging buffer for USAGE_STAGING).
HRESULT CreateTextureCommon(DeviceImpl *device, TextureImpl *tex, VkImageType type, const void *initialData)
{
  (void)device;
  if (!tex->width || !tex->height) return E_INVALIDARG;

  if (tex->usage == D3D11_USAGE_STAGING)
  {
    const UINT bpp = DXGIFormatBitsPerPixel(tex->format);
    if (!bpp)
    {
      phxvk::LogError("CreateTexture: unsupported staging format %d", (int)tex->format);
      return E_INVALIDARG;
    }
    tex->stagingRowPitch = DXGIFormatRowPitch(tex->format, tex->width);
    tex->stagingSize = (uint64_t)tex->stagingRowPitch * tex->height * tex->depth * tex->arraySize;
    if (!tex->stagingSize) return E_INVALIDARG;
    tex->staging = phxvk::CreateBuffer(tex->stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &tex->stagingMemory);
    if (!tex->staging) return E_FAIL;
    return S_OK;
  }

  // The device may not support the D3D11 storage format (notably 24-bit depth
  // on MoltenVK - risk R3); the substitute is remembered on the texture so
  // every view over it uses the same format.
  VkFormat format = DXGIFormatToVkSupported(tex->format, (tex->bindFlags & D3D11_BIND_SHADER_RESOURCE) != 0);
  if (format == VK_FORMAT_UNDEFINED)
  {
    phxvk::LogError("CreateTexture: unsupported format %d", (int)tex->format);
    return E_INVALIDARG;
  }
  tex->storageFormat = format;

  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = type;
  ci.format = format;
  ci.extent = {tex->width, tex->height, tex->depth};
  ci.mipLevels = tex->mipLevels;
  ci.arrayLayers = tex->arraySize;
  ci.samples = DXGISampleCountToVk(tex->sampleCount);
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = UsageForTexture(tex);
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (tex->miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  if (type == VK_IMAGE_TYPE_2D && tex->arraySize >= 6 && (tex->miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

  VkResult res = vkCreateImage(phxvk::Vk().device, &ci, nullptr, &tex->image);
  if (getenv("APEX_MEM_PROBE"))
    phxvk::LogInfo("[tex] %ux%ux%u mips %u array %u fmt %d -> %.1f MB", tex->width, tex->height, tex->depth,
                   tex->mipLevels, tex->arraySize, (int)format,
                   (double)ci.extent.width * ci.extent.height * ci.extent.depth * (ci.mipLevels ? ci.mipLevels : 1) / 1.0e6),
                 phxvk::LogInfo("[tex]   bind 0x%x misc 0x%x", (unsigned)tex->bindFlags, (unsigned)tex->miscFlags);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkCreateImage failed for %ux%ux%u format %d: %s", tex->width, tex->height, tex->depth, (int)format, phxvk::VkResultName(res));
    return E_FAIL;
  }
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(phxvk::Vk().device, tex->image, &req);
  tex->memory = phxvk::AllocateAndBind(tex->image, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  if (initialData)
  {
    // Only the data for subresource 0 is available at creation time; the
    // engine passes one D3D11_SUBRESOURCE_DATA per (mip, layer) otherwise.
    const D3D11_SUBRESOURCE_DATA *data = (const D3D11_SUBRESOURCE_DATA *)initialData;
    UploadTextureSubresource(tex, 0, 0, data->pSysMem, data->SysMemPitch, data->SysMemSlicePitch);
  }
  return S_OK;
}

} // namespace

HRESULT DeviceImpl::CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D)
{
  if (!ppTexture2D) return E_POINTER;
  *ppTexture2D = nullptr;
  if (!pDesc || !pDesc->Width || !pDesc->Height) return E_INVALIDARG;

  auto *t = new Texture2DImpl();
  t->device = this;
  t->tex.width = pDesc->Width;
  t->tex.height = pDesc->Height;
  t->tex.depth = 1;
  t->tex.arraySize = pDesc->ArraySize ? pDesc->ArraySize : 1;
  t->tex.format = pDesc->Format;
  t->tex.sampleCount = pDesc->SampleDesc.Count ? pDesc->SampleDesc.Count : 1;
  t->tex.sampleQuality = pDesc->SampleDesc.Quality;
  t->tex.usage = pDesc->Usage;
  t->tex.bindFlags = pDesc->BindFlags;
  t->tex.cpuAccessFlags = pDesc->CPUAccessFlags;
  t->tex.miscFlags = pDesc->MiscFlags;
  // "MipLevels = 0" means "a full mip chain" only when mip generation was
  // requested; DX11Texture.cpp relies on exactly that.
  t->tex.mipLevels = pDesc->MipLevels;
  if (t->tex.mipLevels == 0)
  {
    UINT levels = 1;
    UINT dim = pDesc->Width > pDesc->Height ? pDesc->Width : pDesc->Height;
    while (dim > 1)
    {
      dim >>= 1;
      levels++;
    }
    t->tex.mipLevels = levels;
  }

  HRESULT hr = CreateTextureCommon(this, &t->tex, VK_IMAGE_TYPE_2D, pInitialData);
  if (FAILED(hr))
  {
    delete t;
    return hr;
  }
  TrackObjectCreated("ID3D11Texture2D", t);
  *ppTexture2D = t;
  return S_OK;
}

HRESULT DeviceImpl::CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture1D **ppTexture1D)
{
  if (!ppTexture1D) return E_POINTER;
  *ppTexture1D = nullptr;
  if (!pDesc || !pDesc->Width) return E_INVALIDARG;

  auto *t = new Texture1DImpl();
  t->device = this;
  t->tex.width = pDesc->Width;
  t->tex.height = 1;
  t->tex.depth = 1;
  t->tex.arraySize = pDesc->ArraySize ? pDesc->ArraySize : 1;
  t->tex.format = pDesc->Format;
  t->tex.usage = pDesc->Usage;
  t->tex.bindFlags = pDesc->BindFlags;
  t->tex.cpuAccessFlags = pDesc->CPUAccessFlags;
  t->tex.miscFlags = pDesc->MiscFlags;
  t->tex.mipLevels = pDesc->MipLevels ? pDesc->MipLevels : 1;

  HRESULT hr = CreateTextureCommon(this, &t->tex, VK_IMAGE_TYPE_1D, pInitialData);
  if (FAILED(hr))
  {
    delete t;
    return hr;
  }
  TrackObjectCreated("ID3D11Texture1D", t);
  *ppTexture1D = t;
  return S_OK;
}

HRESULT DeviceImpl::CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture3D **ppTexture3D)
{
  if (!ppTexture3D) return E_POINTER;
  *ppTexture3D = nullptr;
  if (!pDesc || !pDesc->Width || !pDesc->Height || !pDesc->Depth) return E_INVALIDARG;

  auto *t = new Texture3DImpl();
  t->device = this;
  t->tex.width = pDesc->Width;
  t->tex.height = pDesc->Height;
  t->tex.depth = pDesc->Depth;
  t->tex.arraySize = 1;
  t->tex.format = pDesc->Format;
  t->tex.usage = pDesc->Usage;
  t->tex.bindFlags = pDesc->BindFlags;
  t->tex.cpuAccessFlags = pDesc->CPUAccessFlags;
  t->tex.miscFlags = pDesc->MiscFlags;
  t->tex.mipLevels = pDesc->MipLevels ? pDesc->MipLevels : 1;

  HRESULT hr = CreateTextureCommon(this, &t->tex, VK_IMAGE_TYPE_3D, pInitialData);
  if (FAILED(hr))
  {
    delete t;
    return hr;
  }
  TrackObjectCreated("ID3D11Texture3D", t);
  *ppTexture3D = t;
  return S_OK;
}

//////////////////////////////////////////////////////////////////////////////
// device: views

namespace
{

bool IsDepthFormat(DXGI_FORMAT f) { return DXGIFormatIsDepth(f); }

// Derives the view parameters a NULL view description implies.
void DefaultSRVDesc(const TextureImpl *tex, D3D11_SHADER_RESOURCE_VIEW_DESC *out)
{
  memset(out, 0, sizeof(*out));
  out->Format = DXGIFormatIsTypeless(tex->format) ? DXGIMakeNotTypeless(tex->format) : tex->format;
  if (tex->sampleCount > 1)
    out->ViewDimension = tex->arraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
  else if (tex->arraySize > 1)
    out->ViewDimension = (tex->miscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
  else
    out->ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  out->Texture2D.MostDetailedMip = 0;
  out->Texture2D.MipLevels = tex->mipLevels;
  out->Texture2DArray.MostDetailedMip = 0;
  out->Texture2DArray.MipLevels = tex->mipLevels;
  out->Texture2DArray.FirstArraySlice = 0;
  out->Texture2DArray.ArraySize = tex->arraySize;
}

} // namespace

HRESULT DeviceImpl::CreateShaderResourceView(ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView)
{
  if (!ppSRView) return E_POINTER;
  *ppSRView = nullptr;
  if (!pResource) return E_INVALIDARG;

  auto *srv = new SRVImpl();
  srv->device = this;

  if (BufferImpl *buf = AsBuffer(pResource))
  {
    srv->v.buffer = buf;
    srv->v.format = VK_FORMAT_UNDEFINED;
    if (pDesc) srv->desc = *pDesc;
    else
    {
      memset(&srv->desc, 0, sizeof(srv->desc));
      srv->desc.Format = DXGI_FORMAT_UNKNOWN;
      srv->desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
      srv->desc.Buffer.FirstElement = 0;
      srv->desc.Buffer.NumElements = buf->desc.ByteWidth / (buf->desc.StructureByteStride ? buf->desc.StructureByteStride : 1);
    }
  }
  else if (TextureImpl *tex = AsTexture(pResource))
  {
    srv->v.texture = tex;
    D3D11_SHADER_RESOURCE_VIEW_DESC d;
    if (pDesc) d = *pDesc;
    else DefaultSRVDesc(tex, &d);
    srv->desc = d;

    DXGI_FORMAT viewFormat = d.Format ? d.Format : tex->format;
    const bool depth = IsDepthFormat(tex->format) || IsDepthFormat(viewFormat);
    const bool stencil = DXGIFormatIsStencil(viewFormat) && depth;
    srv->v.isDepth = depth;
    srv->v.isStencil = stencil;

    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    // A depth/stencil SRV reads only the depth aspect in D3D11.
    srv->v.aspect = aspect;

    // A depth/stencil image is sampled through a view of its own storage
    // format: the D3D11 view format (R24_UNORM_X8_TYPELESS and friends) has no
    // Vulkan equivalent, and a different VkFormat would need MUTABLE_FORMAT.
    // The image format may itself have been substituted for one this device
    // supports (R3).  Sampling such a view returns the depth in .x, which is
    // what `Texture2D z:register(t7)` reads.
    VkFormat vkFormat;
    if ((depth || stencil) && tex->storageFormat != VK_FORMAT_UNDEFINED)
      vkFormat = tex->storageFormat;
    else
      vkFormat = DXGIFormatToVkView(viewFormat, DXGIFormatToVk(tex->format), depth, stencil);
    srv->v.format = vkFormat;

    uint32_t baseMip = 0, mips = tex->mipLevels, baseLayer = 0, layers = tex->arraySize;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    switch (d.ViewDimension)
    {
    case D3D11_SRV_DIMENSION_TEXTURE1D:
      baseMip = d.Texture1D.MostDetailedMip;
      mips = d.Texture1D.MipLevels ? d.Texture1D.MipLevels : 1;
      type = VK_IMAGE_VIEW_TYPE_1D;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
      baseMip = d.Texture1DArray.MostDetailedMip;
      mips = d.Texture1DArray.MipLevels ? d.Texture1DArray.MipLevels : 1;
      baseLayer = d.Texture1DArray.FirstArraySlice;
      layers = d.Texture1DArray.ArraySize ? d.Texture1DArray.ArraySize : 1;
      type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2D:
      baseMip = d.Texture2D.MostDetailedMip;
      mips = d.Texture2D.MipLevels ? d.Texture2D.MipLevels : 1;
      type = VK_IMAGE_VIEW_TYPE_2D;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
      baseMip = d.Texture2DArray.MostDetailedMip;
      mips = d.Texture2DArray.MipLevels ? d.Texture2DArray.MipLevels : 1;
      baseLayer = d.Texture2DArray.FirstArraySlice;
      layers = d.Texture2DArray.ArraySize ? d.Texture2DArray.ArraySize : 1;
      type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      break;
    case D3D11_SRV_DIMENSION_TEXTURECUBE:
      baseMip = d.TextureCube.MostDetailedMip;
      mips = d.TextureCube.MipLevels ? d.TextureCube.MipLevels : 1;
      layers = 6;
      type = VK_IMAGE_VIEW_TYPE_CUBE;
      break;
    case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
      baseMip = d.TextureCubeArray.MostDetailedMip;
      mips = d.TextureCubeArray.MipLevels ? d.TextureCubeArray.MipLevels : 1;
      baseLayer = d.TextureCubeArray.First2DArrayFace;
      layers = d.TextureCubeArray.NumCubes * 6;
      type = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE3D:
      baseMip = d.Texture3D.MostDetailedMip;
      mips = d.Texture3D.MipLevels ? d.Texture3D.MipLevels : 1;
      layers = 1;
      type = VK_IMAGE_VIEW_TYPE_3D;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMS:
      type = VK_IMAGE_VIEW_TYPE_2D;
      mips = 1;
      break;
    case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
      type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      mips = 1;
      baseLayer = d.Texture2DMSArray.FirstArraySlice;
      layers = d.Texture2DMSArray.ArraySize ? d.Texture2DMSArray.ArraySize : 1;
      break;
    default:
      phxvk::LogWarn("CreateShaderResourceView: unhandled view dimension %d; using a plain 2D view", (int)d.ViewDimension);
      type = tex->arraySize > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
      break;
    }
    // D3D11 spells "every remaining level" as MipLevels == (UINT)-1 in a view
    // desc (and 0 in some callers); taking either literally would ask for an
    // absurd level count and produce an invalid VkImageView.
    if (mips == 0 || mips == 0xFFFFFFFFu || baseMip + mips > tex->mipLevels)
      mips = tex->mipLevels > baseMip ? tex->mipLevels - baseMip : 1;
    srv->v.baseMip = baseMip;
    srv->v.mipCount = mips;
    srv->v.baseLayer = baseLayer;
    srv->v.layerCount = layers;
    srv->v.viewType = type;

    if (tex->swapchain)
    {
      // The swap chain owns one view per image; the RTV/SRV resolves the
      // acquired one at bind time.
      srv->v.dynamicSwapchainView = true;
    }
    else
    {
      srv->v.view = CreateTextureView(tex, vkFormat, aspect, baseMip, mips, baseLayer, layers, type);
      if (srv->v.view == VK_NULL_HANDLE && depth)
      {
        // Some MoltenVK/Metal combinations cannot sample the X8_D24 format;
        // fall back to the storage format of the image.
        VkFormat fallback = DXGIFormatToVk(tex->format);
        if (fallback != vkFormat)
        {
          phxvk::LogWarn("CreateShaderResourceView: falling back to the image format for the depth view");
          vkFormat = fallback;
          srv->v.format = fallback;
          srv->v.view = CreateTextureView(tex, vkFormat, aspect, baseMip, mips, baseLayer, layers, type);
        }
      }
      if (srv->v.view == VK_NULL_HANDLE)
      {
        delete srv;
        return E_FAIL;
      }
    }
  }
  else
  {
    delete srv;
    return E_INVALIDARG;
  }

  TrackObjectCreated("ID3D11ShaderResourceView", srv);
  *ppSRView = srv;
  return S_OK;
}

HRESULT DeviceImpl::CreateRenderTargetView(ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc, ID3D11RenderTargetView **ppRTView)
{
  if (!ppRTView) return E_POINTER;
  *ppRTView = nullptr;
  if (!pResource) return E_INVALIDARG;

  auto *rtv = new RTVImpl();
  rtv->device = this;

  if (BufferImpl *buf = AsBuffer(pResource))
  {
    // D3D11 can render to a buffer (with the appropriate format); Vulkan needs
    // VK_EXT_... support for that, so be explicit about not supporting it.
    (void)buf;
    phxvk::LogError("CreateRenderTargetView: rendering to a buffer is not supported by this port");
    delete rtv;
    return E_NOTIMPL;
  }
  TextureImpl *tex = AsTexture(pResource);
  if (!tex)
  {
    delete rtv;
    return E_INVALIDARG;
  }

  rtv->v.texture = tex;
  D3D11_RENDER_TARGET_VIEW_DESC d;
  memset(&d, 0, sizeof(d));
  if (pDesc) d = *pDesc;
  else
  {
    d.Format = DXGIFormatIsTypeless(tex->format) ? DXGIMakeNotTypeless(tex->format) : tex->format;
    d.ViewDimension = tex->sampleCount > 1 ? (tex->arraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS)
                                           : (tex->arraySize > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D);
    d.Texture2DArray.MipSlice = 0;
    d.Texture2DArray.FirstArraySlice = 0;
    d.Texture2DArray.ArraySize = tex->arraySize;
  }
  rtv->desc = d;
  rtv->v.format = DXGIFormatToVk(d.Format ? d.Format : tex->format);

  // The engine asks for a D3D11_RTV_DIMENSION_TEXTURE2DMS view of single-sample
  // render targets (Project.cpp:205, phxEngine.cpp:277).  D3D11 would reject
  // that; this shim quietly corrects it below, which is the behaviour we want --
  // but it is worth saying once, because a view dimension that disagrees with
  // the texture is the kind of thing that later looks like a rendering bug.
  {
    const bool msView = d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMS ||
                        d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
    static bool saidOnce = false;
    if (!saidOnce && msView != (tex->sampleCount > 1))
    {
      saidOnce = true;
      phxvk::LogInfo("[d3d11] the engine asked for a %s render target view of a %u-sample texture; using the "
                     "matching dimension",
                     msView ? "multisampled" : "single-sample", tex->sampleCount);
    }
  }

  uint32_t baseMip = 0, mips = 1, baseLayer = 0, layers = 1;
  VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
  switch (d.ViewDimension)
  {
  case D3D11_RTV_DIMENSION_TEXTURE2D:
    baseMip = d.Texture2D.MipSlice;
    type = VK_IMAGE_VIEW_TYPE_2D;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
    baseMip = d.Texture2DArray.MipSlice;
    baseLayer = d.Texture2DArray.FirstArraySlice;
    layers = d.Texture2DArray.ArraySize ? d.Texture2DArray.ArraySize : 1;
    type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2DMS:
    type = VK_IMAGE_VIEW_TYPE_2D;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
    baseLayer = d.Texture2DMSArray.FirstArraySlice;
    layers = d.Texture2DMSArray.ArraySize ? d.Texture2DMSArray.ArraySize : 1;
    type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1D:
    baseMip = d.Texture1D.MipSlice;
    type = VK_IMAGE_VIEW_TYPE_1D;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
    baseMip = d.Texture1DArray.MipSlice;
    baseLayer = d.Texture1DArray.FirstArraySlice;
    layers = d.Texture1DArray.ArraySize ? d.Texture1DArray.ArraySize : 1;
    type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    break;
  case D3D11_RTV_DIMENSION_TEXTURE3D:
    baseMip = d.Texture3D.MipSlice;
    layers = 1;
    type = VK_IMAGE_VIEW_TYPE_3D;
    break;
  default:
    phxvk::LogWarn("CreateRenderTargetView: unhandled view dimension %d", (int)d.ViewDimension);
    break;
  }
  rtv->v.baseMip = baseMip;
  rtv->v.mipCount = mips;
  rtv->v.baseLayer = baseLayer;
  rtv->v.layerCount = layers;
  rtv->v.viewType = type;
  rtv->v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

  if (tex->swapchain)
  {
    rtv->v.dynamicSwapchainView = true;
  }
  else
  {
    rtv->v.view = CreateTextureView(tex, rtv->v.format, VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, baseLayer, layers, type);
    if (rtv->v.view == VK_NULL_HANDLE)
    {
      delete rtv;
      return E_FAIL;
    }
  }

  TrackObjectCreated("ID3D11RenderTargetView", rtv);
  *ppRTView = rtv;
  return S_OK;
}

HRESULT DeviceImpl::CreateDepthStencilView(ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView)
{
  if (!ppDepthStencilView) return E_POINTER;
  *ppDepthStencilView = nullptr;
  TextureImpl *tex = AsTexture(pResource);
  if (!tex) return E_INVALIDARG;

  auto *dsv = new DSVImpl();
  dsv->device = this;
  dsv->v.texture = tex;

  D3D11_DEPTH_STENCIL_VIEW_DESC d;
  memset(&d, 0, sizeof(d));
  if (pDesc) d = *pDesc;
  else
  {
    d.Format = DXGIMakeNotTypeless(tex->format);
    d.ViewDimension = tex->sampleCount > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
    d.Texture2D.MipSlice = 0;
  }
  dsv->desc = d;

  // A depth view must use a format compatible with the image it views.  The
  // image format may have been substituted for one the device supports (R3),
  // and a differing view format would also require MUTABLE_FORMAT, so the
  // image's own storage format wins for depth/stencil views.
  VkFormat vkFormat = tex->storageFormat != VK_FORMAT_UNDEFINED ? tex->storageFormat : DXGIFormatToVk(tex->format);
  if (vkFormat == VK_FORMAT_UNDEFINED) vkFormat = DXGIFormatToVk(d.Format ? d.Format : tex->format);
  if (vkFormat == VK_FORMAT_UNDEFINED) return E_INVALIDARG;
  // Depth+stencil images need both aspects in the view used for rendering.
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (DXGIFormatIsStencil(tex->format)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
  dsv->v.aspect = aspect;
  dsv->v.format = vkFormat;
  dsv->v.isDepth = true;
  dsv->v.isStencil = DXGIFormatIsStencil(tex->format) != 0;

  uint32_t baseMip = d.Texture2D.MipSlice;
  uint32_t baseLayer = 0, layers = 1;
  VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
  if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
  {
    baseLayer = d.Texture2DArray.FirstArraySlice;
    layers = d.Texture2DArray.ArraySize ? d.Texture2DArray.ArraySize : 1;
    type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  }
  else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY)
  {
    baseLayer = d.Texture2DMSArray.FirstArraySlice;
    layers = d.Texture2DMSArray.ArraySize ? d.Texture2DMSArray.ArraySize : 1;
    type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  }
  dsv->v.baseMip = baseMip;
  dsv->v.mipCount = 1;
  dsv->v.baseLayer = baseLayer;
  dsv->v.layerCount = layers;
  dsv->v.viewType = type;

  dsv->v.view = CreateTextureView(tex, vkFormat, aspect, baseMip, 1, baseLayer, layers, type);
  if (dsv->v.view == VK_NULL_HANDLE)
  {
    delete dsv;
    return E_FAIL;
  }

  TrackObjectCreated("ID3D11DepthStencilView", dsv);
  *ppDepthStencilView = dsv;
  return S_OK;
}

HRESULT DeviceImpl::CreateUnorderedAccessView(ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc, ID3D11UnorderedAccessView **ppUAView)
{
  if (!ppUAView) return E_POINTER;
  *ppUAView = nullptr;
  if (!pResource) return E_INVALIDARG;

  auto *uav = new UAVImpl();
  uav->device = this;

  if (BufferImpl *buf = AsBuffer(pResource))
  {
    uav->v.buffer = buf;
    if (pDesc) uav->desc = *pDesc;
    else
    {
      memset(&uav->desc, 0, sizeof(uav->desc));
      uav->desc.Format = DXGI_FORMAT_UNKNOWN;
      uav->desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
      uav->desc.Buffer.NumElements = buf->desc.ByteWidth / (buf->desc.StructureByteStride ? buf->desc.StructureByteStride : 1);
    }
    uav->v.format = VK_FORMAT_UNDEFINED;
  }
  else if (TextureImpl *tex = AsTexture(pResource))
  {
    uav->v.texture = tex;
    if (pDesc) uav->desc = *pDesc;
    else
    {
      memset(&uav->desc, 0, sizeof(uav->desc));
      uav->desc.Format = DXGIMakeNotTypeless(tex->format);
      uav->desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
      uav->desc.Texture2D.MipSlice = 0;
    }
    VkFormat vkFormat = DXGIFormatToVk(uav->desc.Format ? uav->desc.Format : tex->format);
    uav->v.format = vkFormat;
    uav->v.aspect = DXGIFormatIsDepth(tex->format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    uint32_t baseMip = 0, layers = 1, baseLayer = 0;
    VkImageViewType type = VK_IMAGE_VIEW_TYPE_2D;
    switch (uav->desc.ViewDimension)
    {
    case D3D11_UAV_DIMENSION_BUFFER: break;
    case D3D11_UAV_DIMENSION_TEXTURE1D:
      baseMip = uav->desc.Texture1D.MipSlice;
      type = VK_IMAGE_VIEW_TYPE_1D;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
      baseMip = uav->desc.Texture1DArray.MipSlice;
      baseLayer = uav->desc.Texture1DArray.FirstArraySlice;
      layers = uav->desc.Texture1DArray.ArraySize ? uav->desc.Texture1DArray.ArraySize : 1;
      type = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE2D:
      baseMip = uav->desc.Texture2D.MipSlice;
      type = VK_IMAGE_VIEW_TYPE_2D;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
      baseMip = uav->desc.Texture2DArray.MipSlice;
      baseLayer = uav->desc.Texture2DArray.FirstArraySlice;
      layers = uav->desc.Texture2DArray.ArraySize ? uav->desc.Texture2DArray.ArraySize : 1;
      type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
      break;
    case D3D11_UAV_DIMENSION_TEXTURE3D:
      baseMip = uav->desc.Texture3D.MipSlice;
      type = VK_IMAGE_VIEW_TYPE_3D;
      break;
    default:
      phxvk::LogWarn("CreateUnorderedAccessView: unhandled view dimension %d", (int)uav->desc.ViewDimension);
      break;
    }
    uav->v.baseMip = baseMip;
    uav->v.mipCount = 1;
    uav->v.baseLayer = baseLayer;
    uav->v.layerCount = layers;
    uav->v.viewType = type;

    uav->v.view = CreateTextureView(tex, vkFormat, uav->v.aspect, baseMip, 1, baseLayer, layers, type);
    if (uav->v.view == VK_NULL_HANDLE)
    {
      delete uav;
      return E_FAIL;
    }
  }
  else
  {
    delete uav;
    return E_INVALIDARG;
  }

  TrackObjectCreated("ID3D11UnorderedAccessView", uav);
  *ppUAView = uav;
  return S_OK;
}

} // namespace phxd3d11

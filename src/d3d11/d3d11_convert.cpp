// D3D11 enum -> Vulkan enum conversion plus a few shared utilities.
#include "d3d11_internal.h"

#include "dxgi_formats.h"

#include <cstdio>

namespace phxd3d11
{

Once &OnceGsUnsupported()
{
  static Once o;
  return o;
}

// How many pipelines had a geometry shader stage dropped because the device
// cannot run one.  The stage is not decorative -- see the note in
// d3d11_context.cpp where it is dropped.
unsigned g_gsSkippedPipelines = 0;
Once &OnceFullscreen()
{
  static Once o;
  return o;
}
Once &OnceCompressedUpload()
{
  static Once o;
  return o;
}
Once &OnceReductionMode()
{
  static Once o;
  return o;
}

const char *HResultName(HRESULT hr)
{
  switch ((unsigned long)hr)
  {
  case 0: return "S_OK";
  case 0x80070057: return "E_INVALIDARG";
  case 0x80004005: return "E_FAIL";
  case 0x80004001: return "E_NOTIMPL";
  case 0x8007000E: return "E_OUTOFMEMORY";
  case 0x80070005: return "E_ACCESSDENIED";
  case 0x887A0001: return "DXGI_ERROR_INVALID_CALL";
  case 0x887A0004: return "DXGI_ERROR_UNSUPPORTED";
  case 0x887A0005: return "DXGI_ERROR_DEVICE_REMOVED";
  case 0x087A0001: return "DXGI_STATUS_OCCLUDED";
  default: return "HRESULT";
  }
}

//////////////////////////////////////////////////////////////////////////////
// conversions

VkBlendFactor ToVkBlendFactor(D3D11_BLEND b)
{
  switch (b)
  {
  case D3D11_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
  case D3D11_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
  case D3D11_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
  case D3D11_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case D3D11_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
  case D3D11_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case D3D11_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
  case D3D11_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  case D3D11_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
  case D3D11_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case D3D11_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  case D3D11_BLEND_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
  case D3D11_BLEND_INV_BLEND_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
  case D3D11_BLEND_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
  case D3D11_BLEND_INV_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
  case D3D11_BLEND_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
  case D3D11_BLEND_INV_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
  default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp ToVkBlendOp(D3D11_BLEND_OP op)
{
  switch (op)
  {
  case D3D11_BLEND_OP_ADD: return VK_BLEND_OP_ADD;
  case D3D11_BLEND_OP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
  case D3D11_BLEND_OP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
  case D3D11_BLEND_OP_MIN: return VK_BLEND_OP_MIN;
  case D3D11_BLEND_OP_MAX: return VK_BLEND_OP_MAX;
  default: return VK_BLEND_OP_ADD;
  }
}

VkCompareOp ToVkCompareOp(D3D11_COMPARISON_FUNC f)
{
  // D3D11 starts at NEVER = 1, Vulkan at NEVER = 0.
  int v = (int)f - 1;
  if (v < 0 || v > (int)VK_COMPARE_OP_ALWAYS) return VK_COMPARE_OP_ALWAYS;
  return (VkCompareOp)v;
}

VkStencilOp ToVkStencilOp(D3D11_STENCIL_OP op)
{
  switch (op)
  {
  case D3D11_STENCIL_OP_KEEP: return VK_STENCIL_OP_KEEP;
  case D3D11_STENCIL_OP_ZERO: return VK_STENCIL_OP_ZERO;
  case D3D11_STENCIL_OP_REPLACE: return VK_STENCIL_OP_REPLACE;
  case D3D11_STENCIL_OP_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
  case D3D11_STENCIL_OP_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
  case D3D11_STENCIL_OP_INVERT: return VK_STENCIL_OP_INVERT;
  case D3D11_STENCIL_OP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
  case D3D11_STENCIL_OP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
  default: return VK_STENCIL_OP_KEEP;
  }
}

VkCullModeFlags ToVkCullMode(D3D11_CULL_MODE m)
{
  switch (m)
  {
  case D3D11_CULL_NONE: return VK_CULL_MODE_NONE;
  case D3D11_CULL_FRONT: return VK_CULL_MODE_FRONT_BIT;
  case D3D11_CULL_BACK: return VK_CULL_MODE_BACK_BIT;
  default: return VK_CULL_MODE_NONE;
  }
}

VkPolygonMode ToVkPolygonMode(D3D11_FILL_MODE m)
{
  switch (m)
  {
  case D3D11_FILL_WIREFRAME: return VK_POLYGON_MODE_LINE;
  case D3D11_FILL_SOLID:
  default: return VK_POLYGON_MODE_FILL;
  }
}

VkPrimitiveTopology ToVkTopology(D3D11_PRIMITIVE_TOPOLOGY t, uint32_t *patchControlPoints)
{
  if (patchControlPoints) *patchControlPoints = 0;
  if (t >= D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST && t <= D3D11_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
  {
    if (patchControlPoints) *patchControlPoints = (uint32_t)(t - D3D11_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST + 1);
    return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  }
  switch (t)
  {
  case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
  case D3D11_PRIMITIVE_TOPOLOGY_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
  case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
  case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
  case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
  case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
  default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
}

VkSamplerAddressMode ToVkAddressMode(D3D11_TEXTURE_ADDRESS_MODE m)
{
  // D3D11 starts at WRAP = 1, Vulkan at REPEAT = 0, and the remaining order
  // matches except that MIRROR_ONCE maps to MIRROR_CLAMP_TO_EDGE.
  switch (m)
  {
  case D3D11_TEXTURE_ADDRESS_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  case D3D11_TEXTURE_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case D3D11_TEXTURE_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case D3D11_TEXTURE_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
  default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

DXGI_FORMAT IndexFormatToDXGI(DXGI_FORMAT f)
{
  if (f == DXGI_FORMAT_R16_UINT || f == DXGI_FORMAT_R32_UINT) return f;
  return DXGI_FORMAT_R16_UINT;
}

//////////////////////////////////////////////////////////////////////////////
// resource inspection

UINT CalcSubresource(UINT mip, UINT layer, UINT mipLevels) { return mip + layer * (mipLevels ? mipLevels : 1); }

TextureImpl *AsTexture(ID3D11Resource *res)
{
  if (!res) return nullptr;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  res->GetType(&dim);
  switch (dim)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D: return &static_cast<Texture1DImpl *>(res)->tex;
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D: return &static_cast<Texture2DImpl *>(res)->tex;
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D: return &static_cast<Texture3DImpl *>(res)->tex;
  default: return nullptr;
  }
}

BufferImpl *AsBuffer(ID3D11Resource *res)
{
  if (!res) return nullptr;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  res->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return nullptr;
  return static_cast<BufferImpl *>(static_cast<ID3D11Buffer *>(res));
}

ID3D11Resource *ViewResource(const ViewImpl &v)
{
  if (v.texture && v.texture->self) return v.texture->self;
  if (v.buffer) return static_cast<ID3D11Resource *>(v.buffer);
  return nullptr;
}

VkImageView CreateTextureView(TextureImpl *tex, VkFormat format, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer,
                              uint32_t layerCount, VkImageViewType type)
{
  if (!tex) return VK_NULL_HANDLE;
  VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  ci.image = tex->image;
  ci.viewType = type;
  ci.format = format;
  ci.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
  ci.subresourceRange.aspectMask = aspect;
  ci.subresourceRange.baseMipLevel = baseMip;
  ci.subresourceRange.levelCount = mipCount ? mipCount : 1;
  ci.subresourceRange.baseArrayLayer = baseLayer;
  ci.subresourceRange.layerCount = layerCount ? layerCount : 1;
  VkImageView view = VK_NULL_HANDLE;
  VkResult r = vkCreateImageView(phxvk::Vk().device, &ci, nullptr, &view);
  if (r != VK_SUCCESS)
  {
    phxvk::LogError("vkCreateImageView failed for %ux%u texture (format %d, type %d): %s", tex->width, tex->height, (int)format, (int)type,
                    phxvk::VkResultName(r));
    return VK_NULL_HANDLE;
  }
  return view;
}

} // namespace phxd3d11

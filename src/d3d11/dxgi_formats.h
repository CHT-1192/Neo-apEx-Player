// DXGI_FORMAT <-> VkFormat conversion for the D3D11-on-Vulkan shim.
//
// D3D11 separates "typeless" storage formats from the format a view uses, and
// it separates the depth format (D24_UNORM_S8_UINT) from the format a shader
// resource view uses to read depth back (R24_UNORM_X8_TYPELESS).  Vulkan has
// one VkFormat per image plus a view format; the helpers below pick the
// storage format for an image and the (possibly different) format for a view.
#pragma once

#include <vulkan/vulkan.h>

// compat/ is on the include path for this target; this is the DXGI_FORMAT
// enum the rest of the shim (and the vendored engine) sees.
#include <dxgiformat.h>

namespace phxd3d11
{

// Storage format for an image/texture created with `fmt`.
VkFormat DXGIFormatToVk(DXGI_FORMAT fmt);
// Storage format for a texture, adjusted to what the device can actually do.
// MoltenVK on Apple silicon supports no 24-bit depth format at all
// (VK_FORMAT_D24_UNORM_S8_UINT and VK_FORMAT_X8_D24_UNORM_PACK32 report zero
// format features), so a D3D11 R24G8_TYPELESS depth buffer - the engine's
// Project.cpp:96 - is substituted with D32_SFLOAT_S8_UINT or D32_SFLOAT.  The
// chosen format is remembered on the texture and every view over it uses it,
// so no image ever gets two different formats.  `needSampled` also requires
// SAMPLED_IMAGE when the texture is bound as a shader resource.
VkFormat DXGIFormatToVkSupported(DXGI_FORMAT fmt, bool needSampled);
// True for the Vk depth / depth-stencil formats this shim maps to.
bool VkFormatIsDepth(VkFormat fmt);
// Format for a depth/stencil *view* over an image whose storage format is
// `imageFormat` and whose view description asks for `viewFormat`.
VkFormat DXGIFormatToVkView(DXGI_FORMAT viewFormat, VkFormat imageFormat, bool depthAspect, bool stencilAspect);
// Best-effort inverse, used by GetDesc()/GetFormat() fallbacks.
DXGI_FORMAT VkFormatToDXGI(VkFormat fmt);

VkSampleCountFlagBits DXGISampleCountToVk(UINT count);

bool DXGIFormatIsDepth(DXGI_FORMAT fmt);
bool DXGIFormatIsStencil(DXGI_FORMAT fmt);
bool DXGIFormatIsSRGB(DXGI_FORMAT fmt);
bool DXGIFormatIsTypeless(DXGI_FORMAT fmt);
bool DXGIFormatIsCompressed(DXGI_FORMAT fmt);
bool DXGIFormatIsInteger(DXGI_FORMAT fmt);

// Bits for one pixel (or one block for block-compressed formats).
UINT DXGIFormatBitsPerPixel(DXGI_FORMAT fmt);
// Block edge in texels: 4 for BC formats, 1 otherwise.
UINT DXGIFormatBlockSize(DXGI_FORMAT fmt);
// Bytes for a tightly packed row of `width` texels.
UINT DXGIFormatRowPitch(DXGI_FORMAT fmt, UINT width);

// Typeless -> the concrete UNORM/FLOAT variant, like the DDS loader does.
DXGI_FORMAT DXGIMakeNotTypeless(DXGI_FORMAT fmt);

} // namespace phxd3d11

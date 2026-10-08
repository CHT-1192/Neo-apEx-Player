// Sampler / blend / depth-stencil / rasterizer states, queries and the debug
// layer object.  State objects are plain descriptions in D3D11; the Vulkan
// objects they translate to are created here (samplers) or at pipeline build
// time (the rest).
#include "d3d11_internal.h"

#include "dxgi_formats.h"

#include <cstdio>

namespace phxd3d11
{

//////////////////////////////////////////////////////////////////////////////
// state objects

#define PHX_STATE_IMPL(Class, Iface)                                                         \
  Class::~Class() { TrackObjectDestroyed(#Class, this); }                                    \
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
  HRESULT Class::QueryInterface(REFIID riid, void **ppvObject)                                \
  {                                                                                          \
    if (!ppvObject) return E_POINTER;                                                        \
    *ppvObject = nullptr;                                                                    \
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(Iface))) \
    {                                                                                        \
      *ppvObject = static_cast<Iface *>(this);                                                \
      AddRefImpl();                                                                          \
      return S_OK;                                                                           \
    }                                                                                        \
    return E_NOINTERFACE;                                                                    \
  }

PHX_STATE_IMPL(BlendStateImpl, ID3D11BlendState);
PHX_STATE_IMPL(DepthStencilStateImpl, ID3D11DepthStencilState);
PHX_STATE_IMPL(RasterizerStateImpl, ID3D11RasterizerState);
#undef PHX_STATE_IMPL

SamplerStateImpl::~SamplerStateImpl()
{
  if (sampler != VK_NULL_HANDLE && phxvk::Vk().device) vkDestroySampler(phxvk::Vk().device, sampler, nullptr);
  sampler = VK_NULL_HANDLE;
  TrackObjectDestroyed("ID3D11SamplerState", this);
}
void SamplerStateImpl::GetDevice(ID3D11Device **ppDevice)
{
  if (ppDevice)
  {
    *ppDevice = device;
    if (device) device->AddRefImpl();
  }
}
HRESULT SamplerStateImpl::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); }
HRESULT SamplerStateImpl::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); }
HRESULT SamplerStateImpl::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); }
HRESULT SamplerStateImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11SamplerState)))
  {
    *ppvObject = static_cast<ID3D11SamplerState *>(this);
    AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}
void SamplerStateImpl::GetDesc(D3D11_SAMPLER_DESC *pDesc) { if (pDesc) *pDesc = desc; }
void BlendStateImpl::GetDesc(D3D11_BLEND_DESC *pDesc) { if (pDesc) *pDesc = desc; }
void DepthStencilStateImpl::GetDesc(D3D11_DEPTH_STENCIL_DESC *pDesc) { if (pDesc) *pDesc = desc; }
void RasterizerStateImpl::GetDesc(D3D11_RASTERIZER_DESC *pDesc) { if (pDesc) *pDesc = desc; }

//////////////////////////////////////////////////////////////////////////////
// queries

#define PHX_QUERY_IMPL(Class, Iface)                                                        \
  Class::~Class() { TrackObjectDestroyed(#Class, this); }                                   \
  void Class::GetDevice(ID3D11Device **ppDevice)                                            \
  {                                                                                         \
    if (ppDevice)                                                                           \
    {                                                                                       \
      *ppDevice = device;                                                                   \
      if (device) device->AddRefImpl();                                                     \
    }                                                                                       \
  }                                                                                         \
  HRESULT Class::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); } \
  void Class::GetDesc(D3D11_QUERY_DESC *pDesc) { if (pDesc) *pDesc = desc; }                 \
  HRESULT Class::QueryInterface(REFIID riid, void **ppvObject)                               \
  {                                                                                         \
    if (!ppvObject) return E_POINTER;                                                       \
    *ppvObject = nullptr;                                                                   \
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11Asynchronous)) || \
        GuidEq(riid, __uuidof(ID3D11Query)) || GuidEq(riid, __uuidof(Iface)))                \
    {                                                                                       \
      *ppvObject = static_cast<Iface *>(this);                                              \
      AddRefImpl();                                                                         \
      return S_OK;                                                                          \
    }                                                                                       \
    return E_NOINTERFACE;                                                                   \
  }

PHX_QUERY_IMPL(QueryImpl, ID3D11Query);
PHX_QUERY_IMPL(PredicateImpl, ID3D11Predicate);
#undef PHX_QUERY_IMPL

UINT QueryImpl::GetDataSize(void)
{
  switch (desc.Query)
  {
  case D3D11_QUERY_OCCLUSION: return sizeof(UINT64);
  case D3D11_QUERY_TIMESTAMP: return sizeof(UINT64);
  case D3D11_QUERY_TIMESTAMP_DISJOINT: return 16; /* D3D11_QUERY_DATA_TIMESTAMP_DISJOINT */
  case D3D11_QUERY_PIPELINE_STATISTICS: return 88; /* D3D11_QUERY_DATA_PIPELINE_STATISTICS */
  case D3D11_QUERY_OCCLUSION_PREDICATE: return sizeof(BOOL);
  default: return sizeof(UINT64);
  }
}

UINT PredicateImpl::GetDataSize(void)
{
  switch (desc.Query)
  {
  case D3D11_QUERY_OCCLUSION_PREDICATE: return sizeof(BOOL);
  default: return sizeof(UINT64);
  }
}

//////////////////////////////////////////////////////////////////////////////
// device: state objects

HRESULT DeviceImpl::CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState)
{
  if (!ppSamplerState) return E_POINTER;
  *ppSamplerState = nullptr;
  if (!pSamplerDesc) return E_INVALIDARG;

  auto *s = new SamplerStateImpl();
  s->device = this;
  s->desc = *pSamplerDesc;

  const D3D11_FILTER f = pSamplerDesc->Filter;
  VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  ci.magFilter = (f & 0x4) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  ci.minFilter = (f & 0x10) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  ci.mipmapMode = (f & 0x1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  const bool comparison = ((f >> 7) & 0x3) == 1;
  const bool reduction = ((f >> 7) & 0x3) >= 2;
  ci.addressModeU = ToVkAddressMode(pSamplerDesc->AddressU);
  ci.addressModeV = ToVkAddressMode(pSamplerDesc->AddressV);
  ci.addressModeW = ToVkAddressMode(pSamplerDesc->AddressW);
  ci.mipLodBias = pSamplerDesc->MipLODBias;
  ci.anisotropyEnable = (f & 0x40) ? VK_TRUE : VK_FALSE;
  ci.maxAnisotropy = pSamplerDesc->MaxAnisotropy ? (float)pSamplerDesc->MaxAnisotropy : 1.0f;
  ci.compareEnable = comparison ? VK_TRUE : VK_FALSE;
  ci.compareOp = ToVkCompareOp(pSamplerDesc->ComparisonFunc);
  ci.minLod = pSamplerDesc->MinLOD;
  ci.maxLod = pSamplerDesc->MaxLOD;
  ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  ci.unnormalizedCoordinates = VK_FALSE;

  // D3D's border colour is a float4; Vulkan only offers a few presets plus the
  // custom-border-colour feature, so map the common cases and warn otherwise.
  const FLOAT *bc = pSamplerDesc->BorderColor;
  if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 0)
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  else if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 1)
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  else if (bc[0] == 1 && bc[1] == 1 && bc[2] == 1 && bc[3] == 1)
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  else
    phxvk::LogWarn("CreateSamplerState: border colour (%.2f %.2f %.2f %.2f) is not one of the Vulkan presets; using transparent black", bc[0], bc[1], bc[2], bc[3]);

  VkSamplerReductionModeCreateInfo reductionInfo{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
  if (reduction)
  {
    reductionInfo.reductionMode = ((f >> 7) & 0x3) == 2 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
    ci.pNext = &reductionInfo;
    if (OnceReductionMode())
      phxvk::LogWarn("[d3d11] minimum/maximum texture filters map to VkSamplerReductionMode (VK_EXT_sampler_filter_minmax on Vulkan 1.1)");
  }

  VkResult res = vkCreateSampler(phxvk::Vk().device, &ci, nullptr, &s->sampler);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("vkCreateSampler failed: %s", phxvk::VkResultName(res));
    delete s;
    return E_FAIL;
  }

  TrackObjectCreated("ID3D11SamplerState", s);
  *ppSamplerState = s;
  return S_OK;
}

HRESULT DeviceImpl::CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc, ID3D11BlendState **ppBlendState)
{
  if (!ppBlendState) return E_POINTER;
  *ppBlendState = nullptr;
  if (!pBlendStateDesc) return E_INVALIDARG;
  auto *s = new BlendStateImpl();
  s->device = this;
  s->desc = *pBlendStateDesc;
  TrackObjectCreated("ID3D11BlendState", s);
  *ppBlendState = s;
  return S_OK;
}

HRESULT DeviceImpl::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc, ID3D11DepthStencilState **ppDepthStencilState)
{
  if (!ppDepthStencilState) return E_POINTER;
  *ppDepthStencilState = nullptr;
  if (!pDepthStencilDesc) return E_INVALIDARG;
  auto *s = new DepthStencilStateImpl();
  s->device = this;
  s->desc = *pDepthStencilDesc;
  TrackObjectCreated("ID3D11DepthStencilState", s);
  *ppDepthStencilState = s;
  return S_OK;
}

HRESULT DeviceImpl::CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc, ID3D11RasterizerState **ppRasterizerState)
{
  if (!ppRasterizerState) return E_POINTER;
  *ppRasterizerState = nullptr;
  if (!pRasterizerDesc) return E_INVALIDARG;
  auto *s = new RasterizerStateImpl();
  s->device = this;
  s->desc = *pRasterizerDesc;
  TrackObjectCreated("ID3D11RasterizerState", s);
  *ppRasterizerState = s;
  return S_OK;
}

HRESULT DeviceImpl::CreateQuery(const D3D11_QUERY_DESC *pQueryDesc, ID3D11Query **ppQuery)
{
  if (!ppQuery) return E_POINTER;
  *ppQuery = nullptr;
  if (!pQueryDesc) return E_INVALIDARG;
  auto *q = new QueryImpl();
  q->device = this;
  q->desc = *pQueryDesc;
  TrackObjectCreated("ID3D11Query", q);
  *ppQuery = q;
  return S_OK;
}

HRESULT DeviceImpl::CreatePredicate(const D3D11_QUERY_DESC *pPredicateDesc, ID3D11Predicate **ppPredicate)
{
  if (!ppPredicate) return E_POINTER;
  *ppPredicate = nullptr;
  if (!pPredicateDesc) return E_INVALIDARG;
  auto *q = new PredicateImpl();
  q->device = this;
  q->desc = *pPredicateDesc;
  TrackObjectCreated("ID3D11Predicate", q);
  *ppPredicate = q;
  return S_OK;
}

//////////////////////////////////////////////////////////////////////////////
// debug layer

DebugImpl::~DebugImpl() { TrackObjectDestroyed("ID3D11Debug", this); }
HRESULT DebugImpl::SetFeatureMask(UINT) { return S_OK; }
UINT DebugImpl::GetFeatureMask(void) { return 0; }
HRESULT DebugImpl::SetPresentPerRenderOpDelay(UINT) { return S_OK; }
UINT DebugImpl::GetPresentPerRenderOpDelay(void) { return 0; }
HRESULT DebugImpl::SetSwapChain(IDXGISwapChain *) { return S_OK; }
HRESULT DebugImpl::GetSwapChain(IDXGISwapChain **ppSwapChain)
{
  if (!ppSwapChain) return E_POINTER;
  *ppSwapChain = device ? device->swapChain : nullptr;
  if (*ppSwapChain) (*ppSwapChain)->AddRef();
  return S_OK;
}
HRESULT DebugImpl::ReportLiveDeviceObjects(D3D11_RLDO_FLAGS Flags)
{
  // The engine calls this from CCoreDX11Device::~CCoreDX11Device to dump leaked
  // objects; the shim keeps its own list.
  ReportLiveObjects(Flags == D3D11_RLDO_DETAIL);
  return S_OK;
}
HRESULT DebugImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11Debug)))
  {
    *ppvObject = static_cast<ID3D11Debug *>(this);
    AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}

} // namespace phxd3d11

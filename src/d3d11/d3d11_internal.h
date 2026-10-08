// Private internals of the D3D11-on-Vulkan shim.
//
// Nothing here is part of the public surface: the engine only ever sees the
// ID3D11*/IDXGI* interfaces from compat/.  This header declares the objects
// that implement them and the shared helpers they use.
#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <vulkan/vulkan.h>

#include "../rhi/shader_compiler.h"
#include "../rhi/vk_core.h"
#include "vk_swapchain.h"

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace phxd3d11
{

// ===========================================================================
// small helpers
// ===========================================================================

inline bool GuidEq(REFGUID a, REFGUID b) { return memcmp(&a, &b, sizeof(GUID)) == 0; }

// compat/D3Dcommon.h provides __uuidof() accessors for the D3D interfaces but
// not for IUnknown itself; this is the real IID_IUnknown.
inline const GUID &IID_IUnknownRef()
{
  static const GUID g = {0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
  return g;
}

// One-time "log this only once" gate.
struct Once
{
  bool done = false;
  bool operator()()
  {
    if (done) return false;
    done = true;
    return true;
  }
  // So that both `if (OnceFoo())` and `cond && OnceFoo()` mean "first time?".
  operator bool() { return (*this)(); }
};
Once &OnceGsUnsupported();
// Pipelines whose geometry shader stage was dropped (device has no GS).
extern unsigned g_gsSkippedPipelines;
Once &OnceFullscreen();
Once &OnceCompressedUpload();
Once &OnceReductionMode();

const char *HResultName(HRESULT hr);

// Reflection carried alongside a compiled shader (see d3d11_shader.cpp).
struct ShaderReflectionRecord
{
  phxvk::Reflection reflection;
  phxvk::ShaderStage stage = phxvk::ShaderStage::Vertex;
  // Set for geometry stages: the shape of the shader the device cannot run.
  phxvk::GeometryShaderInfo geometry;
};
// Registers the reflection of a compiled blob, keyed by its SPIR-V bytes, so
// that CreateVertexShader()/D3DReflect() can recover it from a plain SPIR-V
// blob (the design requires the blob payload to be raw SPIR-V).
void RegisterShaderReflection(const void *spirv, size_t size, const ShaderReflectionRecord &rec);

// The vertex stage that stands in for a geometry shader Metal cannot run,
// keyed by that geometry shader's SPIR-V.  See src/rhi/gs_emulation.h.
void RegisterEmulatedVertexShader(const void *gsSpirv, size_t gsSize, const std::vector<uint32_t> &vsSpirv);
bool LookupEmulatedVertexShader(const void *gsSpirv, size_t gsSize, std::vector<uint32_t> *out);
bool LookupShaderReflection(const void *spirv, size_t size, ShaderReflectionRecord *out);

// Blobs (d3d11_blob.cpp).
ID3DBlob *CreateBlob(const void *data, size_t size);
ID3DBlob *CreateBlobString(const std::string &text);

// Shader reflection object (d3d11_reflection.cpp).
ID3D11ShaderReflection *CreateReflection(const ShaderReflectionRecord &rec, const char *debugName);

// Live-object bookkeeping for ID3D11Debug.
void TrackObjectCreated(const char *kind, void *object);
void TrackObjectDestroyed(const char *kind, void *object);
void ReportLiveObjects(bool detail);

// ===========================================================================
// COM plumbing
// ===========================================================================

struct ObjectBase
{
  std::atomic<ULONG> refCount{1};
  ULONG AddRefImpl() { return ++refCount; }
  ULONG ReleaseImpl()
  {
    ULONG n = --refCount;
    if (n == 0) delete this;
    return n;
  }

  struct PrivateData
  {
    GUID guid{};
    std::vector<uint8_t> data;
    IUnknown *iface = nullptr;
  };
  std::vector<PrivateData> privateData;

  HRESULT GetPrivateDataImpl(REFGUID guid, UINT *size, void *data)
  {
    for (auto &p : privateData)
    {
      if (!GuidEq(p.guid, guid)) continue;
      if (size) *size = (UINT)p.data.size();
      if (data) memcpy(data, p.data.data(), p.data.size());
      return S_OK;
    }
    return E_INVALIDARG;
  }
  HRESULT SetPrivateDataImpl(REFGUID guid, UINT size, const void *data)
  {
    for (auto &p : privateData)
    {
      if (!GuidEq(p.guid, guid)) continue;
      p.data.assign((const uint8_t *)data, (const uint8_t *)data + size);
      return S_OK;
    }
    PrivateData p;
    p.guid = guid;
    p.data.assign((const uint8_t *)data, (const uint8_t *)data + size);
    privateData.push_back(std::move(p));
    return S_OK;
  }
  HRESULT SetPrivateDataInterfaceImpl(REFGUID guid, const IUnknown *iface)
  {
    for (auto &p : privateData)
    {
      if (!GuidEq(p.guid, guid)) continue;
      p.iface = const_cast<IUnknown *>(iface);
      return S_OK;
    }
    PrivateData p;
    p.guid = guid;
    p.iface = const_cast<IUnknown *>(iface);
    privateData.push_back(std::move(p));
    return S_OK;
  }

  virtual ~ObjectBase() = default;
};

struct DeviceImpl;
struct ContextImpl;

// ===========================================================================
// resources
// ===========================================================================

struct BufferImpl : public ID3D11Buffer, public ObjectBase
{
  DeviceImpl *device = nullptr;
  D3D11_BUFFER_DESC desc{};
  VkBuffer buffer = VK_NULL_HANDLE; // non-dynamic buffers
  VkDeviceMemory memory = VK_NULL_HANDLE;
  phxvk::RingAlloc ring; // dynamic buffers: current allocation
  bool dynamic = false;
  bool isStaging = false;
  void *mappedPtr = nullptr;
  bool mapped = false;

  // CPU copy of the contents, kept for vertex and index buffers.  The geometry
  // shader emulation has to read the original vertices and indices back (see
  // src/rhi/gs_emulation.h), and D3D11's own contract gives no way to read a
  // buffer the GPU owns.  Empty for buffers the emulation never needs.
  std::vector<uint8_t> shadow;
  bool wantsShadow() const
  {
    return !isStaging && (desc.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) != 0;
  }

  // A dynamic resource is renamed by Map(WRITE_DISCARD); these are the
  // buffer+offset a descriptor must use right now.
  VkBuffer boundBuffer() const { return dynamic ? ring.buffer : buffer; }
  uint64_t boundOffset() const { return dynamic ? ring.offset : 0; }
  uint64_t size() const { return desc.ByteWidth; }

  void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;
  void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override;
  void STDMETHODCALLTYPE SetEvictionPriority(UINT) override;
  UINT STDMETHODCALLTYPE GetEvictionPriority(void) override;
  void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ~BufferImpl() override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }
};

// A D3D11 1D/2D/3D texture (and the swap chain back buffer, which is the same
// record with a swap-chain-backed VkImage).
struct TextureImpl
{
  ID3D11Resource *self = nullptr; // the texture interface that owns this record
  D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
  UINT width = 1;
  UINT height = 1;
  UINT depth = 1;
  UINT mipLevels = 1;
  UINT arraySize = 1;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT sampleCount = 1;
  UINT sampleQuality = 0;
  UINT bindFlags = 0;
  UINT miscFlags = 0;
  UINT cpuAccessFlags = 0;
  D3D11_USAGE usage = D3D11_USAGE_DEFAULT;

  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // The VkFormat the image was created with.  For depth textures this can
  // differ from DXGIFormatToVk(format) because the device may not support the
  // D3D11 depth format (R3); every view over the image must use this format.
  VkFormat storageFormat = VK_FORMAT_UNDEFINED;

  // USAGE_STAGING textures are backed by a host visible VkBuffer: D3D11 only
  // ever Map()s or CopyResource()s into them, and MoltenVK cannot map a
  // linear VkImage, so a buffer is both cheaper and portable.
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
  uint64_t stagingSize = 0;
  UINT stagingRowPitch = 0;
  bool mapped = false;

  // Swap chain back buffers re-resolve their image/view every frame.
  Swapchain *swapchain = nullptr;

  VkImage vkImage() const { return swapchain ? swapchain->CurrentImage() : image; }
  VkImageView vkSwapchainView() const { return swapchain ? swapchain->CurrentView() : VK_NULL_HANDLE; }
  VkImageLayout currentLayout() const { return swapchain ? swapchain->LayoutAt(swapchain->CurrentIndex()) : layout; }
  void setCurrentLayout(VkImageLayout l)
  {
    if (swapchain) swapchain->SetLayoutAt(swapchain->CurrentIndex(), l);
    else layout = l;
  }
  bool isStaging() const { return staging != VK_NULL_HANDLE; }
};

#define PHX_TEXTURE_OBJECT(Class, Iface, DescType)                                           \
  struct Class : public Iface, public ObjectBase                                             \
  {                                                                                          \
    TextureImpl tex;                                                                         \
    DeviceImpl *device = nullptr;                                                            \
    Class();                                                                                 \
    ~Class() override;                                                                       \
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;                       \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;               \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;           \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;    \
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override;    \
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override;                                \
    UINT STDMETHODCALLTYPE GetEvictionPriority(void) override;                                \
    void STDMETHODCALLTYPE GetDesc(DescType *pDesc) override;                                 \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;         \
    ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }                    \
    ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }                  \
  }

PHX_TEXTURE_OBJECT(Texture1DImpl, ID3D11Texture1D, D3D11_TEXTURE1D_DESC);
PHX_TEXTURE_OBJECT(Texture2DImpl, ID3D11Texture2D, D3D11_TEXTURE2D_DESC);
PHX_TEXTURE_OBJECT(Texture3DImpl, ID3D11Texture3D, D3D11_TEXTURE3D_DESC);
#undef PHX_TEXTURE_OBJECT

struct ViewImpl
{
  TextureImpl *texture = nullptr; // one of these two is set
  BufferImpl *buffer = nullptr;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
  uint32_t baseMip = 0;
  uint32_t mipCount = 1;
  uint32_t baseLayer = 0;
  uint32_t layerCount = 1;
  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D;
  bool isDepth = false;
  bool isStencil = false;
  // Swap chain back buffer views: the VkImageView changes every frame.
  bool dynamicSwapchainView = false;

  VkImageView vkView() const { return dynamicSwapchainView && texture ? texture->vkSwapchainView() : view; }
};

#define PHX_VIEW_OBJECT(Class, Iface, DescType)                                             \
  struct Class : public Iface, public ObjectBase                                            \
  {                                                                                         \
    ViewImpl v;                                                                             \
    DescType desc{};                                                                        \
    DeviceImpl *device = nullptr;                                                            \
    /* Identity for the pipeline cache; the DESC is plain data, so hashing it makes  */      \
    /* two equivalent state objects share one pipeline instead of leaking one each. */      \
    uint64_t contentHash = 0;                                                               \
    ~Class() override;                                                                      \
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;                      \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;              \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;          \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;   \
    void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override;                \
    void STDMETHODCALLTYPE GetDesc(DescType *pDesc) override;                                \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;        \
    ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }                   \
    ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }                 \
  }

PHX_VIEW_OBJECT(SRVImpl, ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC);
PHX_VIEW_OBJECT(RTVImpl, ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC);
PHX_VIEW_OBJECT(DSVImpl, ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC);
PHX_VIEW_OBJECT(UAVImpl, ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC);
#undef PHX_VIEW_OBJECT

// ===========================================================================
// shaders / input layout
// ===========================================================================

// FNV-1a over raw bytes.  Used to give shaders and state objects a stable
// identity for the pipeline cache: see PipelineKey.
inline uint64_t PhxHashBytes(const void *data, size_t n, uint64_t seed = 1469598103934665603ull)
{
  const unsigned char *p = (const unsigned char *)data;
  uint64_t h = seed;
  for (size_t i = 0; i < n; i++)
  {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
inline uint64_t PhxHashCombine(uint64_t h, uint64_t v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h;
}

struct ShaderCommon
{
  phxvk::ShaderStage stage = phxvk::ShaderStage::Vertex;
  VkShaderModule module = VK_NULL_HANDLE;
  // Hash of the SPIR-V.  The pipeline cache keys on this rather than on
  // `module`, because the engine recreates shaders with identical bytecode (a
  // shadertoy material is rebuilt every frame) and each rebuild used to mint a
  // brand new VkShaderModule - and therefore a brand new pipeline that was never
  // released.  See PipelineKey.
  uint64_t contentHash = 0;
  phxvk::Reflection reflection;
  std::vector<uint32_t> spirv;
  std::string debugName;
  // glslang names the SPIR-V entry point after the HLSL function ("u", "a",
  // ...), which D3D11's Create*Shader does not tell us, so it is parsed out of
  // the module.
  std::string entryPoint = "main";
};

#define PHX_SHADER_OBJECT(Class, Iface)                                                     \
  struct Class : public Iface, public ObjectBase, public ShaderCommon                       \
  {                                                                                         \
    DeviceImpl *device = nullptr;                                                           \
    ~Class() override;                                                                      \
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;                      \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;              \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;          \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;   \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;        \
    ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }                   \
    ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }                 \
  }

PHX_SHADER_OBJECT(VertexShaderImpl, ID3D11VertexShader);
PHX_SHADER_OBJECT(PixelShaderImpl, ID3D11PixelShader);
PHX_SHADER_OBJECT(GeometryShaderImpl, ID3D11GeometryShader);
PHX_SHADER_OBJECT(HullShaderImpl, ID3D11HullShader);
PHX_SHADER_OBJECT(DomainShaderImpl, ID3D11DomainShader);
PHX_SHADER_OBJECT(ComputeShaderImpl, ID3D11ComputeShader);
#undef PHX_SHADER_OBJECT

struct InputLayoutImpl : public ID3D11InputLayout, public ObjectBase
{
  DeviceImpl *device = nullptr;
  std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
  std::vector<std::string> names; // backs elements[].SemanticName
  phxvk::Reflection vertexReflection;
  bool reflectionValid = false;
  uint64_t contentHash = 0;

  ~InputLayoutImpl() override;
  void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }
};

// ===========================================================================
// state objects
// ===========================================================================

#define PHX_STATE_OBJECT(Class, Iface, DescType)                                            \
  struct Class : public Iface, public ObjectBase                                            \
  {                                                                                         \
    DescType desc{};                                                                        \
    DeviceImpl *device = nullptr;                                                            \
    /* Identity for the pipeline cache; the DESC is plain data, so hashing it makes  */      \
    /* two equivalent state objects share one pipeline instead of leaking one each. */      \
    uint64_t contentHash = 0;                                                               \
    ~Class() override;                                                                      \
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;                      \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;              \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;          \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;   \
    void STDMETHODCALLTYPE GetDesc(DescType *pDesc) override;                                \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;        \
    ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }                   \
    ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }                 \
  }

PHX_STATE_OBJECT(BlendStateImpl, ID3D11BlendState, D3D11_BLEND_DESC);
PHX_STATE_OBJECT(DepthStencilStateImpl, ID3D11DepthStencilState, D3D11_DEPTH_STENCIL_DESC);
PHX_STATE_OBJECT(RasterizerStateImpl, ID3D11RasterizerState, D3D11_RASTERIZER_DESC);
#undef PHX_STATE_OBJECT

// SamplerStateImpl needs a VkSampler on top of the description, so it is
// spelled out rather than generated by the macro above.
struct SamplerStateImpl : public ID3D11SamplerState, public ObjectBase
{
  D3D11_SAMPLER_DESC desc{};
  VkSampler sampler = VK_NULL_HANDLE;
  DeviceImpl *device = nullptr;

  ~SamplerStateImpl() override;
  void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;
  void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }
};

// ===========================================================================
// queries
// ===========================================================================

#define PHX_QUERY_OBJECT(Class, Iface)                                                       \
  struct Class : public Iface, public ObjectBase                                             \
  {                                                                                          \
    DeviceImpl *device = nullptr;                                                            \
    D3D11_QUERY_DESC desc{};                                                                 \
    bool begun = false;                                                                      \
    ~Class() override;                                                                       \
    void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;                       \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;               \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;           \
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;    \
    UINT STDMETHODCALLTYPE GetDataSize(void) override;                                        \
    void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC *pDesc) override;                         \
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;         \
    ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }                    \
    ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }                  \
  }

PHX_QUERY_OBJECT(QueryImpl, ID3D11Query);
PHX_QUERY_OBJECT(PredicateImpl, ID3D11Predicate);
#undef PHX_QUERY_OBJECT

// ID3D11Debug: the engine only creates it and calls ReportLiveDeviceObjects().
struct DebugImpl : public ID3D11Debug, public ObjectBase
{
  DeviceImpl *device = nullptr;
  ~DebugImpl() override;
  HRESULT STDMETHODCALLTYPE SetFeatureMask(UINT Mask) override;
  UINT STDMETHODCALLTYPE GetFeatureMask(void) override;
  HRESULT STDMETHODCALLTYPE SetPresentPerRenderOpDelay(UINT Milliseconds) override;
  UINT STDMETHODCALLTYPE GetPresentPerRenderOpDelay(void) override;
  HRESULT STDMETHODCALLTYPE SetSwapChain(IDXGISwapChain *pSwapChain) override;
  HRESULT STDMETHODCALLTYPE GetSwapChain(IDXGISwapChain **ppSwapChain) override;
  HRESULT STDMETHODCALLTYPE ReportLiveDeviceObjects(D3D11_RLDO_FLAGS Flags) override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }
};

// ===========================================================================
// swap chain
// ===========================================================================

class SwapChainImpl : public IDXGISwapChain, public ObjectBase
{
public:
  ~SwapChainImpl() override;

  bool Initialize(DeviceImpl *device, HWND outputWindow, const DXGI_SWAP_CHAIN_DESC &desc, ContextImpl *context);
  void Shutdown();

  // Called by the context when it starts/finishes a frame.
  bool BeginFrame();
  HRESULT PresentFrame(UINT syncInterval, UINT flags);
  void OnBackBufferChanged();

  Swapchain &chain() { return m_chain; }
  ID3D11Texture2D *backBufferTexture() { return m_backBuffer; }
  ID3D11RenderTargetView *backBufferView() { return m_backBufferRTV; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }
  HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void **ppSurface) override;
  HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) override;
  HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *pFullscreen, IDXGIOutput **ppTarget) override;
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
  HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) override;

private:
  bool CreateBackBufferObjects();
  // Releases the swap chain's own back-buffer render target view.  It is only
  // valid while the images it was made from are, so a resize drops it - but the
  // Texture2DImpl behind it is kept, see d3d11_swapchain.cpp.
  void DestroyBackBufferRTV();
  // Shutdown only: the swap chain is going away, so the back-buffer wrapper can
  // go with it.
  void DestroyBackBufferObjects();

  DeviceImpl *m_device = nullptr;
  ContextImpl *m_context = nullptr;
  HWND m_window = nullptr;
  Swapchain m_chain;
  ID3D11Texture2D *m_backBuffer = nullptr; // swap-chain backed Texture2DImpl
  ID3D11RenderTargetView *m_backBufferRTV = nullptr;
};

// ===========================================================================
// device
// ===========================================================================

struct DeviceImpl : public ID3D11Device, public ObjectBase
{
  ContextImpl *context = nullptr;
  DebugImpl *debug = nullptr;
  IDXGISwapChain *swapChain = nullptr;
  D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
  UINT creationFlags = 0;
  bool removed = false;

  ~DeviceImpl() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override;
  HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **) override;
  HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture1D **) override;
  HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **) override;
  HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture3D **) override;
  HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource *, const D3D11_SHADER_RESOURCE_VIEW_DESC *, ID3D11ShaderResourceView **) override;
  HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource *, const D3D11_UNORDERED_ACCESS_VIEW_DESC *, ID3D11UnorderedAccessView **) override;
  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource *, const D3D11_RENDER_TARGET_VIEW_DESC *, ID3D11RenderTargetView **) override;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource *, const D3D11_DEPTH_STENCIL_VIEW_DESC *, ID3D11DepthStencilView **) override;
  HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *, UINT, const void *, SIZE_T, ID3D11InputLayout **) override;
  HRESULT STDMETHODCALLTYPE CreateVertexShader(const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11VertexShader **) override;
  HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11GeometryShader **) override;
  HRESULT STDMETHODCALLTYPE CreatePixelShader(const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11PixelShader **) override;
  HRESULT STDMETHODCALLTYPE CreateHullShader(const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11HullShader **) override;
  HRESULT STDMETHODCALLTYPE CreateDomainShader(const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11DomainShader **) override;
  HRESULT STDMETHODCALLTYPE CreateComputeShader(const void *, SIZE_T, ID3D11ClassLinkage *, ID3D11ComputeShader **) override;
  HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC *, ID3D11SamplerState **) override;
  HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC *, ID3D11BlendState **) override;
  HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *, ID3D11DepthStencilState **) override;
  HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC *, ID3D11RasterizerState **) override;
  HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC *, ID3D11Query **) override;
  HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC *, ID3D11Predicate **) override;
  D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel(void) override;
  HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport) override;
  void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext **ppImmediateContext) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void *pData) override;
};

// ===========================================================================
// device context
// ===========================================================================

constexpr UINT kMaxConstantBuffers = 16;
constexpr UINT kMaxShaderResources = 128;
constexpr UINT kMaxSamplers = 16;
constexpr UINT kMaxUAVs = 16;
constexpr UINT kMaxRenderTargets = 8;

enum StageIndex
{
  kStageVS = 0,
  kStagePS,
  kStageGS,
  kStageHS,
  kStageDS,
  kStageCS,
  kStageCount,
};

struct StageState
{
  ID3D11Buffer *cb[kMaxConstantBuffers] = {};
  ID3D11ShaderResourceView *srv[kMaxShaderResources] = {};
  ID3D11SamplerState *samplers[kMaxSamplers] = {};
  ID3D11UnorderedAccessView *uav[kMaxUAVs] = {};
  ShaderCommon *shader = nullptr;
};

// One descriptor binding after merging every bound stage's reflection.
struct MergedBinding
{
  phxvk::DescriptorBinding desc;
  VkShaderStageFlags stages = 0;
};

// Keyed on CONTENT, not identity.
//
// These used to be raw pointers (and VkShaderModule handles).  The engine throws
// its state objects and shaders away and rebuilds them - per frame, for a
// shadertoy material - so every frame produced a key that had never been seen,
// the pipeline map only ever grew, and each entry is a compiled Metal pipeline.
// By the time the demo reached frame 6000 that was ~28 seconds per frame with
// the machine swapping; hashing the content makes a rebuild a cache hit.
struct PipelineKey
{
  uint64_t vs = 0, ps = 0, hs = 0, ds = 0;      // SPIR-V content hashes
  uint64_t layout = 0;                           // input layout content hash
  uint64_t blend = 0, depthStencil = 0, rasterizer = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  VkFormat color[8] = {};
  UINT colorCount = 0;
  VkFormat depth = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  UINT sampleMask = 0xffffffff;
  bool operator<(const PipelineKey &o) const
  {
    if (vs != o.vs) return vs < o.vs;
    if (ps != o.ps) return ps < o.ps;
    if (hs != o.hs) return hs < o.hs;
    if (ds != o.ds) return ds < o.ds;
    if (layout != o.layout) return layout < o.layout;
    if (blend != o.blend) return blend < o.blend;
    if (depthStencil != o.depthStencil) return depthStencil < o.depthStencil;
    if (rasterizer != o.rasterizer) return rasterizer < o.rasterizer;
    if (topology != o.topology) return topology < o.topology;
    if (colorCount != o.colorCount) return colorCount < o.colorCount;
    for (UINT i = 0; i < colorCount; i++)
      if (color[i] != o.color[i]) return color[i] < o.color[i];
    if (depth != o.depth) return depth < o.depth;
    if (samples != o.samples) return samples < o.samples;
    return sampleMask < o.sampleMask;
  }
};

struct ContextImpl : public ID3D11DeviceContext, public ObjectBase
{
  DeviceImpl *device = nullptr;
  SwapChainImpl *swapChain = nullptr;

  StageState stages[kStageCount];

  ID3D11RenderTargetView *rtv[kMaxRenderTargets] = {};
  UINT rtvCount = 0;
  ID3D11DepthStencilView *dsv = nullptr;
  ID3D11UnorderedAccessView *omUav[kMaxUAVs] = {};
  UINT omUavStart = 0;
  UINT omUavCount = 0;

  ID3D11InputLayout *inputLayout = nullptr;
  ID3D11Buffer *vertexBuffers[16] = {};
  UINT vertexStrides[16] = {};
  UINT vertexOffsets[16] = {};
  UINT vertexBufferCount = 0;
  ID3D11Buffer *indexBuffer = nullptr;
  DXGI_FORMAT indexFormat = DXGI_FORMAT_R16_UINT;
  UINT indexOffset = 0;
  D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

  D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
  bool viewportSet = false;
  D3D11_RECT scissor{0, 0, 0, 0};
  bool scissorSet = false;

  BlendStateImpl *blendState = nullptr;
  FLOAT blendFactor[4] = {1, 1, 1, 1};
  UINT sampleMask = 0xffffffff;
  DepthStencilStateImpl *depthStencilState = nullptr;
  UINT stencilRef = 0;
  RasterizerStateImpl *rasterizerState = nullptr;
  ID3D11Predicate *predication = nullptr;
  BOOL predicationValue = FALSE;

  // ---- Vulkan plumbing ----
  std::map<std::vector<uint32_t>, VkDescriptorSetLayout> setLayouts;
  std::map<VkDescriptorSetLayout, VkPipelineLayout> pipelineLayouts;
  std::map<PipelineKey, VkPipeline> pipelines;
  std::vector<VkDescriptorPool> descriptorPools;
  VkDescriptorSetLayout lastSetLayout = VK_NULL_HANDLE;
  int frameParity = 0;
  uint64_t lastFrameIndex = ~0ull;
  bool renderingActive = false;
  bool poolsCleared = false;

  VkBuffer dummyBuffer = VK_NULL_HANDLE;
  VkDeviceMemory dummyBufferMemory = VK_NULL_HANDLE;
  VkImage dummyImage = VK_NULL_HANDLE;
  VkDeviceMemory dummyImageMemory = VK_NULL_HANDLE;
  VkImageView dummyImageView = VK_NULL_HANDLE;
  VkSampler dummySampler = VK_NULL_HANDLE;
  bool dummiesReady = false;

  ~ContextImpl() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override;

  void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT *, void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void *) override;
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown *) override;

  void STDMETHODCALLTYPE IASetInputLayout(ID3D11InputLayout *pInputLayout) override;
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppVertexBuffers, const UINT *pStrides, const UINT *pOffsets) override;
  void STDMETHODCALLTYPE IASetIndexBuffer(ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override;
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override;
  void STDMETHODCALLTYPE IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY *pTopology) override;
  // State getters.  D3D11 requires these; the engine never reads its own state
  // back, so they were an unimplemented corner until Dear ImGui's DX11 backend
  // needed them to save and restore the pipeline around its own drawing.
  void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout **ppInputLayout) override;
  void STDMETHODCALLTYPE IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppVertexBuffers, UINT *pStrides, UINT *pOffsets) override;
  void STDMETHODCALLTYPE IAGetIndexBuffer(ID3D11Buffer **pIndexBuffer, DXGI_FORMAT *Format, UINT *Offset) override;
  void STDMETHODCALLTYPE VSGetShader(ID3D11VertexShader **ppVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT *pNumClassInstances) override;
  void STDMETHODCALLTYPE PSGetShader(ID3D11PixelShader **ppPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT *pNumClassInstances) override;
  void STDMETHODCALLTYPE GSGetShader(ID3D11GeometryShader **ppGeometryShader, ID3D11ClassInstance *const *ppClassInstances, UINT *pNumClassInstances) override;
  void STDMETHODCALLTYPE VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override;
  void STDMETHODCALLTYPE PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override;
  void STDMETHODCALLTYPE PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override;
  void STDMETHODCALLTYPE RSGetViewports(UINT *pNumViewports, D3D11_VIEWPORT *pViewports) override;
  void STDMETHODCALLTYPE RSGetScissorRects(UINT *pNumRects, D3D11_RECT *pRects) override;
  void STDMETHODCALLTYPE RSGetState(ID3D11RasterizerState **ppRasterizerState) override;
  void STDMETHODCALLTYPE OMGetBlendState(ID3D11BlendState **ppBlendState, FLOAT BlendFactor[4], UINT *pSampleMask) override;
  void STDMETHODCALLTYPE OMGetDepthStencilState(ID3D11DepthStencilState **ppDepthStencilState, UINT *pStencilRef) override;

  void STDMETHODCALLTYPE VSSetShader(ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) override;
  void STDMETHODCALLTYPE VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override;
  void STDMETHODCALLTYPE VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
  void STDMETHODCALLTYPE VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override;
  void STDMETHODCALLTYPE VSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) override;

  void STDMETHODCALLTYPE HSSetShader(ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) override;
  void STDMETHODCALLTYPE HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override;
  void STDMETHODCALLTYPE HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
  void STDMETHODCALLTYPE HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override;

  void STDMETHODCALLTYPE DSSetShader(ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) override;
  void STDMETHODCALLTYPE DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override;
  void STDMETHODCALLTYPE DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
  void STDMETHODCALLTYPE DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override;

  void STDMETHODCALLTYPE GSSetShader(ID3D11GeometryShader *pShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) override;
  void STDMETHODCALLTYPE GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override;
  void STDMETHODCALLTYPE GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
  void STDMETHODCALLTYPE GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override;

  void STDMETHODCALLTYPE SOSetTargets(UINT NumBuffers, ID3D11Buffer *const *ppSOTargets, const UINT *pOffsets) override;

  void STDMETHODCALLTYPE Draw(UINT VertexCount, UINT StartVertexLocation) override;
  void STDMETHODCALLTYPE DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override;
  void STDMETHODCALLTYPE DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation) override;
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation) override;
  void STDMETHODCALLTYPE DrawAuto(void) override;

  void STDMETHODCALLTYPE Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override;
  void STDMETHODCALLTYPE PSSetShader(ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) override;
  void STDMETHODCALLTYPE PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override;
  void STDMETHODCALLTYPE PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
  void STDMETHODCALLTYPE PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override;
  void STDMETHODCALLTYPE PSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) override;

  void STDMETHODCALLTYPE CSSetShader(ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) override;
  void STDMETHODCALLTYPE CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override;
  void STDMETHODCALLTYPE CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override;
  void STDMETHODCALLTYPE CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts) override;
  void STDMETHODCALLTYPE CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override;

  void STDMETHODCALLTYPE OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView) override;
  void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView,
                                                                  UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
                                                                  const UINT *pUAVInitialCounts) override;
  void STDMETHODCALLTYPE OMSetBlendState(ID3D11BlendState *pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override;
  void STDMETHODCALLTYPE OMSetDepthStencilState(ID3D11DepthStencilState *pDepthStencilState, UINT StencilRef) override;

  void STDMETHODCALLTYPE ClearRenderTargetView(ID3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4]) override;
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *pUnorderedAccessView, const UINT Values[4]) override;
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *pUnorderedAccessView, const FLOAT Values[4]) override;
  void STDMETHODCALLTYPE ClearDepthStencilView(ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) override;
  void STDMETHODCALLTYPE GenerateMips(ID3D11ShaderResourceView *pShaderResourceView) override;
  void STDMETHODCALLTYPE ResolveSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, ID3D11Resource *pSrcResource, UINT SrcSubresource, DXGI_FORMAT Format) override;
  void STDMETHODCALLTYPE CopyResource(ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource) override;
  void STDMETHODCALLTYPE CopySubresourceRegion(ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource,
                                               const D3D11_BOX *pSrcBox) override;
  void STDMETHODCALLTYPE UpdateSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch) override;

  void STDMETHODCALLTYPE RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT *pViewports) override;
  void STDMETHODCALLTYPE RSSetScissorRects(UINT NumRects, const D3D11_RECT *pRects) override;
  void STDMETHODCALLTYPE RSSetState(ID3D11RasterizerState *pRasterizerState) override;
  void STDMETHODCALLTYPE ClearState(void) override;
  void STDMETHODCALLTYPE Flush(void) override;

  HRESULT STDMETHODCALLTYPE Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource) override;
  void STDMETHODCALLTYPE Unmap(ID3D11Resource *pResource, UINT Subresource) override;
  void STDMETHODCALLTYPE Begin(ID3D11Asynchronous *pAsync) override;
  void STDMETHODCALLTYPE End(ID3D11Asynchronous *pAsync) override;
  HRESULT STDMETHODCALLTYPE GetData(ID3D11Asynchronous *pAsync, void *pData, UINT DataSize, UINT GetDataFlags) override;
  D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType(void) override;

  // ---- internal API used by the rest of the shim ----
  void EnsureDummies();
  void ResetState();
  void EndRendering();
  void EnsureRenderingStarted();
  void TransitionImage(TextureImpl *tex, VkImageLayout newLayout);
  bool PrepareDraw(VkPipeline *outPipeline, VkPipelineLayout *outLayout, std::vector<MergedBinding> *outBindings);
  // Moves every sampled image the draw reads into SHADER_READ_ONLY_OPTIMAL.
  // Must run *before* EnsureRenderingStarted(): image layout transitions are
  // illegal inside a dynamic rendering instance.
  void TransitionBindings(const std::vector<MergedBinding> &bindings);
  // Diagnostics for a descriptor write that would need a layout change while a
  // render pass is open (which TransitionBindings() prevents).
  void CheckSampledLayout(TextureImpl *tex);
  bool WriteDescriptorSet(VkDescriptorSet set, const std::vector<MergedBinding> &bindings, std::vector<uint32_t> &dynamicOffsets);
  bool BeginFrameIfNeeded();
  void SubmitAndWait();
  void OnSwapchainRecreated();
  void DestroyVulkanObjects();
  void ResetDescriptorPools();
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
  VkDescriptorSetLayout GetSetLayout(const std::vector<MergedBinding> &bindings);
  VkPipelineLayout GetPipelineLayout(VkDescriptorSetLayout setLayout, const std::vector<MergedBinding> &bindings);
  std::vector<MergedBinding> MergeBindings() const;
  bool ApplyGraphicsState(VkCommandBuffer cmd);
  bool DrawInternal(bool indexed, UINT count, UINT instanceCount, UINT startIndex, INT baseVertex, UINT startVertex, UINT startInstance);
};

// ===========================================================================
// shared conversion helpers (d3d11_convert.cpp)
// ===========================================================================

VkBlendFactor ToVkBlendFactor(D3D11_BLEND b);
VkBlendOp ToVkBlendOp(D3D11_BLEND_OP op);
VkCompareOp ToVkCompareOp(D3D11_COMPARISON_FUNC f);
VkStencilOp ToVkStencilOp(D3D11_STENCIL_OP op);
VkCullModeFlags ToVkCullMode(D3D11_CULL_MODE m);
VkPolygonMode ToVkPolygonMode(D3D11_FILL_MODE m);
VkPrimitiveTopology ToVkTopology(D3D11_PRIMITIVE_TOPOLOGY t, uint32_t *patchControlPoints);
VkSamplerAddressMode ToVkAddressMode(D3D11_TEXTURE_ADDRESS_MODE m);

TextureImpl *AsTexture(ID3D11Resource *res);
BufferImpl *AsBuffer(ID3D11Resource *res);
ID3D11Resource *ViewResource(const ViewImpl &v);

VkImageView CreateTextureView(TextureImpl *tex, VkFormat format, VkImageAspectFlags aspect, uint32_t baseMip, uint32_t mipCount, uint32_t baseLayer, uint32_t layerCount,
                              VkImageViewType type);

// Copies host memory into a device-local buffer through the frame command
// buffer (d3d11_resource.cpp).
void UploadBufferData(VkBuffer dst, uint64_t dstOffset, const void *data, uint64_t size);
// Uploads one (mip, layer) of a texture from host memory (d3d11_resource.cpp).
void UpdateTextureSubresource(TextureImpl *tex, uint32_t mip, uint32_t layer, const void *data, uint32_t rowPitch, uint32_t depthPitch);
// Maps a range of host-visible memory (d3d11_resource.cpp).
void *MapStagingMemoryRange(VkDeviceMemory memory, uint64_t offset, uint64_t size);

} // namespace phxd3d11

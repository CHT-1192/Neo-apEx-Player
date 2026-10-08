// Shader objects and input layouts.
//
// D3DCompile hands us SPIR-V; CreateVertexShader()/CreatePixelShader()/... turn
// it into a VkShaderModule and recover the reflection that D3DCompile stashed
// for it.
#include "d3d11_internal.h"

#include "dxgi_formats.h"

#include <cstdio>

namespace phxd3d11
{

namespace
{

static const uint32_t kSpirvMagic = 0x07230203;
static const uint32_t kOpEntryPoint = 15;

// Reads the (single) entry point's name out of a SPIR-V module.
std::string SpirvEntryPointName(const uint32_t *words, size_t wordCount)
{
  size_t i = 5; // skip the 5-word header
  while (i < wordCount)
  {
    const uint32_t word = words[i];
    const uint32_t wordCountOfInst = word >> 16;
    const uint32_t opcode = word & 0xffff;
    if (wordCountOfInst == 0) break;
    if (opcode == kOpEntryPoint && i + 3 < wordCount)
    {
      // OpEntryPoint %executionModel %id "name"
      return std::string(reinterpret_cast<const char *>(&words[i + 3]));
    }
    i += wordCountOfInst;
  }
  return "main";
}

// Creates a VkShaderModule (and recovers the reflection) for one SPIR-V blob.
bool BuildShaderCommon(const void *bytecode, SIZE_T length, phxvk::ShaderStage stage, bool geometryRequested, ShaderCommon *out, const char *what)
{
  if (!bytecode || length < sizeof(uint32_t) * 5)
  {
    phxvk::LogError("%s: bytecode is too small to be SPIR-V", what);
    return false;
  }
  const uint32_t *words = (const uint32_t *)bytecode;
  if (words[0] != kSpirvMagic)
  {
    phxvk::LogError("%s: bytecode is not SPIR-V (magic 0x%08x); DXBC is not supported by this port", what, words[0]);
    return false;
  }

  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = (size_t)length;
  ci.pCode = words;
  VkResult res = vkCreateShaderModule(phxvk::Vk().device, &ci, nullptr, &out->module);
  if (res != VK_SUCCESS)
  {
    phxvk::LogError("%s: vkCreateShaderModule failed: %s", what, phxvk::VkResultName(res));
    return false;
  }

  out->stage = stage;
  out->spirv.assign(words, words + length / sizeof(uint32_t));
  out->entryPoint = SpirvEntryPointName(words, out->spirv.size());
  // Identity for the pipeline cache: identical bytecode must map to one
  // pipeline even though the engine keeps minting new shader objects.
  {
    uint64_t h = PhxHashBytes(out->spirv.data(), out->spirv.size() * sizeof(uint32_t));
    h = PhxHashCombine(h, (uint64_t)stage);
    h = PhxHashCombine(h, PhxHashBytes(out->entryPoint.data(), out->entryPoint.size()));
    out->contentHash = h ? h : 1; // 0 is reserved for "no shader"
  }

  ShaderReflectionRecord rec;
  if (LookupShaderReflection(bytecode, (size_t)length, &rec))
    out->reflection = rec.reflection;
  else
    phxvk::LogWarn("%s: no reflection registered for this module; anything bound to it will use dummy descriptors", what);

  if (geometryRequested && phxvk::Vk().features.geometryShader == VK_FALSE && OnceGsUnsupported()())
    phxvk::LogWarn("[d3d11] VkPhysicalDeviceFeatures::geometryShader is false (MoltenVK); geometry shaders are created but their stage is skipped at "
                   "pipeline creation so the demo still renders");

  return true;
}

template <class T, class Iface>
HRESULT CreateShaderObject(DeviceImpl *device, const void *bytecode, SIZE_T length, phxvk::ShaderStage stage, bool geometryRequested, Iface **out, const char *what)
{
  if (!out) return E_POINTER;
  *out = nullptr;
  auto *s = new T();
  s->device = device;
  if (!BuildShaderCommon(bytecode, length, stage, geometryRequested, s, what))
  {
    delete s;
    return E_FAIL;
  }
  TrackObjectCreated(what + 7 /* skip "Create" */, s);
  *out = s;
  return S_OK;
}

} // namespace

//////////////////////////////////////////////////////////////////////////////
// shader objects

#define PHX_SHADER_IMPL(Class, Iface)                                                              \
  Class::~Class()                                                                                  \
  {                                                                                                \
    if (module != VK_NULL_HANDLE && phxvk::Vk().device) vkDestroyShaderModule(phxvk::Vk().device, module, nullptr); \
    module = VK_NULL_HANDLE;                                                                       \
    TrackObjectDestroyed(#Class, this);                                                            \
  }                                                                                                \
  void Class::GetDevice(ID3D11Device **ppDevice)                                                   \
  {                                                                                                \
    if (ppDevice)                                                                                  \
    {                                                                                              \
      *ppDevice = device;                                                                          \
      if (device) device->AddRefImpl();                                                            \
    }                                                                                              \
  }                                                                                                \
  HRESULT Class::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); } \
  HRESULT Class::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); } \
  HRESULT Class::QueryInterface(REFIID riid, void **ppvObject)                                     \
  {                                                                                                \
    if (!ppvObject) return E_POINTER;                                                              \
    *ppvObject = nullptr;                                                                          \
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(Iface))) \
    {                                                                                              \
      *ppvObject = static_cast<Iface *>(this);                                                      \
      AddRefImpl();                                                                                \
      return S_OK;                                                                                 \
    }                                                                                              \
    return E_NOINTERFACE;                                                                          \
  }

PHX_SHADER_IMPL(VertexShaderImpl, ID3D11VertexShader);
PHX_SHADER_IMPL(PixelShaderImpl, ID3D11PixelShader);
PHX_SHADER_IMPL(GeometryShaderImpl, ID3D11GeometryShader);
PHX_SHADER_IMPL(HullShaderImpl, ID3D11HullShader);
PHX_SHADER_IMPL(DomainShaderImpl, ID3D11DomainShader);
PHX_SHADER_IMPL(ComputeShaderImpl, ID3D11ComputeShader);
#undef PHX_SHADER_IMPL

//////////////////////////////////////////////////////////////////////////////
// input layout

InputLayoutImpl::~InputLayoutImpl() { TrackObjectDestroyed("ID3D11InputLayout", this); }

void InputLayoutImpl::GetDevice(ID3D11Device **ppDevice)
{
  if (ppDevice)
  {
    *ppDevice = device;
    if (device) device->AddRefImpl();
  }
}
HRESULT InputLayoutImpl::GetPrivateData(REFGUID g, UINT *s, void *d) { return GetPrivateDataImpl(g, s, d); }
HRESULT InputLayoutImpl::SetPrivateData(REFGUID g, UINT s, const void *d) { return SetPrivateDataImpl(g, s, d); }
HRESULT InputLayoutImpl::SetPrivateDataInterface(REFGUID g, const IUnknown *p) { return SetPrivateDataInterfaceImpl(g, p); }
HRESULT InputLayoutImpl::QueryInterface(REFIID riid, void **ppvObject)
{
  if (!ppvObject) return E_POINTER;
  *ppvObject = nullptr;
  if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11DeviceChild)) || GuidEq(riid, __uuidof(ID3D11InputLayout)))
  {
    *ppvObject = static_cast<ID3D11InputLayout *>(this);
    AddRefImpl();
    return S_OK;
  }
  return E_NOINTERFACE;
}

//////////////////////////////////////////////////////////////////////////////
// device-side creation

HRESULT DeviceImpl::CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *, ID3D11VertexShader **ppVertexShader)
{
  return CreateShaderObject<VertexShaderImpl>(this, pShaderBytecode, BytecodeLength, phxvk::ShaderStage::Vertex, false, ppVertexShader, "CreateVertexShader");
}

HRESULT DeviceImpl::CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *, ID3D11PixelShader **ppPixelShader)
{
  return CreateShaderObject<PixelShaderImpl>(this, pShaderBytecode, BytecodeLength, phxvk::ShaderStage::Pixel, false, ppPixelShader, "CreatePixelShader");
}

HRESULT DeviceImpl::CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *, ID3D11GeometryShader **ppGeometryShader)
{
  return CreateShaderObject<GeometryShaderImpl>(this, pShaderBytecode, BytecodeLength, phxvk::ShaderStage::Geometry, true, ppGeometryShader, "CreateGeometryShader");
}

HRESULT DeviceImpl::CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *, ID3D11HullShader **ppHullShader)
{
  return CreateShaderObject<HullShaderImpl>(this, pShaderBytecode, BytecodeLength, phxvk::ShaderStage::Hull, false, ppHullShader, "CreateHullShader");
}

HRESULT DeviceImpl::CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *, ID3D11DomainShader **ppDomainShader)
{
  return CreateShaderObject<DomainShaderImpl>(this, pShaderBytecode, BytecodeLength, phxvk::ShaderStage::Domain, false, ppDomainShader, "CreateDomainShader");
}

HRESULT DeviceImpl::CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *, ID3D11ComputeShader **ppComputeShader)
{
  return CreateShaderObject<ComputeShaderImpl>(this, pShaderBytecode, BytecodeLength, phxvk::ShaderStage::Compute, false, ppComputeShader, "CreateComputeShader");
}

HRESULT DeviceImpl::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements, const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength,
                                      ID3D11InputLayout **ppInputLayout)
{
  if (!ppInputLayout) return E_POINTER;
  *ppInputLayout = nullptr;
  if (!pInputElementDescs || !NumElements || !pShaderBytecodeWithInputSignature) return E_INVALIDARG;

  auto *layout = new InputLayoutImpl();
  layout->device = this;
  TrackObjectCreated("ID3D11InputLayout", layout);

  layout->elements.assign(pInputElementDescs, pInputElementDescs + NumElements);
  layout->names.reserve(NumElements);
  for (UINT i = 0; i < NumElements; i++)
  {
    layout->names.push_back(pInputElementDescs[i].SemanticName ? pInputElementDescs[i].SemanticName : "");
    layout->elements[i].SemanticName = layout->names.back().c_str();
  }

  // Identity for the pipeline cache (see PipelineKey): element fields plus the
  // semantic names, which live outside the element struct.
  {
    uint64_t h = 0x4004;
    for (UINT i = 0; i < NumElements; i++)
    {
      h = PhxHashCombine(h, (uint64_t)layout->elements[i].SemanticIndex);
      h = PhxHashCombine(h, (uint64_t)layout->elements[i].Format);
      h = PhxHashCombine(h, (uint64_t)layout->elements[i].InputSlot);
      h = PhxHashCombine(h, (uint64_t)layout->elements[i].AlignedByteOffset);
      h = PhxHashCombine(h, (uint64_t)layout->elements[i].InputSlotClass);
      h = PhxHashCombine(h, (uint64_t)layout->elements[i].InstanceDataStepRate);
      h = PhxHashCombine(h, PhxHashBytes(layout->names[i].data(), layout->names[i].size()));
    }
    layout->contentHash = h ? h : 1;
  }

  // The reflection of the vertex shader tells us which locations the SPIR-V
  // declares; see docs/DESIGN-d3d11-shim.md section 4.  Location N is paired
  // with D3D11_INPUT_ELEMENT_DESC[N] (glslang assigns locations by declaration
  // index and does not compact unused members), so the reflection is only used
  // to warn when the element array does not cover every declared location.
  ShaderReflectionRecord rec;
  if (LookupShaderReflection(pShaderBytecodeWithInputSignature, (size_t)BytecodeLength, &rec))
  {
    layout->vertexReflection = rec.reflection;
    layout->reflectionValid = true;
    UINT highest = 0;
    for (auto &in : rec.reflection.inputs)
      if (in.location + 1 > highest) highest = in.location + 1;
    if (highest > NumElements)
      phxvk::LogWarn("CreateInputLayout: the vertex shader declares input location %u but only %u element(s) were supplied; those inputs will read "
                     "undefined vertex data",
                     highest - 1, NumElements);
  }
  else
  {
    phxvk::LogWarn("CreateInputLayout: no reflection available for the vertex shader; input elements are used as supplied");
  }

  *ppInputLayout = layout;
  return S_OK;
}

} // namespace phxd3d11

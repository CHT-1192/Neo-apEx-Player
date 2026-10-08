// ID3D11ShaderReflection and its constant-buffer/variable/type sub-objects,
// backed by the phxvk::Reflection that the HLSL front end produced.
//
// The sub-objects (ID3D11ShaderReflectionType / Variable / ConstantBuffer) do
// not derive from IUnknown in the SDK and are owned by the reflection object,
// so they are plain children here as well.
#include "d3d11_internal.h"

#include <D3Dcompiler.h>

#include <cstdio>

namespace phxd3d11
{

// ===========================================================================
// types
// ===========================================================================

namespace
{

// A reflection type node.  glslang's SPIR-V reflection gives us struct sizes
// and member offsets, which is what the engine's tool code actually reads.
struct TypeImpl : public ID3D11ShaderReflectionType
{
  struct Member
  {
    std::string name;
    std::unique_ptr<TypeImpl> type;
  };

  D3D_SHADER_VARIABLE_CLASS varClass = D3D_SVC_SCALAR;
  D3D_SHADER_VARIABLE_TYPE varType = D3D_SVT_VOID;
  UINT rows = 1;
  UINT columns = 1;
  UINT elements = 0;
  UINT offset = 0;
  UINT size = 0;
  std::string name;
  std::vector<Member> members;

  HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_TYPE_DESC *pDesc) override
  {
    if (!pDesc) return E_INVALIDARG;
    memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Class = varClass;
    pDesc->Type = varType;
    pDesc->Rows = rows;
    pDesc->Columns = columns;
    pDesc->Elements = elements;
    pDesc->Members = (UINT)members.size();
    pDesc->Offset = offset;
    pDesc->Name = name.empty() ? nullptr : name.c_str();
    return S_OK;
  }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetMemberTypeByIndex(UINT Index) override
  {
    return Index < members.size() ? members[Index].type.get() : nullptr;
  }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetMemberTypeByName(LPCSTR Name) override
  {
    if (!Name) return nullptr;
    for (auto &m : members)
      if (m.name == Name) return m.type.get();
    return nullptr;
  }
  LPCSTR STDMETHODCALLTYPE GetMemberTypeName(UINT Index) override { return Index < members.size() ? members[Index].name.c_str() : nullptr; }
  HRESULT STDMETHODCALLTYPE IsEqual(ID3D11ShaderReflectionType *pType) override { return pType == this ? S_OK : S_FALSE; }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetSubType(void) override { return nullptr; }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetBaseClass(void) override { return nullptr; }
  UINT STDMETHODCALLTYPE GetNumInterfaces(void) override { return 0; }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetInterfaceByIndex(UINT) override { return nullptr; }
  HRESULT STDMETHODCALLTYPE IsOfType(ID3D11ShaderReflectionType *pType) override { return pType == this ? S_OK : S_FALSE; }
  HRESULT STDMETHODCALLTYPE ImplementsInterface(ID3D11ShaderReflectionType *) override { return S_FALSE; }
};

struct VariableImpl : public ID3D11ShaderReflectionVariable
{
  std::string name;
  UINT startOffset = 0;
  UINT size = 0;
  ID3D11ShaderReflectionConstantBuffer *buffer = nullptr;
  std::unique_ptr<TypeImpl> type;

  HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_VARIABLE_DESC *pDesc) override
  {
    if (!pDesc) return E_INVALIDARG;
    memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Name = name.c_str();
    pDesc->StartOffset = startOffset;
    pDesc->Size = size;
    pDesc->uFlags = D3D_SVF_USED;
    pDesc->DefaultValue = nullptr;
    return S_OK;
  }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetType(void) override { return type.get(); }
  ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE GetBuffer(void) override { return buffer; }
  UINT STDMETHODCALLTYPE GetInterfaceSlot(UINT) override { return 0; }
};

struct ConstantBufferImpl : public ID3D11ShaderReflectionConstantBuffer
{
  std::string name;
  UINT size = 0;
  D3D_CBUFFER_TYPE cbType = D3D_CT_CBUFFER;
  std::vector<std::unique_ptr<VariableImpl>> variables;
  std::unique_ptr<TypeImpl> type;

  HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_BUFFER_DESC *pDesc) override
  {
    if (!pDesc) return E_INVALIDARG;
    memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Name = name.c_str();
    pDesc->Type = cbType;
    pDesc->Variables = (UINT)variables.size();
    pDesc->Size = size;
    pDesc->uFlags = 0;
    return S_OK;
  }
  ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByIndex(UINT Index) override
  {
    return Index < variables.size() ? variables[Index].get() : nullptr;
  }
  ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) override
  {
    if (!Name) return nullptr;
    for (auto &v : variables)
      if (v->name == Name) return v.get();
    return nullptr;
  }
  ID3D11ShaderReflectionType *STDMETHODCALLTYPE GetType(void) override { return type.get(); }
};

// Maps a phxvk descriptor to the D3D-style input type/dimension the tooling
// expects.
static D3D_SHADER_INPUT_TYPE InputTypeFor(const phxvk::DescriptorBinding &b)
{
  switch (b.regClass)
  {
  case phxvk::RegisterClass::ConstantBuffer: return D3D_SIT_CBUFFER;
  case phxvk::RegisterClass::Texture:
    if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) return D3D_SIT_STRUCTURED;
    return D3D_SIT_TEXTURE;
  case phxvk::RegisterClass::Sampler: return D3D_SIT_SAMPLER;
  case phxvk::RegisterClass::UAV:
    if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) return D3D_SIT_UAV_RWSTRUCTURED;
    return D3D_SIT_UAV_RWTYPED;
  default: return D3D_SIT_TEXTURE;
  }
}

static D3D_SRV_DIMENSION DimensionFor(const phxvk::DescriptorBinding &b)
{
  if (b.regClass == phxvk::RegisterClass::ConstantBuffer) return D3D_SRV_DIMENSION_BUFFER;
  if (b.regClass == phxvk::RegisterClass::Sampler) return D3D_SRV_DIMENSION_UNKNOWN;
  if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) return D3D_SRV_DIMENSION_BUFFER;
  return D3D_SRV_DIMENSION_TEXTURE2D;
}

class ReflectionImpl : public ID3D11ShaderReflection, public ObjectBase
{
public:
  ReflectionImpl(const ShaderReflectionRecord &rec, std::string dbg)
  {
    m_debugName = std::move(dbg);
    m_stage = rec.stage;

    // Group the descriptors the way D3D presents them: constant buffers by
    // bind point, then every other resource as a bound resource.
    for (auto &b : rec.reflection.bindings)
    {
      if (b.regClass == phxvk::RegisterClass::ConstantBuffer && b.type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
      {
        m_buffers.push_back(MakeConstantBuffer(b));
      }
      else
      {
        m_resources.push_back(b);
      }
    }

    for (auto &in : rec.reflection.inputs)
    {
      SemanticName name;
      name.text = in.name.empty() ? "TEXCOORD" : in.name;
      m_semanticNames.push_back(std::move(name));
    }
    (void)m_stage;
  }

  // -- ID3D11ShaderReflection
  HRESULT STDMETHODCALLTYPE GetDesc(D3D11_SHADER_DESC *pDesc) override
  {
    if (!pDesc) return E_INVALIDARG;
    memset(pDesc, 0, sizeof(*pDesc));
    pDesc->Version = 0x0500; // ps_5_0/vs_5_0 style; we run SPIR-V
    pDesc->Creator = "phxvk:libshaderc";
    pDesc->Flags = 0;
    pDesc->ConstantBuffers = (UINT)m_buffers.size();
    pDesc->BoundResources = (UINT)m_resources.size();
    pDesc->InputParameters = (UINT)m_semanticNames.size();
    pDesc->OutputParameters = 0;
    pDesc->InstructionCount = 0;
    pDesc->GSOutputTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    return S_OK;
  }
  ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE GetConstantBufferByIndex(UINT Index) override
  {
    return Index < m_buffers.size() ? m_buffers[Index].get() : nullptr;
  }
  ID3D11ShaderReflectionConstantBuffer *STDMETHODCALLTYPE GetConstantBufferByName(LPCSTR Name) override
  {
    if (!Name) return nullptr;
    for (auto &b : m_buffers)
      if (b->name == Name) return b.get();
    return nullptr;
  }
  HRESULT STDMETHODCALLTYPE GetResourceBindingDesc(UINT ResourceIndex, D3D11_SHADER_INPUT_BIND_DESC *pDesc) override
  {
    if (!pDesc || ResourceIndex >= m_resources.size()) return E_INVALIDARG;
    const auto &b = m_resources[ResourceIndex];
    memset(pDesc, 0, sizeof(*pDesc));
    m_bindNames.push_back(b.name.empty() ? std::string("resource") : b.name);
    pDesc->Name = m_bindNames.back().c_str();
    pDesc->Type = InputTypeFor(b);
    pDesc->BindPoint = b.regIndex;
    pDesc->BindCount = b.arraySize;
    pDesc->uFlags = 0;
    pDesc->ReturnType = D3D_RETURN_TYPE_MIXED;
    pDesc->Dimension = DimensionFor(b);
    pDesc->NumSamples = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetInputParameterDesc(UINT ParameterIndex, D3D11_SIGNATURE_PARAMETER_DESC *pDesc) override
  {
    if (!pDesc || ParameterIndex >= m_semanticNames.size()) return E_INVALIDARG;
    const auto &in = m_semanticNames[ParameterIndex];
    memset(pDesc, 0, sizeof(*pDesc));
    pDesc->SemanticName = in.text.c_str();
    pDesc->SemanticIndex = 0;
    pDesc->Register = ParameterIndex;
    pDesc->SystemValueType = D3D_NAME_UNDEFINED;
    pDesc->ComponentType = D3D_REGISTER_COMPONENT_FLOAT32;
    pDesc->Mask = 0xf;
    pDesc->ReadWriteMask = 0xf;
    pDesc->Stream = 0;
    pDesc->MinPrecision = D3D_MIN_PRECISION_DEFAULT;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetOutputParameterDesc(UINT, D3D11_SIGNATURE_PARAMETER_DESC *) override { return E_INVALIDARG; }
  HRESULT STDMETHODCALLTYPE GetPatchConstantParameterDesc(UINT, D3D11_SIGNATURE_PARAMETER_DESC *) override { return E_INVALIDARG; }
  ID3D11ShaderReflectionVariable *STDMETHODCALLTYPE GetVariableByName(LPCSTR Name) override
  {
    if (!Name) return nullptr;
    for (auto &b : m_buffers)
      if (auto *v = b->GetVariableByName(Name)) return v;
    return nullptr;
  }
  HRESULT STDMETHODCALLTYPE GetResourceBindingDescByName(LPCSTR Name, D3D11_SHADER_INPUT_BIND_DESC *pDesc) override
  {
    if (!Name) return E_INVALIDARG;
    for (UINT i = 0; i < m_resources.size(); i++)
      if (m_resources[i].name == Name) return GetResourceBindingDesc(i, pDesc);
    return E_INVALIDARG;
  }
  UINT STDMETHODCALLTYPE GetMovInstructionCount(void) override { return 0; }
  UINT STDMETHODCALLTYPE GetMovcInstructionCount(void) override { return 0; }
  UINT STDMETHODCALLTYPE GetConversionInstructionCount(void) override { return 0; }
  UINT STDMETHODCALLTYPE GetBitwiseInstructionCount(void) override { return 0; }
  D3D_PRIMITIVE STDMETHODCALLTYPE GetGSInputPrimitive(void) override { return D3D_PRIMITIVE_UNDEFINED; }
  BOOL STDMETHODCALLTYPE IsSampleFrequencyShader(void) override { return FALSE; }
  UINT STDMETHODCALLTYPE GetNumInterfaceSlots(void) override { return 0; }

  // -- IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
  {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D11ShaderReflection)))
    {
      *ppvObject = static_cast<ID3D11ShaderReflection *>(this);
      AddRefImpl();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }

private:
  struct SemanticName
  {
    std::string text;
  };

  std::unique_ptr<ConstantBufferImpl> MakeConstantBuffer(const phxvk::DescriptorBinding &b)
  {
    auto cb = std::make_unique<ConstantBufferImpl>();
    cb->name = b.name.empty() ? "cb" + std::to_string(b.regIndex) : b.name;
    cb->size = b.blockSize ? b.blockSize : 0;
    cb->cbType = D3D_CT_CBUFFER;
    cb->type = std::make_unique<TypeImpl>();
    cb->type->varClass = D3D_SVC_STRUCT;
    cb->type->varType = D3D_SVT_VOID;
    return cb;
  }

  std::string m_debugName;
  phxvk::ShaderStage m_stage;
  std::vector<std::unique_ptr<ConstantBufferImpl>> m_buffers;
  std::vector<phxvk::DescriptorBinding> m_resources;
  std::vector<SemanticName> m_semanticNames;
  std::vector<std::string> m_bindNames;
};

} // namespace

ID3D11ShaderReflection *CreateReflection(const ShaderReflectionRecord &rec, const char *debugName)
{
  auto *r = new ReflectionImpl(rec, debugName ? debugName : "");
  TrackObjectCreated("ID3D11ShaderReflection", r);
  return static_cast<ID3D11ShaderReflection *>(r);
}

} // namespace phxd3d11

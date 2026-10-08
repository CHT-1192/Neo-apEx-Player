// ID3D10Blob/ID3DBlob plus the small process-wide tables the shim needs:
// the SPIR-V -> reflection map and the live object list used by ID3D11Debug.
#include "d3d11_internal.h"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace phxd3d11
{

namespace
{

struct BlobImpl : public ID3D10Blob, public ObjectBase
{
  std::vector<uint8_t> data;

  LPVOID STDMETHODCALLTYPE GetBufferPointer(void) override { return data.empty() ? nullptr : data.data(); }
  SIZE_T STDMETHODCALLTYPE GetBufferSize(void) override { return data.size(); }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
  {
    if (!ppvObject) return E_POINTER;
    *ppvObject = nullptr;
    if (GuidEq(riid, IID_IUnknownRef()) || GuidEq(riid, __uuidof(ID3D10Blob)) || GuidEq(riid, __uuidof(ID3D10Blob)))
    {
      *ppvObject = static_cast<ID3D10Blob *>(this);
      AddRefImpl();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef(void) override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release(void) override { return ReleaseImpl(); }
};

} // namespace

ID3DBlob *CreateBlob(const void *data, size_t size)
{
  auto *b = new BlobImpl();
  if (data && size) b->data.assign((const uint8_t *)data, (const uint8_t *)data + size);
  TrackObjectCreated("ID3D10Blob", b);
  return static_cast<ID3D10Blob *>(b);
}

ID3DBlob *CreateBlobString(const std::string &text)
{
  auto *b = new BlobImpl();
  b->data.assign(text.begin(), text.end());
  b->data.push_back(0); // callers hand GetBufferPointer() to printf/CString
  TrackObjectCreated("ID3D10Blob", b);
  return static_cast<ID3D10Blob *>(b);
}

//////////////////////////////////////////////////////////////////////////////
// SPIR-V -> reflection side table
//
// D3DCompile must put *raw SPIR-V* in the blob (that is the contract the rest
// of the port relies on), so the reflection glslang produced alongside it is
// kept here, keyed by a hash of the SPIR-V.  CreateVertexShader() and
// D3DReflect() look it up again.

namespace
{
std::mutex g_reflMutex;
std::unordered_map<uint64_t, ShaderReflectionRecord> g_reflections;

// Emulated vertex stage per geometry shader (see gs_emulation.h), keyed by the
// geometry shader's SPIR-V so pipeline creation can find it from the shader the
// engine bound.
std::mutex g_emuMutex;
std::unordered_map<uint64_t, EmulatedGeometryShader> g_emulatedGeometryShaders;

uint64_t HashBytes(const void *data, size_t size)
{
  const uint8_t *p = (const uint8_t *)data;
  uint64_t h = 1469598103934665603ull; // FNV-1a
  for (size_t i = 0; i < size; i++)
  {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
} // namespace

void RegisterEmulatedGeometryShader(const void *gsSpirv, size_t gsSize, const EmulatedGeometryShader &emu)
{
  if (!gsSpirv || !gsSize || emu.vertexSpirv.empty()) return;
  std::lock_guard<std::mutex> lock(g_emuMutex);
  g_emulatedGeometryShaders[HashBytes(gsSpirv, gsSize)] = emu;
}

bool LookupEmulatedGeometryShader(const void *gsSpirv, size_t gsSize, EmulatedGeometryShader *out)
{
  if (!gsSpirv || !gsSize || !out) return false;
  std::lock_guard<std::mutex> lock(g_emuMutex);
  auto it = g_emulatedGeometryShaders.find(HashBytes(gsSpirv, gsSize));
  if (it == g_emulatedGeometryShaders.end()) return false;
  *out = it->second;
  return true;
}

void RegisterShaderReflection(const void *spirv, size_t size, const ShaderReflectionRecord &rec)
{
  if (!spirv || !size) return;
  std::lock_guard<std::mutex> lock(g_reflMutex);
  g_reflections[HashBytes(spirv, size)] = rec;
}

bool LookupShaderReflection(const void *spirv, size_t size, ShaderReflectionRecord *out)
{
  if (!spirv || !size) return false;
  std::lock_guard<std::mutex> lock(g_reflMutex);
  auto it = g_reflections.find(HashBytes(spirv, size));
  if (it == g_reflections.end()) return false;
  if (out) *out = it->second;
  return true;
}

//////////////////////////////////////////////////////////////////////////////
// live object tracking (ID3D11Debug::ReportLiveDeviceObjects)

namespace
{
struct LiveObject
{
  const char *kind;
  void *object;
};
std::mutex g_liveMutex;
std::vector<LiveObject> g_live;
} // namespace

void TrackObjectCreated(const char *kind, void *object)
{
  std::lock_guard<std::mutex> lock(g_liveMutex);
  g_live.push_back({kind, object});
}

void TrackObjectDestroyed(const char *kind, void *object)
{
  std::lock_guard<std::mutex> lock(g_liveMutex);
  for (auto it = g_live.begin(); it != g_live.end(); ++it)
  {
    if (it->object == object)
    {
      g_live.erase(it);
      return;
    }
  }
  (void)kind;
}

void ReportLiveObjects(bool detail)
{
  std::lock_guard<std::mutex> lock(g_liveMutex);
  if (g_live.empty())
  {
    phxvk::LogInfo("[d3d11] no live device objects");
    return;
  }
  phxvk::LogWarn("[d3d11] %zu live device object(s):", g_live.size());
  if (!detail) return;
  for (auto &o : g_live) phxvk::LogWarn("    %s %p", o.kind, o.object);
}

} // namespace phxd3d11
